#include "system_runtime.h"
#include <stdio.h>
#include <string.h>
#include "mqtt.h"
#include "mqtt_protocol_core.h"
#include "mqtt_protocol_codec.h"
#include "ota_ctr.h"
#include "formula_store.h"
#include "device_statistics_store.h"
#include "drink_record_service.h"
#include "extraction_curve_service.h"
#include "mqtt_protocol.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_context.h"
#include "esp_timer.h"

static const char *TAG = "MQTT_PROTOCOL";

#define MQTT_INTERNAL_TEST_SUB_TOPIC "controlserver/kalerm/iot/internalTest/result/%s"
#define MQTT_INTERNAL_TEST_SWITCH_DELAY_MS 800
#define MQTT_REMOTE_PLAYER_RECORD_ID_MIN 50000U

typedef enum {
    MQTT_ACK_TOPIC_REMOTE_CTRL = 0,
    MQTT_ACK_TOPIC_INTERNAL_TEST,
    MQTT_ACK_TOPIC_CLEANING,
} mqtt_ack_topic_t;

typedef struct {
    int ack_code;
    char msg[128];
    ack_sensor_info_t sensors[4];
    int sensors_count;
    int results[4];
    int results_count;
    mqtt_ack_topic_t ack_topic;
} mqtt_command_ack_t;

typedef struct {
    bool pending;
    int setting_type;
    char msg_id[64];
    bool machine_started;
    int64_t idle_since_ms;
} mqtt_remote_completion_ack_t;

typedef struct {
    bool pending;
    int cleaning_mode;
    char msg_id[64];
    bool machine_started;
    bool failed;
    int64_t idle_since_ms;
    uint32_t last_results_mask;
} mqtt_remote_cleaning_ack_t;

typedef enum {
    FORMULA_CMD_SRC_INVALID = 0,
    FORMULA_CMD_SRC_INLINE,
    FORMULA_CMD_SRC_LOCAL_STORE,
} formula_cmd_source_t;

extern FLASH_FACTORY_DATA factory_data;

static mqtt_remote_completion_ack_t s_remote_controller_completion_acks[2] = {0};
static mqtt_remote_cleaning_ack_t s_remote_cleaning_ack = {0};

static bool mqtt_cmd_no_equals(const char *cmd_no, const char *expected);
static mqtt_remote_completion_ack_t *mqtt_find_remote_completion_slot(int setting_type);
static mqtt_remote_completion_ack_t *mqtt_alloc_remote_completion_slot(int setting_type);
static int64_t mqtt_remote_ack_now_ms(void);
static bool mqtt_remote_maint_active(const MACHINE_STATUS *status);

static void mqtt_publish_followup_status_after_control(int cmd_type,
                                                       const char *cmd_no,
                                                       const mqtt_command_ack_t *command_ack)
{
    if (!command_ack || command_ack->ack_code != 0) {
        return;
    }

    if (command_ack->ack_topic == MQTT_ACK_TOPIC_CLEANING) {
        ESP_LOGI(TAG, "[MQTT][REMOTE] immediate queued sensors status for accepted cleaning task");
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                    "remote_clean_start",
                                                    0U);
        return;
    }

    if (command_ack->ack_topic != MQTT_ACK_TOPIC_REMOTE_CTRL) {
        return;
    }

    if (cmd_type == CMD_TYPE_STATUS) {
        ESP_LOGI(TAG, "[MQTT][REMOTE] immediate-priority full sensors status for cmd_type=%d cmd_no=%s",
                 cmd_type,
                 cmd_no ? cmd_no : "<null>");
        mqtt_log_memory_probe("remote_status_query_request");
        mqtt_request_immediate_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                             "remote_status_query");
        return;
    }

    if (cmd_type != CMD_TYPE_OTHER || cmd_no == NULL) {
        return;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_REMOTE_POWER_ON) ||
        mqtt_cmd_no_equals(cmd_no, CMD_NO_REMOTE_POWER_OFF) ||
        mqtt_cmd_no_equals(cmd_no, CMD_NO_CHILD_LOCK)) {
        ESP_LOGI(TAG, "[MQTT][REMOTE] immediate queued sensors status for cmd_no=%s", cmd_no);
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                    cmd_no,
                                                    0U);
        return;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_FACTORY_RESET)) {
        ESP_LOGI(TAG, "[MQTT][REMOTE] immediate queued sensors status for cmd_no=%s before split status sequence", cmd_no);
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                    "factory_reset_sensors",
                                                    0U);
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL,
                                                    "factory_reset_formula",
                                                    3000U);
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_CALIBRATION |
                                                        MQTT_DEVICE_STATUS_SECTION_STATISTICS |
                                                        MQTT_DEVICE_STATUS_SECTION_SETTING |
                                                        MQTT_DEVICE_STATUS_SECTION_WIFI,
                                                    "factory_reset_meta",
                                                    6000U);
        return;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_RESET_DRINKS)) {
        ESP_LOGI(TAG, "[MQTT][REMOTE] immediate queued formulaOverall status for cmd_no=%s", cmd_no);
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL,
                                                    "reset_drinks_formula",
                                                    0U);
        return;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_REMOTE_PARAM_SETTING)) {
        ESP_LOGI(TAG, "[MQTT][REMOTE] immediate queued setting status for cmd_no=%s", cmd_no);
        mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SETTING,
                                                    "remote_param_setting",
                                                    0U);
        return;
    }
}

static bool mqtt_cmd_no_equals(const char *cmd_no, const char *expected)
{
    return cmd_no != NULL && expected != NULL && strcmp(cmd_no, expected) == 0;
}

static int64_t mqtt_remote_ack_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static bool mqtt_remote_maint_active(const MACHINE_STATUS *status)
{
    app_state_t app_state = sp_pro_app_get_state();

    if (!status) {
        return false;
    }

    return (status->drink_making_flg == DRINK_MAKER_MAINTANCE) ||
           (status->hot_water_flg != 0U) ||
           (app_state == ST_CLEAN_BREW) ||
           (app_state == ST_MAINT_BREW) ||
           (app_state == ST_MAINT_DES) ||
           (app_state == ST_MAINT_STEAM) ||
           (app_state == ST_MAINT_DRAIN);
}

static void mqtt_set_command_ack(mqtt_command_ack_t *command_ack, int ack_code, const char *msg)
{
    if (!command_ack)
        return;

    command_ack->ack_code = ack_code;
    command_ack->sensors_count = 0;
    command_ack->results_count = 0;
    command_ack->ack_topic = MQTT_ACK_TOPIC_REMOTE_CTRL;
    snprintf(command_ack->msg, sizeof(command_ack->msg), "%s", msg ? msg : "");
}

static void mqtt_ensure_command_ack_failure(mqtt_command_ack_t *command_ack, const char *fallback_msg)
{
    if (!command_ack) {
        return;
    }

    if (command_ack->ack_code == 0) {
        mqtt_set_command_ack(command_ack, -1, fallback_msg);
        return;
    }

    if (command_ack->msg[0] == '\0' && fallback_msg) {
        snprintf(command_ack->msg, sizeof(command_ack->msg), "%s", fallback_msg);
    }
}

static mqtt_remote_completion_ack_t *mqtt_find_remote_completion_slot(int setting_type)
{
    for (size_t i = 0; i < sizeof(s_remote_controller_completion_acks) / sizeof(s_remote_controller_completion_acks[0]); ++i) {
        if (s_remote_controller_completion_acks[i].pending &&
            s_remote_controller_completion_acks[i].setting_type == setting_type) {
            return &s_remote_controller_completion_acks[i];
        }
    }

    return NULL;
}

static mqtt_remote_completion_ack_t *mqtt_alloc_remote_completion_slot(int setting_type)
{
    mqtt_remote_completion_ack_t *empty_slot = NULL;

    for (size_t i = 0; i < sizeof(s_remote_controller_completion_acks) / sizeof(s_remote_controller_completion_acks[0]); ++i) {
        if (s_remote_controller_completion_acks[i].pending &&
            s_remote_controller_completion_acks[i].setting_type == setting_type) {
            return &s_remote_controller_completion_acks[i];
        }

        if (!s_remote_controller_completion_acks[i].pending && empty_slot == NULL) {
            empty_slot = &s_remote_controller_completion_acks[i];
        }
    }

    return empty_slot;
}

void mqtt_track_remote_controller_completion(const char *msg_id, int setting_type)
{
    mqtt_remote_completion_ack_t *slot;

    if (!msg_id || msg_id[0] == 0) {
        return;
    }

    if (setting_type != SETTING_TYPE_CLEAR_WATERWAY &&
        setting_type != SETTING_TYPE_BREW_CLEAN) {
        return;
    }

    slot = mqtt_alloc_remote_completion_slot(setting_type);
    if (!slot) {
        ESP_LOGW(TAG,
                 "Drop remote controller completion tracking because no slot is available, settingType=%d msgId=%s",
                 setting_type,
                 msg_id);
        return;
    }

    slot->pending = true;
    slot->setting_type = setting_type;
    snprintf(slot->msg_id, sizeof(slot->msg_id), "%s", msg_id);
    ESP_LOGI(TAG,
             "Track remote controller completion settingType=%d msgId=%s",
             setting_type,
             slot->msg_id);
}

void mqtt_clear_remote_controller_completion(int setting_type)
{
    mqtt_remote_completion_ack_t *slot = mqtt_find_remote_completion_slot(setting_type);

    if (!slot) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
}

static void mqtt_add_ack_result(mqtt_command_ack_t *command_ack, int result)
{
    if (!command_ack || command_ack->results_count >= (int)(sizeof(command_ack->results) / sizeof(command_ack->results[0]))) {
        return;
    }

    command_ack->results[command_ack->results_count++] = result;
}

void mqtt_notify_remote_controller_complete(int setting_type, bool success)
{
    mqtt_remote_completion_ack_t *slot = mqtt_find_remote_completion_slot(setting_type);

    if (!slot) {
        return;
    }

    ESP_LOGI(TAG,
             "Publish remote controller completion ack settingType=%d msgId=%s success=%d",
             setting_type,
             slot->msg_id,
             success ? 1 : 0);
    publish_drink_ack_to_mqtt(slot->msg_id,
                              success ? 2 : -1,
                              NULL,
                              0,
                              success ? "complete" : "failed");
    memset(slot, 0, sizeof(*slot));
}

static void mqtt_track_remote_cleaning_ack(const char *msg_id, int cleaning_mode)
{
    if (!msg_id || msg_id[0] == 0) {
        return;
    }

    memset(&s_remote_cleaning_ack, 0, sizeof(s_remote_cleaning_ack));
    s_remote_cleaning_ack.pending = true;
    s_remote_cleaning_ack.cleaning_mode = cleaning_mode;
    snprintf(s_remote_cleaning_ack.msg_id, sizeof(s_remote_cleaning_ack.msg_id), "%s", msg_id);
    ESP_LOGI(TAG,
             "Track remote cleaning ack cleaningMode=%d msgId=%s",
             cleaning_mode,
             s_remote_cleaning_ack.msg_id);
}

static void mqtt_clear_remote_cleaning_ack(void)
{
    memset(&s_remote_cleaning_ack, 0, sizeof(s_remote_cleaning_ack));
}

void mqtt_handle_remote_maintenance_machine_status(const MACHINE_STATUS *status)
{
    int64_t now_ms;
    uint32_t abnormal_mask = 0U;
    int results[2];
    int results_count = 0;

    if (!status) {
        return;
    }

    now_ms = mqtt_remote_ack_now_ms();

    for (size_t i = 0; i < sizeof(s_remote_controller_completion_acks) / sizeof(s_remote_controller_completion_acks[0]); ++i) {
        mqtt_remote_completion_ack_t *slot = &s_remote_controller_completion_acks[i];

        if (!slot->pending) {
            continue;
        }

        if (mqtt_remote_maint_active(status)) {
            slot->machine_started = true;
            slot->idle_since_ms = 0;
            continue;
        }

        if (!slot->machine_started) {
            continue;
        }

        if (slot->idle_since_ms == 0) {
            slot->idle_since_ms = now_ms;
            continue;
        }

        if ((now_ms - slot->idle_since_ms) >= 3000LL) {
            mqtt_notify_remote_controller_complete(slot->setting_type, status->error_code == 0U);
        }
    }

    if (!s_remote_cleaning_ack.pending) {
        return;
    }

    if (status->water_box_shortage_flag) {
        abnormal_mask |= (1U << 1);
    }
    if (s_remote_cleaning_ack.cleaning_mode == CLEANING_MODE_BREW &&
        !status->brew_handle_postion_flag) {
        abnormal_mask |= (1U << 0);
    }

    if (abnormal_mask != s_remote_cleaning_ack.last_results_mask) {
        int progress_ack_code = s_remote_cleaning_ack.machine_started ? 1 : 0;

        if ((abnormal_mask & (1U << 0)) != 0U) {
            results[results_count++] = 1;
        }
        if ((abnormal_mask & (1U << 1)) != 0U) {
            results[results_count++] = 2;
        }
        publish_cleaning_ack_to_mqtt(s_remote_cleaning_ack.msg_id,
                                     progress_ack_code,
                                     results_count > 0 ? results : NULL,
                                     results_count,
                                     results_count > 0 ? "running" : "");
        s_remote_cleaning_ack.last_results_mask = abnormal_mask;
    }

    if (mqtt_remote_maint_active(status)) {
        s_remote_cleaning_ack.machine_started = true;
        s_remote_cleaning_ack.idle_since_ms = 0;
        if (status->error_code != 0U) {
            s_remote_cleaning_ack.failed = true;
        }
        return;
    }

    if (!s_remote_cleaning_ack.machine_started) {
        return;
    }

    if (status->error_code != 0U) {
        s_remote_cleaning_ack.failed = true;
    }

    if (s_remote_cleaning_ack.idle_since_ms == 0) {
        s_remote_cleaning_ack.idle_since_ms = now_ms;
        return;
    }

    if ((now_ms - s_remote_cleaning_ack.idle_since_ms) >= 3000LL) {
        int final_result = s_remote_cleaning_ack.failed ? 2 : 1;
        publish_cleaning_ack_to_mqtt(s_remote_cleaning_ack.msg_id,
                                     2,
                                     &final_result,
                                     1,
                                     s_remote_cleaning_ack.failed ? "failed" : "complete");
        mqtt_clear_remote_cleaning_ack();
    }
}

static void mqtt_add_ack_sensor(mqtt_command_ack_t *command_ack, const char *name, int status)
{
    ack_sensor_info_t *sensor;

    if (!command_ack || !name || command_ack->sensors_count >= (int)(sizeof(command_ack->sensors) / sizeof(command_ack->sensors[0]))) {
        return;
    }

    sensor = &command_ack->sensors[command_ack->sensors_count++];
    snprintf(sensor->name, sizeof(sensor->name), "%s", name);
    sensor->status = status;
}

static void dump_master_formula(formula_info_t *formula)
{
    ESP_LOGI(TAG, "========== MASTER FORMULA DUMP ==========");
    ESP_LOGI(TAG, "record_id: %lu", formula->record_id);
    ESP_LOGI(TAG, "formula_id: %lu", formula->formula_id);
    ESP_LOGI(TAG, "formula_name: %s", formula->formula_name);
    ESP_LOGI(TAG, "formula_remark: %s", formula->formula_remark);
    ESP_LOGI(TAG, "drink_id: %d", (int)formula->drink_id);
    ESP_LOGI(TAG, "drink_name: %s", formula->drink_name);
    ESP_LOGI(TAG, "label_id: %d", (int)formula->label_id);
    ESP_LOGI(TAG, "label: %s", formula->label);
    ESP_LOGI(TAG, "grind_range: %d", (int)formula->grind_range);
    ESP_LOGI(TAG, "grind_weight: %.1f", formula->grind_weight);
    ESP_LOGI(TAG, "preset_temperature: %d", (int)formula->preset_temperature);
    ESP_LOGI(TAG, "preset_liquid_weight: %d", (int)formula->preset_liquid_weight);
    ESP_LOGI(TAG, "water_temperature: %d", (int)formula->water_temperature);
    ESP_LOGI(TAG, "water_weight: %d", (int)formula->water_weight);
    ESP_LOGI(TAG, "milk_temperature: %d", (int)formula->milk_temperature);
    ESP_LOGI(TAG, "stage_priority: %d", (int)formula->stage_priority);
    ESP_LOGI(TAG, "prebrew: status=%u water=%u flow=%u time=%u",
             formula->prebrew.status,
             formula->prebrew.water_volume,
             formula->prebrew.flow_velocity,
             formula->prebrew.wait_time);
    ESP_LOGI(TAG, "pressure_stage_cnt: %u", formula->pressure_stage_cnt);
    for (int i = 0; i < formula->pressure_stage_cnt; i++) {
        ESP_LOGI(TAG, "pressure_stage[%d]: pressure=%.1f wait=%u",
                 i,
                 formula->pressure_stage[i].pressure,
                 formula->pressure_stage[i].wait_time);
    }
    ESP_LOGI(TAG, "velocity_stage_cnt: %u", formula->velocity_stage_cnt);
    for (int i = 0; i < formula->velocity_stage_cnt; i++) {
        ESP_LOGI(TAG, "velocity_stage[%d]: flow=%u wait=%u",
                 i,
                 formula->velocity_stage[i].flow_velocity,
                 formula->velocity_stage[i].wait_time);
    }
}

static bool mqtt_formula_param_has_inline_fields(cJSON *param)
{
    if (!param) {
        return false;
    }

    return cJSON_GetObjectItem(param, "preBrew") != NULL ||
           cJSON_GetObjectItem(param, "pressureStage") != NULL ||
           cJSON_GetObjectItem(param, "velocityStage") != NULL ||
           cJSON_GetObjectItem(param, "stagePriority") != NULL ||
           cJSON_GetObjectItem(param, "presetTemperature") != NULL ||
           cJSON_GetObjectItem(param, "presetLiquidWeight") != NULL ||
           cJSON_GetObjectItem(param, "waterTemperature") != NULL ||
           cJSON_GetObjectItem(param, "waterWeight") != NULL ||
           cJSON_GetObjectItem(param, "milkTemperature") != NULL ||
           cJSON_GetObjectItem(param, "grindRange") != NULL ||
           cJSON_GetObjectItem(param, "grindWeight") != NULL;
}

static formula_cmd_source_t mqtt_get_formula_cmd_source(cJSON *param)
{
    uint32_t record_id;

    if (!param) {
        return FORMULA_CMD_SRC_INVALID;
    }

    record_id = (uint32_t)json_get_int(param, "recordId", 0);
    if (record_id > 0U) {
        return FORMULA_CMD_SRC_LOCAL_STORE;
    }

    if (mqtt_formula_param_has_inline_fields(param)) {
        return FORMULA_CMD_SRC_INLINE;
    }

    return FORMULA_CMD_SRC_INVALID;
}

static bool mqtt_is_remote_player_formula_record_id(uint32_t record_id)
{
    return record_id > MQTT_REMOTE_PLAYER_RECORD_ID_MIN;
}

bool mqtt_start_drink_action(formula_info_t *formula)
{
    if (!formula)
        return false;

    ESP_LOGI(TAG, "[MQTT][DRINK] drink_id=%d record_id=%lu formula_id=%lu", formula->drink_id, formula->record_id, formula->formula_id);
    return sp_pro_app_start_remote_drink(formula);
}

static bool mqtt_start_grind_action(formula_info_t *formula)
{
    if (!formula)
        return false;

    sp_pro_app_set_ctrl_src(CTRL_SRC_MQTT);
    ESP_LOGI(TAG, "[MQTT][GRIND] record_id=%lu grind_range=%d grind_weight=%.1f", formula->record_id, formula->grind_range, formula->grind_weight);
    if (ctr_cmd_action(CTRL_ACT_GRIND_START, formula)) {
        device_statistics_notify_remote_action_start(CTRL_ACT_GRIND_START, formula);
        drink_record_notify_remote_action_start(CTRL_ACT_GRIND_START, formula);
        return true;
    }
    return false;
}

static bool parse_grind_cmd_from_mqtt(cJSON *param, mqtt_command_ack_t *command_ack)
{
    uint32_t record_id;
    formula_info_t formula = {0};

    if (!param) {
        mqtt_set_command_ack(command_ack, -1, "grind param required");
        return false;
    }

    record_id = (uint32_t)json_get_int(param, "recordId", 0);
    if (record_id == 0U) {
        mqtt_set_command_ack(command_ack, -1, "recordId required");
        return false;
    }

    if (!sp_pro_app_is_grind_handle_in_place()) {
        mqtt_set_command_ack(command_ack, -1, "grind handle not in place");
        mqtt_add_ack_sensor(command_ack, "grind_handle_postion_flag", 1);
        return false;
    }

    if (!formula_store_get_formula(0U, record_id, &formula)) {
        if (!mqtt_parse_formula_from_json(param, &formula) ||
            (formula.grind_weight <= 0.0f && formula.grind_range == 0U)) {
            mqtt_set_command_ack(command_ack, -1, "local grind formula not found");
            return false;
        }

        formula.record_id = record_id;
        if (formula.formula_id == 0U) {
            formula.formula_id = record_id;
        }
        if (formula.drink_id == 0U) {
            formula.drink_id = DRINK_ID_MASTER;
        }

        ESP_LOGI(TAG,
                 "[MQTT][GRIND] fallback to inline grind params for record_id=%lu grind_range=%u grind_weight=%.1f",
                 (unsigned long)formula.record_id,
                 (unsigned int)formula.grind_range,
                 formula.grind_weight);
    }

    return mqtt_start_grind_action(&formula);
}

static bool mqtt_try_run_action(control_action_t action, void *param)
{
    sp_pro_app_set_ctrl_src(CTRL_SRC_MQTT);
    return ctr_cmd_action(action, param);
}

#if MQTT_INTERNAL_TEST_ENABLE
static bool mqtt_copy_json_string_if_present(cJSON *param, const char *key, char *dst, size_t size)
{
    cJSON *item = NULL;

    if (!param || !key || !dst || size == 0U) {
        return false;
    }

    item = cJSON_GetObjectItem(param, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, size, "%s", item->valuestring);
        return true;
    }

    return false;
}

static void mqtt_internal_test_fill_topic_defaults(mqtt_config_t *config)
{
    if (!config) {
        return;
    }

    if (config->qos[0] == 0) {
        snprintf(config->qos, sizeof(config->qos), "%s", mqtt_config.qos[0] ? mqtt_config.qos : "0");
    }

    if (config->keepalive[0] == 0) {
        snprintf(config->keepalive, sizeof(config->keepalive), "%s", mqtt_config.keepalive[0] ? mqtt_config.keepalive : "60");
    }

    if (config->deviceType[0] == 0) {
        snprintf(config->deviceType, sizeof(config->deviceType), "%s", mqtt_config.deviceType);
    }

    if (config->subscribe_topic[0] == 0) {
        snprintf(config->subscribe_topic,
                 sizeof(config->subscribe_topic),
                 "%s/kalerm/iot/command/controlserver/home",
                 config->client_id);
    }

    if (config->publish_topic[0] == 0) {
        snprintf(config->publish_topic,
                 sizeof(config->publish_topic),
                 "stateserver/kalerm/iot/home/%s",
                 config->client_id);
    }
}

static bool mqtt_handle_internal_test_switch(cJSON *param, mqtt_command_ack_t *command_ack)
{
    mqtt_config_t next_config = mqtt_config;
    const char *action = NULL;
    bool client_id_overridden;
    bool subscribe_topic_overridden;
    bool publish_topic_overridden;
    esp_err_t err;

    if (command_ack) {
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
    }

    if (!param) {
        mqtt_set_command_ack(command_ack, -1, "param required");
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
        return false;
    }

    action = json_get_string(param, "action", NULL);
    if (action && strcmp(action, "switchMqttEnv") != 0) {
        mqtt_set_command_ack(command_ack, -1, "unsupported internalTest action");
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
        return false;
    }

    mqtt_copy_json_string_if_present(param, "brokerUri", next_config.broker_uri, sizeof(next_config.broker_uri));
    client_id_overridden = mqtt_copy_json_string_if_present(param, "clientId", next_config.client_id, sizeof(next_config.client_id));
    mqtt_copy_json_string_if_present(param, "username", next_config.username, sizeof(next_config.username));
    mqtt_copy_json_string_if_present(param, "password", next_config.password, sizeof(next_config.password));
    subscribe_topic_overridden = mqtt_copy_json_string_if_present(param, "subscribeTopic", next_config.subscribe_topic, sizeof(next_config.subscribe_topic));
    publish_topic_overridden = mqtt_copy_json_string_if_present(param, "publishTopic", next_config.publish_topic, sizeof(next_config.publish_topic));
    mqtt_copy_json_string_if_present(param, "qos", next_config.qos, sizeof(next_config.qos));
    mqtt_copy_json_string_if_present(param, "keepalive", next_config.keepalive, sizeof(next_config.keepalive));
    mqtt_copy_json_string_if_present(param, "deviceType", next_config.deviceType, sizeof(next_config.deviceType));

    if (client_id_overridden && !subscribe_topic_overridden) {
        memset(next_config.subscribe_topic, 0, sizeof(next_config.subscribe_topic));
    }

    if (client_id_overridden && !publish_topic_overridden) {
        memset(next_config.publish_topic, 0, sizeof(next_config.publish_topic));
    }

    if (next_config.broker_uri[0] == 0) {
        mqtt_set_command_ack(command_ack, -1, "brokerUri required");
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
        return false;
    }

    if (next_config.client_id[0] == 0) {
        mqtt_set_command_ack(command_ack, -1, "clientId required");
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
        return false;
    }

    next_config.auth_err = 0;
    mqtt_internal_test_fill_topic_defaults(&next_config);
    err = mqtt_schedule_switch_config(&next_config, MQTT_INTERNAL_TEST_SWITCH_DELAY_MS);
    if (err == ESP_ERR_INVALID_STATE) {
        mqtt_set_command_ack(command_ack, -1, "mqtt switch busy");
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
        return false;
    }
    if (err != ESP_OK) {
        mqtt_set_command_ack(command_ack, -1, "mqtt switch schedule failed");
        command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
        return false;
    }

    mqtt_set_command_ack(command_ack, 0, "MQTT env switch scheduled");
    command_ack->ack_topic = MQTT_ACK_TOPIC_INTERNAL_TEST;
    return true;
}

static void publish_internal_test_ack_to_mqtt(const char *msg_id,
                                              int ack_code,
                                              const ack_sensor_info_t *sensors,
                                              int sensors_count,
                                              const char *msg)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id, NULL, &data);
    char sub_topic[200];

    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), MQTT_INTERNAL_TEST_SUB_TOPIC);
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *control_ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(control_ack, "ackCode", ack_code);
    if (sensors != NULL && sensors_count > 0) {
        cJSON *sensors_arr = cJSON_CreateArray();
        for (int i = 0; i < sensors_count; i++) {
            cJSON *sensor = cJSON_CreateObject();
            cJSON_AddStringToObject(sensor, "name", sensors[i].name);
            cJSON_AddNumberToObject(sensor, "status", sensors[i].status);
            cJSON_AddItemToArray(sensors_arr, sensor);
        }
        cJSON_AddItemToObject(control_ack, "sensors", sensors_arr);
    }
    if (msg != NULL) {
        cJSON_AddStringToObject(control_ack, "msg", msg);
    }
    cJSON_AddItemToObject(data, "controllerAck", control_ack);
    mqtt_publish_json_root(root);
}
#endif

static bool mqtt_apply_remote_setting(const char *msg_id, cJSON *param, mqtt_command_ack_t *command_ack)
{
    if (!param)
    {
        mqtt_set_command_ack(command_ack, -1, "param required");
        return false;
    }

    mqtt_sync_runtime_setting_from_device();

    int setting_type = json_get_int(param, "settingType", -1);
    switch (setting_type)
    {
    case SETTING_TYPE_AUTO_POWER_OFF:
        mqtt_runtime_setting.auto_power_off_time = json_get_int(param, "autoPowerOffTime", mqtt_runtime_setting.auto_power_off_time);
        mqtt_normalize_runtime_setting();
        if (!mqtt_save_runtime_setting()) {
            mqtt_set_command_ack(command_ack, -1, "save auto power off failed");
            return false;
        }
        break;

    case SETTING_TYPE_AUTO_STANDBY:
        mqtt_runtime_setting.auto_stand_by_time = json_get_int(param, "autoStandByTime", mqtt_runtime_setting.auto_stand_by_time);
        mqtt_normalize_runtime_setting();
        if (!mqtt_save_runtime_setting()) {
            mqtt_set_command_ack(command_ack, -1, "save auto standby failed");
            return false;
        }
        break;

    case SETTING_TYPE_SOUND:
        mqtt_runtime_setting.voice_touch_tone = json_get_int(param, "voiceTouchTone", mqtt_runtime_setting.voice_touch_tone);
        mqtt_runtime_setting.voice_prompt = json_get_int(param, "voicePrompt", mqtt_runtime_setting.voice_prompt);
        mqtt_runtime_setting.voice_volume = json_get_int(param, "voiceVolume", mqtt_runtime_setting.voice_volume);
        mqtt_normalize_runtime_setting();
        if (!mqtt_save_runtime_setting()) {
            mqtt_set_command_ack(command_ack, -1, "save sound setting failed");
            return false;
        }
        break;

    case SETTING_TYPE_AUTO_CLEAR_BEAN:
        mqtt_runtime_setting.auto_clear_bean = json_get_int(param, "autoClearBean", mqtt_runtime_setting.auto_clear_bean);
        break;

    case SETTING_TYPE_STEAM_AUTO_CLEAN:
        mqtt_runtime_setting.steam_auto_clean = json_get_int(param, "steamAutoClean", mqtt_runtime_setting.steam_auto_clean);
        break;

    case SETTING_TYPE_AUTO_OTA:
    {
        mqtt_runtime_setting.auto_ota = json_get_int(param, "autoOta", mqtt_runtime_setting.auto_ota) ? 1 : 0;
        char auto_ota_buf[2] = {0};
        snprintf(auto_ota_buf, sizeof(auto_ota_buf), "%d", mqtt_runtime_setting.auto_ota);
        ota_params_edit(OTA_INFO_AUTOUP, auto_ota_buf);
        break;
    }

    case SETTING_TYPE_BREW_WATER_VOLUME:
        mqtt_runtime_setting.brew_water_volume = json_get_int(param, "brewWaterVolume", mqtt_runtime_setting.brew_water_volume);
        sp_pro_app_set_clean_volume((float)mqtt_runtime_setting.brew_water_volume);
        mqtt_sync_runtime_setting_from_device();
        if (!sp_pro_app_save_settings()) {
            mqtt_set_command_ack(command_ack, -1, "save brew water volume failed");
            return false;
        }
        break;

    case SETTING_TYPE_WATER_INTAKE_MODE:
        mqtt_runtime_setting.water_intake_mode = json_get_int(param, "waterIntakeMode", mqtt_runtime_setting.water_intake_mode) ? 1 : 0;
        factory_data.water_supply_mode = mqtt_runtime_setting.water_intake_mode;
        sp_pro_app_set_water_in_mode(mqtt_runtime_setting.water_intake_mode ? WATER_IN_MODE_BUCKET : WATER_IN_MODE_TANK);
        mqtt_sync_runtime_setting_from_device();
        if (!sp_pro_app_save_settings()) {
            mqtt_set_command_ack(command_ack, -1, "save water intake mode failed");
            return false;
        }
        if (!ctr_cmd_action(CTRL_ACT_FACTORY_WRITE, NULL)) {
            mqtt_set_command_ack(command_ack, -1, "water intake mode factory write failed");
            return false;
        }
        break;

    case SETTING_TYPE_CLEAR_WATERWAY:
        if (mqtt_get_task_status() != 0 && mqtt_get_task_status() != 22) {
            mqtt_set_command_ack(command_ack, -1, "coffee machine busy");
            return false;
        }
        sp_pro_app_set_ctrl_src(CTRL_SRC_MQTT);
        sp_pro_app_enter_maint_drain();
        mqtt_track_remote_controller_completion(msg_id, SETTING_TYPE_CLEAR_WATERWAY);
        break;

    case SETTING_TYPE_REMOTE_CLEAN:
    {
        int cleaning_mode = json_get_int(param, "cleaningMode", -1);
        bool ok = false;
        command_ack->ack_topic = MQTT_ACK_TOPIC_CLEANING;

        if (mqtt_get_task_status() != 0 && mqtt_get_task_status() != 22) {
            mqtt_set_command_ack(command_ack, -1, "coffee machine busy");
            mqtt_add_ack_result(command_ack, 1);
            return false;
        }

        if (cleaning_mode == CLEANING_MODE_BREW && !sp_pro_app_is_brew_handle_in_place()) {
            mqtt_set_command_ack(command_ack, -1, "brew handle not installed");
            mqtt_add_ack_result(command_ack, 3);
            return false;
        }

        if (cleaning_mode == CLEANING_MODE_DESCALING && factory_data.water_supply_mode != 0) {
            mqtt_set_command_ack(command_ack, -1, "water tank mode required");
            mqtt_add_ack_result(command_ack, 4);
            return false;
        }

        if (g_ctx.ms.water_box_shortage_flag) {
            mqtt_set_command_ack(command_ack, -1, "water shortage");
            mqtt_add_ack_result(command_ack, cleaning_mode == CLEANING_MODE_DESCALING ? 5 : 2);
            return false;
        }

        sp_pro_app_set_ctrl_src(CTRL_SRC_MQTT);
        if (cleaning_mode == CLEANING_MODE_BREW) {
            sp_pro_app_enter_maint_brew();
            ok = true;
        } else if (cleaning_mode == CLEANING_MODE_STEAM) {
            sp_pro_app_enter_maint_steam();
            ok = true;
        } else if (cleaning_mode == CLEANING_MODE_DESCALING) {
            sp_pro_app_enter_maint_des();
            ok = true;
        }

        if (!ok)
        {
            mqtt_set_command_ack(command_ack, -1, "remote clean dispatch failed");
            return false;
        }
        mqtt_mark_clean_result_incomplete(cleaning_mode);
        mqtt_track_remote_cleaning_ack(msg_id, cleaning_mode);
        break;
    }

    case SETTING_TYPE_CANCEL_GRIND:
        if (!mqtt_try_run_action(CTRL_ACT_GRIND_STOP, NULL))
        {
            mqtt_set_command_ack(command_ack, -1, "cancel grind failed");
            return false;
        }
        device_statistics_notify_remote_cancel();
        drink_record_notify_remote_cancel();
        extraction_curve_notify_remote_cancel();
        break;

    case SETTING_TYPE_CANCEL_EXTRACTION:
    case SETTING_TYPE_CANCEL_BREW_CLEAN:
        if (!mqtt_try_run_action(CTRL_ACT_CANCEL, NULL))
        {
            mqtt_set_command_ack(command_ack, -1, "cancel action failed");
            return false;
        }
        mqtt_clear_remote_controller_completion(SETTING_TYPE_BREW_CLEAN);
        mqtt_clear_remote_cleaning_ack();
        device_statistics_notify_remote_cancel();
        drink_record_notify_remote_cancel();
        extraction_curve_notify_remote_cancel();
        break;

    case SETTING_TYPE_BREW_CLEAN:
        if (!sp_pro_app_is_brew_handle_in_place()) {
            mqtt_set_command_ack(command_ack, -1, "brew handle missing");
            return false;
        }
        sp_pro_app_set_ctrl_src(CTRL_SRC_MQTT);
        sp_pro_app_enter_clean_brew();
        mqtt_track_remote_controller_completion(msg_id, SETTING_TYPE_BREW_CLEAN);
        break;

    default:
        mqtt_set_command_ack(command_ack, -1, "unsupported settingType");
        return false;
    }

    mqtt_set_command_ack(command_ack, 0, "OK");
    return true;
}

static bool parse_formula_cmd_from_mqtt(cJSON *param, mqtt_command_ack_t *command_ack)
{
    uint32_t record_id;
    uint32_t formula_id;
    formula_info_t formula = {0};
    formula_cmd_source_t source;

    if (!param) {
        mqtt_set_command_ack(command_ack, -1, "formula param required");
        return false;
    }

    ESP_LOGI(TAG, "=========parse_formula_cmd_from_mqtt=========");

    record_id = (uint32_t)json_get_int(param, "recordId", 0);
    formula_id = (uint32_t)json_get_int(param, "formulaId", 0);
    source = mqtt_get_formula_cmd_source(param);

    switch (source)
    {
    case FORMULA_CMD_SRC_INLINE:
        ESP_LOGI(TAG, "[MQTT][DRINK] one-shot brew with inline formula, recordId=%lu formulaId=%lu", record_id, formula_id);
        if (!mqtt_parse_formula_from_json(param, &formula)) {
            mqtt_set_command_ack(command_ack, -1, "invalid inline formula");
            return false;
        }
        break;

    case FORMULA_CMD_SRC_LOCAL_STORE:
        ESP_LOGI(TAG, "[MQTT][DRINK] resolve formula from local store, recordId=%lu formulaId=%lu", record_id, formula_id);
        if (mqtt_formula_param_has_inline_fields(param)) {
            ESP_LOGI(TAG,
                     "[MQTT][DRINK] recordId=%lu carries inline formula fields, prefer MQTT payload over local store",
                     record_id);
            if (mqtt_is_remote_player_formula_record_id(record_id)) {
                if (!mqtt_parse_player_formula_from_json(param, &formula)) {
                    mqtt_set_command_ack(command_ack, -1, "invalid player formula");
                    return false;
                }
            } else if (!mqtt_parse_formula_from_json(param, &formula)) {
                mqtt_set_command_ack(command_ack, -1, "invalid inline formula");
                return false;
            }
            break;
        }
        if (!formula_store_get_formula(formula_id, record_id, &formula)) {
            mqtt_set_command_ack(command_ack, -1, "local formula not found");
            return false;
        }
        break;

    default:
        mqtt_set_command_ack(command_ack, -1, "formula source unresolved");
        return false;
    }

#if MQTT_DEBUG_DUMP
    dump_master_formula(&formula);
#endif
    if (sp_pro_app_formula_requires_brew_handle(&formula) &&
        !sp_pro_app_is_brew_handle_in_place()) {
        mqtt_set_command_ack(command_ack, -1, "brew handle not in place");
        mqtt_add_ack_sensor(command_ack, "brew_handle_postion_flag", 1);
        return false;
    }

    if (!mqtt_start_drink_action(&formula)) {
        mqtt_set_command_ack(command_ack, -1, "drink command build failed");
        return false;
    }
    mqtt_set_command_ack(command_ack, 0, "OK");
    return true;
}

static bool parse_other_cmd_from_mqtt(const char *msg_id, const char *cmd_no, cJSON *param, mqtt_command_ack_t *command_ack)
{
    ESP_LOGI(TAG, "========== OTHER CMD DUMP ==========");
    ESP_LOGI(TAG, "cmd_no: %s", cmd_no ? cmd_no : "<null>");
    mqtt_set_command_ack(command_ack, 0, "OK");

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_REMOTE_POWER_ON)) {
        sp_pro_app_request_remote_power_on();
        return true;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_REMOTE_POWER_OFF)) {
        sp_pro_app_request_remote_power_off();
        return true;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_FACTORY_RESET)) {
        formula_store_factory_reset();
        mqtt_restore_local_defaults();
        device_statistics_factory_reset();
        mqtt_set_formula_force_update_pending(true);
        formula_store_set_force_update(true);
        sp_pro_app_request_remote_power_off();
        return true;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_RESET_DRINKS)) {
        mqtt_set_formula_force_update_pending(true);
        formula_store_set_force_update(true);
        return true;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_CHILD_LOCK)) {
        int task_status = mqtt_get_task_status();

        if (task_status != 0 && task_status != 22) {
            mqtt_set_command_ack(command_ack, -1, "child lock requires taskStatus 0 or 22");
            mqtt_add_ack_sensor(command_ack, "taskStatus", task_status);
            return false;
        }

        sp_pro_app_toggle_child_lock();
        return true;
    }

    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_REMOTE_PARAM_SETTING)) {
        return mqtt_apply_remote_setting(msg_id, param, command_ack);
    }

#if MQTT_INTERNAL_TEST_ENABLE
    if (mqtt_cmd_no_equals(cmd_no, CMD_NO_INTERNAL_TEST)) {
        return mqtt_handle_internal_test_switch(param, command_ack);
    }
#endif

    mqtt_set_command_ack(command_ack, -1, "unsupported cmd_no");
    return false;
}

bool mqtt_handle_control_data_from_mqtt(const char *msg_id, cJSON *data)
{
    cJSON *controll = json_get_object(data, "controll");
    if (!controll)
        return false;

    int cmd_type = json_get_int(controll, "cmd_type", -1);
    const char *cmd_no = json_get_string(controll, "cmd_no", NULL);
    cJSON *param = json_get_object(controll, "param");
    mqtt_command_ack_t command_ack = {0};
    mqtt_set_command_ack(&command_ack, 0, "OK");

    ESP_LOGI(TAG, "[MQTT][REMOTE] msg_id=%s cmd_type=%d cmd_no=%s",
             msg_id ? msg_id : "<null>",
             cmd_type,
             cmd_no ? cmd_no : "<null>");

    switch (cmd_type)
    {
    case CMD_TYPE_STATUS:
        if (!(cmd_no == NULL || mqtt_cmd_no_equals(cmd_no, CMD_NO_STATUS_REPORT))) {
            mqtt_set_command_ack(&command_ack, -1, "unsupported status cmd");
        }
        break;

    case CMD_TYPE_GRIND:
        if (!parse_grind_cmd_from_mqtt(param, &command_ack))
            mqtt_ensure_command_ack_failure(&command_ack, "invalid grind param");
        break;

    case CMD_TYPE_MASTER:
        if (!parse_formula_cmd_from_mqtt(param, &command_ack))
            mqtt_ensure_command_ack_failure(&command_ack, "invalid formula param");
        break;

    case CMD_TYPE_OTHER:
        parse_other_cmd_from_mqtt(msg_id, cmd_no, param, &command_ack);
        break;

    default:
        mqtt_set_command_ack(&command_ack, -1, "unsupported cmd_type");
        break;
    }

    if (msg_id)
    {
        if (command_ack.ack_topic == MQTT_ACK_TOPIC_INTERNAL_TEST) {
#if MQTT_INTERNAL_TEST_ENABLE
            publish_internal_test_ack_to_mqtt(msg_id,
                                              command_ack.ack_code,
                                              command_ack.sensors_count > 0 ? command_ack.sensors : NULL,
                                              command_ack.sensors_count,
                                              command_ack.msg);
#endif
        } else if (command_ack.ack_topic == MQTT_ACK_TOPIC_CLEANING) {
            publish_cleaning_ack_to_mqtt(msg_id,
                                         command_ack.ack_code,
                                         command_ack.results_count > 0 ? command_ack.results : NULL,
                                         command_ack.results_count,
                                         command_ack.msg);
        } else {
            publish_drink_ack_to_mqtt(msg_id,
                                      command_ack.ack_code,
                                      command_ack.sensors_count > 0 ? command_ack.sensors : NULL,
                                      command_ack.sensors_count,
                                      command_ack.msg);
        }
    }

    mqtt_publish_followup_status_after_control(cmd_type, cmd_no, &command_ack);

    return true;
}

void publish_drink_ack_to_mqtt(const char *msg_id,
                               int ack_code,
                               const ack_sensor_info_t *sensors,
                               int sensors_count,
                               const char *msg)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id, NULL, &data);
    char sub_topic[200];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "controlserver/kalerm/iot/remoteCtrl/result/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *control_ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(control_ack, "ackCode", ack_code);
    if (sensors != NULL && sensors_count > 0)
    {
        cJSON *sensors_arr = cJSON_CreateArray();
        for (int i = 0; i < sensors_count; i++)
        {
            cJSON *sensor = cJSON_CreateObject();
            cJSON_AddStringToObject(sensor, "name", sensors[i].name);
            cJSON_AddNumberToObject(sensor, "status", sensors[i].status);
            cJSON_AddItemToArray(sensors_arr, sensor);
        }
        cJSON_AddItemToObject(control_ack, "sensors", sensors_arr);
    }
    if (msg != NULL)
        cJSON_AddStringToObject(control_ack, "msg", msg);
    cJSON_AddItemToObject(data, "controllerAck", control_ack);
    mqtt_publish_json_root(root);
}

void publish_cleaning_ack_to_mqtt(const char *msg_id,
                                  int ack_code,
                                  const int *results,
                                  int results_count,
                                  const char *msg)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id, NULL, &data);
    char sub_topic[200];
    cJSON *cleaning_ack;

    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "controlserver/kalerm/iot/remoteCtrl/result/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cleaning_ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(cleaning_ack, "ackCode", ack_code);
    if (results != NULL && results_count > 0) {
        cJSON *results_arr = cJSON_CreateArray();
        for (int i = 0; i < results_count; ++i) {
            cJSON_AddItemToArray(results_arr, cJSON_CreateNumber(results[i]));
        }
        cJSON_AddItemToObject(cleaning_ack, "results", results_arr);
    }
    if (msg != NULL) {
        cJSON_AddStringToObject(cleaning_ack, "msg", msg);
    }
    cJSON_AddItemToObject(data, "cleaningAck", cleaning_ack);
    mqtt_publish_json_root(root);
}
