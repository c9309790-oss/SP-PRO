#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_protocol_core.h"
#include "mqtt_protocol_codec.h"
#include "formula_store.h"

static const char *TAG = "MQTT_PROTOCOL";
static mqtt_formula_overall_handler_t s_formula_overall_handler = NULL;
static bool s_formula_force_update_pending = false;
#define PLAYER_DRINK_NAME u8"\u73a9\u5bb6"

static void parse_prebrew_param(cJSON *param, formula_info_t *formula)
{
    cJSON *prebrew = json_get_object(param, "preBrew");
    if (!prebrew)
        return;

    formula->prebrew.status = json_get_int(prebrew, "status", 0);
    formula->prebrew.flow_velocity = json_get_int(prebrew, "flowVelocity", 0);
    formula->prebrew.wait_time = json_get_int(prebrew, "waitTime", 0);
    formula->prebrew.water_volume = json_get_int(prebrew, "waterVolume", 0);
}

static void parse_pressure_param(cJSON *param, formula_info_t *formula)
{
    cJSON *arr = json_get_array(param, "pressureStage");
    if (!arr)
        return;

    int size = cJSON_GetArraySize(arr);
    formula->pressure_stage_cnt = size > MAX_STAGE_NUM ? MAX_STAGE_NUM : size;

    for (int i = 0; i < formula->pressure_stage_cnt; i++)
    {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        formula->pressure_stage[i].pressure = json_get_float(item, "pressure", 0.0f);
        formula->pressure_stage[i].wait_time = json_get_int(item, "waitTime", 0);
    }
}

static void parse_velocity_param(cJSON *param, formula_info_t *formula)
{
    cJSON *arr = json_get_array(param, "velocityStage");
    if (!arr)
        return;

    int size = cJSON_GetArraySize(arr);
    formula->velocity_stage_cnt = size > MAX_STAGE_NUM ? MAX_STAGE_NUM : size;

    for (int i = 0; i < formula->velocity_stage_cnt; i++)
    {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        formula->velocity_stage[i].flow_velocity = json_get_int(item, "flowVelocity", 0);
        formula->velocity_stage[i].wait_time = json_get_int(item, "waitTime", 0);
    }
}

static bool mqtt_parse_formula_from_json_ex(cJSON *param, formula_info_t *formula, bool is_player_formula)
{
    if (!param || !formula)
        return false;

    memset(formula, 0, sizeof(*formula));
    formula->record_id = json_get_int(param, "recordId", 0);
    formula->formula_id = json_get_int(param, "formulaId", 0);

    strncpy(formula->formula_name, json_get_string(param, "formulaName", ""), sizeof(formula->formula_name) - 1);
    strncpy(formula->formula_remark, json_get_string(param, "formulaRemark", ""), sizeof(formula->formula_remark) - 1);
    strncpy(formula->support_mode, json_get_string(param, "supportMode", ""), sizeof(formula->support_mode) - 1);
    if (is_player_formula) {
        formula->drink_id = DRINK_ID_MASTER;
        strncpy(formula->drink_name, PLAYER_DRINK_NAME, sizeof(formula->drink_name) - 1);
    } else {
        formula->drink_id = json_get_int(param, "drinkId", 0);
        strncpy(formula->drink_name, json_get_string(param, "drinkName", ""), sizeof(formula->drink_name) - 1);
    }
    formula->label_id = json_get_int(param, "labelId", 0);
    strncpy(formula->label, json_get_string(param, "label", ""), sizeof(formula->label) - 1);
    formula->grind_range = json_get_int(param, "grindRange", 0);
    formula->grind_weight = json_get_float(param, "grindWeight", 0.0f);
    formula->preset_temperature = json_get_int(param, "presetTemperature", 0);
    formula->preset_liquid_weight = json_get_int(param, "presetLiquidWeight", 0);
    formula->water_temperature = json_get_int(param, "waterTemperature", 0);
    formula->water_weight = json_get_int(param, "waterWeight", 0);
    formula->milk_temperature = json_get_int(param, "milkTemperature", 0);
    formula->stage_priority = json_get_int(param, "stagePriority", 0);

    parse_prebrew_param(param, formula);
    parse_pressure_param(param, formula);
    parse_velocity_param(param, formula);
    return true;
}

bool mqtt_parse_formula_from_json(cJSON *param, formula_info_t *formula)
{
    return mqtt_parse_formula_from_json_ex(param, formula, false);
}

bool mqtt_parse_player_formula_from_json(cJSON *param, formula_info_t *formula)
{
    return mqtt_parse_formula_from_json_ex(param, formula, true);
}

static bool mqtt_parse_formula_array(cJSON *arr, formula_info_t **formula_list, int *formula_count, bool is_player_formula)
{
    if (!formula_list || !formula_count)
        return false;

    *formula_list = NULL;
    *formula_count = 0;

    if (!arr)
        return true;

    int count = cJSON_GetArraySize(arr);
    if (count <= 0)
        return true;

    formula_info_t *list = calloc(count, sizeof(formula_info_t));
    if (!list)
        return false;

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!mqtt_parse_formula_from_json_ex(item, &list[i], is_player_formula))
        {
            free(list);
            return false;
        }
    }

    *formula_list = list;
    *formula_count = count;
    return true;
}

void mqtt_free_formula_overall(formula_overall_t *formula_overall)
{
    if (!formula_overall)
        return;

    free(formula_overall->formula_intel_list);
    free(formula_overall->formula_list);
    memset(formula_overall, 0, sizeof(*formula_overall));
}

bool mqtt_parse_formula_overall_json(cJSON *formula_overall_json, formula_overall_t *formula_overall)
{
    if (!formula_overall_json || !formula_overall)
        return false;

    memset(formula_overall, 0, sizeof(*formula_overall));
    formula_overall->version = json_get_int(formula_overall_json, "version", 0);

    cJSON *force_update = cJSON_GetObjectItem(formula_overall_json, "forceUpdate");
    if (cJSON_IsBool(force_update))
        formula_overall->force_update = cJSON_IsTrue(force_update);

    if (!mqtt_parse_formula_array(json_get_array(formula_overall_json, "formulaIntelList"),
                                  &formula_overall->formula_intel_list,
                                  &formula_overall->formula_intel_list_count,
                                  false))
    {
        mqtt_free_formula_overall(formula_overall);
        return false;
    }

    if (!mqtt_parse_formula_array(json_get_array(formula_overall_json, "formulaList"),
                                  &formula_overall->formula_list,
                                  &formula_overall->formula_list_count,
                                  true))
    {
        mqtt_free_formula_overall(formula_overall);
        return false;
    }

    return true;
}

static uint16_t mqtt_formula_prebrew_water_volume_for_report(const formula_info_t *formula,
                                                             bool is_player_formula)
{
    uint16_t water_volume;

    if (!formula) {
        return 0U;
    }

    water_volume = formula->prebrew.water_volume;
    if (is_player_formula && water_volume < 5U) {
        return 5U;
    }

    return water_volume;
}

static cJSON *mqtt_create_formula_json_ex(const formula_info_t *formula, bool is_player_formula)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "recordId", formula->record_id);
    cJSON_AddNumberToObject(item, "formulaId", formula->formula_id);
    cJSON_AddStringToObject(item, "formulaName", formula->formula_name);
    cJSON_AddStringToObject(item, "formulaRemark", formula->formula_remark);
    if (strlen(formula->support_mode) > 0)
        cJSON_AddStringToObject(item, "supportMode", formula->support_mode);
    if (is_player_formula) {
        cJSON_AddNumberToObject(item, "drinkId", 0);
        cJSON_AddStringToObject(item, "drinkName", PLAYER_DRINK_NAME);
    } else if (formula->drink_id != 0) {
        cJSON_AddNumberToObject(item, "drinkId", formula->drink_id);
        if (strlen(formula->drink_name) > 0) {
            cJSON_AddStringToObject(item, "drinkName", formula->drink_name);
        }
    }
    cJSON_AddNumberToObject(item, "labelId", formula->label_id);
    cJSON_AddStringToObject(item, "label", formula->label);
    cJSON_AddNumberToObject(item, "grindRange", formula->grind_range);
    cJSON_AddNumberToObject(item, "grindWeight", formula->grind_weight);
    cJSON_AddNumberToObject(item, "presetTemperature", formula->preset_temperature);
    cJSON_AddNumberToObject(item, "presetLiquidWeight", formula->preset_liquid_weight);
    cJSON_AddNumberToObject(item, "waterTemperature", formula->water_temperature);
    cJSON_AddNumberToObject(item, "waterWeight", formula->water_weight);
    cJSON_AddNumberToObject(item, "milkTemperature", formula->milk_temperature);

    cJSON *prebrew = cJSON_CreateObject();
    cJSON_AddNumberToObject(prebrew, "status", formula->prebrew.status);
    cJSON_AddNumberToObject(prebrew, "flowVelocity", formula->prebrew.flow_velocity);
    cJSON_AddNumberToObject(prebrew, "waitTime", formula->prebrew.wait_time);
    cJSON_AddNumberToObject(prebrew,
                            "waterVolume",
                            mqtt_formula_prebrew_water_volume_for_report(formula, is_player_formula));
    cJSON_AddItemToObject(item, "preBrew", prebrew);

    cJSON_AddNumberToObject(item, "stagePriority", formula->stage_priority);

    cJSON *pressure_stage_arr = cJSON_CreateArray();
    for (int i = 0; i < formula->pressure_stage_cnt; i++)
    {
        cJSON *stage = cJSON_CreateObject();
        cJSON_AddNumberToObject(stage, "pressure", formula->pressure_stage[i].pressure);
        cJSON_AddNumberToObject(stage, "waitTime", formula->pressure_stage[i].wait_time);
        cJSON_AddItemToArray(pressure_stage_arr, stage);
    }
    cJSON_AddItemToObject(item, "pressureStage", pressure_stage_arr);

    cJSON *velocity_stage_arr = cJSON_CreateArray();
    for (int i = 0; i < formula->velocity_stage_cnt; i++)
    {
        cJSON *stage = cJSON_CreateObject();
        cJSON_AddNumberToObject(stage, "flowVelocity", formula->velocity_stage[i].flow_velocity);
        cJSON_AddNumberToObject(stage, "waitTime", formula->velocity_stage[i].wait_time);
        cJSON_AddItemToArray(velocity_stage_arr, stage);
    }
    cJSON_AddItemToObject(item, "velocityStage", velocity_stage_arr);

    return item;
}

cJSON *mqtt_create_formula_json(const formula_info_t *formula)
{
    return mqtt_create_formula_json_ex(formula, false);
}

cJSON *mqtt_create_formula_list_json(const formula_info_t *formula)
{
    return mqtt_create_formula_json_ex(formula, true);
}

void mqtt_register_formula_overall_handler(mqtt_formula_overall_handler_t handler)
{
    s_formula_overall_handler = handler;
}

void mqtt_set_formula_force_update_pending(bool pending)
{
    s_formula_force_update_pending = pending;
}

bool mqtt_is_formula_force_update_pending(void)
{
    return s_formula_force_update_pending;
}

static bool mqtt_parse_formula_overall_setting_from_mqtt(const char *msg_id, cJSON *data)
{
    cJSON *setting = json_get_object(data, "setting");
    if (!setting)
        return false;

    cJSON *formula_overall_json = json_get_object(setting, "formulaOverall");
    if (!formula_overall_json)
        return false;

    formula_overall_t formula_overall = {0};
    mqtt_ack_result_t ack_result = {0};
    mqtt_set_ack_result(&ack_result, 2, "formula handler not registered");

    if (!mqtt_parse_formula_overall_json(formula_overall_json, &formula_overall))
    {
        mqtt_set_ack_result(&ack_result, 2, "formulaOverall parse failed");
    }
    else if (s_formula_overall_handler)
    {
        mqtt_set_ack_result(&ack_result, 0, "OK");
        if (!s_formula_overall_handler(&formula_overall, &ack_result) && ack_result.msg[0] == 0)
            mqtt_set_ack_result(&ack_result, 2, "formula handler rejected");
    }

    ESP_LOGI(TAG, "formulaOverallAck ackCode=%d msg=%s version=%d", ack_result.ack_code, ack_result.msg, formula_overall.version);
    publish_formula_overall_ack_to_mqtt(msg_id ? msg_id : "0", ack_result.ack_code, ack_result.msg);
    if (ack_result.ack_code == 0) {
        ESP_LOGI(TAG, "queue formulaOverall status report after setting apply");
        mqtt_set_next_formula_overall_report_msg_id(msg_id ? msg_id : "0");
        if (!mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL,
                                                         "formula_overall_setting",
                                                         0U)) {
            mqtt_set_next_formula_overall_report_msg_id(NULL);
        }
    }

    mqtt_free_formula_overall(&formula_overall);
    return true;
}

static bool parse_formula_force_update_ack_from_mqtt(cJSON *data)
{
    cJSON *formula_overall = json_get_object(data, "formulaOverall");
    if (!formula_overall)
        return false;

    cJSON *force_update_ack = cJSON_GetObjectItem(formula_overall, "forceUpdateAck");
    if (!cJSON_IsBool(force_update_ack))
        return false;

    bool ack = cJSON_IsTrue(force_update_ack);
    ESP_LOGI(TAG, "formula forceUpdateAck=%d", ack);
    if (ack) {
        s_formula_force_update_pending = false;
        formula_store_set_force_update(false);
    }

    return true;
}

bool mqtt_handle_formula_ack_data_from_mqtt(cJSON *data)
{
    return parse_formula_force_update_ack_from_mqtt(data);
}

bool mqtt_handle_formula_setting_data_from_mqtt(const char *msg_id, cJSON *data)
{
    return mqtt_parse_formula_overall_setting_from_mqtt(msg_id, data);
}

void publish_formula_overall_ack_to_mqtt(const char *msg_id, int ack_code, const char *msg)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id ? msg_id : "0", NULL, &data);
    char sub_topic[200];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "controlserver/kalerm/iot/remoteCtrl/result/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *formula_overall_ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(formula_overall_ack, "ackCode", ack_code);
    cJSON_AddStringToObject(formula_overall_ack, "msg", msg ? msg : "");
    cJSON_AddItemToObject(data, "formulaOverallAck", formula_overall_ack);
    mqtt_publish_json_root(root);
}




