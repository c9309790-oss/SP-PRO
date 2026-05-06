#include "system_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "mqtt.h"
#include "mqtt_protocol_core.h"
#include "mqtt_protocol_codec.h"
#include "ota_ctr.h"
#include "formula_store.h"
#include "device_statistics_store.h"
#include "mqtt_protocol.h"
#include "sp_pro_app_ctrl.h"
#include "nvs_retry.h"
#include "wifi.h"

static mqtt_device_status_provider_t s_device_status_provider = NULL;
static const char *TAG = "MQTT_STATUS";
#define MQTT_TEST_TRIM_STATUS_STATISTICS 0
#define MQTT_TEST_TRIM_STATUS_FORMULA_OVERALL 0
#define MQTT_FORMULA_REPORT_LABEL "DEFAULT"
#define MQTT_CALIBRATION_DEFAULT_LIQUID_K      0.0f
#define MQTT_CALIBRATION_DEFAULT_LIQUID_B      0.0f
#define MQTT_CALIBRATION_DEFAULT_FLOWMETER_ADC 0.45f
#define MQTT_RUNTIME_SETTING_NVS_NAMESPACE "mqtt_runtime"
#define MQTT_RUNTIME_SETTING_NVS_KEY "setting"
#define MQTT_RUNTIME_SETTING_MAGIC 0x4D515254UL
#define MQTT_RUNTIME_SETTING_VERSION 1U
static char s_next_formula_overall_report_msg_id[64] = {0};
static bool s_runtime_setting_loaded = false;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    setting_info_t setting;
} mqtt_runtime_setting_blob_t;

static clean_result_info_t s_clean_result = {
    .steam_pole_cleaning = 1,
    .back_wash = 1,
    .descaling_water = 1,
};

static int mqtt_formula_report_record_id(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_ESPRESSO:
        return 1001;
    case DRINK_ID_AMERICAN:
        return 1002;
    case DRINK_ID_COLDBREW:
        return 1003;
    case DRINK_ID_WATER:
        return 1005;
    default:
        return 1000 + (int)drink_id;
    }
}

static const char *mqtt_formula_report_name(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_ESPRESSO:
        return u8"\u6d53\u7f29\u81ea\u5b9a\u4e49\u914d\u65b9";
    case DRINK_ID_AMERICAN:
        return u8"\u7f8e\u5f0f\u81ea\u5b9a\u4e49\u914d\u65b9";
    case DRINK_ID_COLDBREW:
        return u8"\u51b7\u8403\u81ea\u5b9a\u4e49\u914d\u65b9";
    case DRINK_ID_WATER:
        return u8"\u996e\u7528\u6c34";
    default:
        return "";
    }
}

static const char *mqtt_formula_report_drink_name(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_ESPRESSO:
        return u8"\u6d53\u7f29";
    case DRINK_ID_AMERICAN:
        return u8"\u7f8e\u5f0f";
    case DRINK_ID_COLDBREW:
        return u8"\u51b7\u8403";
    case DRINK_ID_WATER:
        return u8"\u996e\u7528\u6c34";
    default:
        return "";
    }
}

static void mqtt_apply_formula_report_runtime_override(formula_info_t *formula,
                                                       const app_command_view_t *settings)
{
    if (!formula || !settings) {
        return;
    }

    switch (formula->drink_id) {
    case DRINK_ID_ESPRESSO:
        formula->grind_weight = settings->grind_w;
        formula->preset_temperature = (uint16_t)(settings->esp_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->esp_brew_w + 0.5f);
        break;

    case DRINK_ID_AMERICAN:
        formula->grind_weight = settings->grind_w;
        formula->preset_temperature = (uint16_t)(settings->ame_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->ame_brew_w + 0.5f);
        formula->water_temperature = (uint16_t)(settings->ame_water_t + 0.5f);
        formula->water_weight = (uint16_t)(settings->ame_water_w + 0.5f);
        break;

    case DRINK_ID_COLDBREW:
        formula->grind_weight = settings->grind_w;
        formula->preset_temperature = 0U;
        formula->preset_liquid_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        formula->water_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        break;

    case DRINK_ID_WATER:
        formula->grind_weight = 0.0f;
        formula->grind_range = 0U;
        formula->preset_temperature = 0U;
        formula->preset_liquid_weight = 0U;
        formula->water_temperature = (uint16_t)(settings->hot_water_t + 0.5f);
        formula->water_weight = (uint16_t)(settings->hot_water_w + 0.5f);
        formula->milk_temperature = 0U;
        formula->prebrew.status = 0U;
        formula->prebrew.flow_velocity = 0U;
        formula->prebrew.wait_time = 0U;
        formula->prebrew.water_volume = 0U;
        formula->stage_priority = 0U;
        formula->pressure_stage_cnt = 0U;
        memset(formula->pressure_stage, 0, sizeof(formula->pressure_stage));
        snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", u8"\u996e\u7528\u6c34");
        snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", u8"\u996e\u7528\u6c34");
        break;

    default:
        break;
    }
}

static cJSON *mqtt_create_formula_report_json(const formula_info_t *formula, bool is_player_formula)
{
    cJSON *item;

    item = is_player_formula ? mqtt_create_formula_list_json(formula)
                             : mqtt_create_formula_json(formula);
    if (!item) {
        return cJSON_CreateObject();
    }

    return item;
}

static cJSON *mqtt_create_formula_intel_report_json(const formula_info_t *formula)
{
    app_command_view_t settings = {0};
    formula_info_t report_formula;

    if (!formula) {
        return cJSON_CreateObject();
    }

    sp_pro_app_get_command_view(&settings);
    report_formula = *formula;
    mqtt_apply_formula_report_runtime_override(&report_formula, &settings);
    return mqtt_create_formula_json(&report_formula);
}

setting_info_t mqtt_runtime_setting = {
    .auto_power_off_time = 1800,
    .auto_stand_by_time = 1800,
    .voice_touch_tone = 1,
    .voice_prompt = 1,
    .voice_volume = 3,
    .auto_clear_bean = 1,
    .steam_auto_clean = 1,
    .factory_status = 0,
    .auto_ota = 0,
    .brew_water_volume = 100,
    .water_intake_mode = 0,
};

extern MACHINE_STATUS machine_status;
extern FLASH_FACTORY_DATA factory_data;

static void publish_device_status_to_mqtt_internal(const device_status_message_t *status,
                                                   uint32_t section_mask);
static void mqtt_report_device_status_internal(uint32_t section_mask,
                                               const char *reason);
static void mqtt_load_runtime_setting_once(void);

static esp_err_t mqtt_runtime_setting_rewrite_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    const mqtt_runtime_setting_blob_t *blob = (const mqtt_runtime_setting_blob_t *)ctx;

    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_set_blob(nvs_handle, MQTT_RUNTIME_SETTING_NVS_KEY, blob, sizeof(*blob));
}

void mqtt_set_next_formula_overall_report_msg_id(const char *msg_id)
{
    if (msg_id && msg_id[0] != '\0') {
        mqtt_copy_string(s_next_formula_overall_report_msg_id,
                         sizeof(s_next_formula_overall_report_msg_id),
                         msg_id,
                         NULL);
    } else {
        s_next_formula_overall_report_msg_id[0] = '\0';
    }
}

const setting_info_t *mqtt_get_runtime_setting(void)
{
    mqtt_load_runtime_setting_once();
    return &mqtt_runtime_setting;
}

static void mqtt_load_runtime_setting_once(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    mqtt_runtime_setting_blob_t blob = {0};
    size_t size = sizeof(blob);

    if (s_runtime_setting_loaded) {
        return;
    }

    s_runtime_setting_loaded = true;

    err = nvs_open(MQTT_RUNTIME_SETTING_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "runtime setting nvs_open skipped: %s", esp_err_to_name(err));
        mqtt_normalize_runtime_setting();
        return;
    }

    err = nvs_get_blob(nvs_handle, MQTT_RUNTIME_SETTING_NVS_KEY, &blob, &size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "runtime setting load skipped: %s", esp_err_to_name(err));
        mqtt_normalize_runtime_setting();
        return;
    }

    if (size != sizeof(blob) ||
        blob.magic != MQTT_RUNTIME_SETTING_MAGIC ||
        blob.version != MQTT_RUNTIME_SETTING_VERSION) {
        ESP_LOGW(TAG,
                 "runtime setting blob invalid size=%u magic=0x%08lx version=%u",
                 (unsigned)size,
                 (unsigned long)blob.magic,
                 (unsigned)blob.version);
        mqtt_normalize_runtime_setting();
        return;
    }

    mqtt_runtime_setting = blob.setting;
    mqtt_normalize_runtime_setting();
    ESP_LOGI(TAG, "runtime setting loaded from NVS");
}

void mqtt_normalize_runtime_setting(void)
{
    /* Factory / validation builds should keep prompts enabled so H-alarm
     * verification is not affected by stale runtime settings from MQTT. */
    mqtt_runtime_setting.voice_touch_tone = 1;
    mqtt_runtime_setting.voice_prompt = 1;

    if (mqtt_runtime_setting.auto_power_off_time > 0 &&
        mqtt_runtime_setting.auto_stand_by_time > 0 &&
        mqtt_runtime_setting.auto_stand_by_time > mqtt_runtime_setting.auto_power_off_time) {
        mqtt_runtime_setting.auto_stand_by_time = mqtt_runtime_setting.auto_power_off_time;
    }

    if (mqtt_runtime_setting.voice_volume <= 0) {
        mqtt_runtime_setting.voice_volume = 3;
    } else if (mqtt_runtime_setting.voice_volume > 5) {
        mqtt_runtime_setting.voice_volume = 5;
    }
}

void mqtt_register_device_status_provider(mqtt_device_status_provider_t provider)
{
    s_device_status_provider = provider;
}

void mqtt_sync_runtime_setting_from_device(void)
{
    mqtt_load_runtime_setting_once();
    mqtt_runtime_setting.factory_status = factory_data.first_powered_on ? 1 : mqtt_runtime_setting.factory_status;

    if (g_ota_info.ota_auto_up[0] == '0' || g_ota_info.ota_auto_up[0] == '1')
        mqtt_runtime_setting.auto_ota = (g_ota_info.ota_auto_up[0] == '1') ? 1 : 0;

    if (sp_pro_app_get_clean_volume() > 0.0f)
        mqtt_runtime_setting.brew_water_volume = (int)(sp_pro_app_get_clean_volume() + 0.5f);

    mqtt_runtime_setting.water_intake_mode = factory_data.water_supply_mode;
    mqtt_normalize_runtime_setting();
}

void mqtt_restore_local_defaults(void)
{
    mqtt_load_runtime_setting_once();
    mqtt_runtime_setting.auto_power_off_time = 1800;
    mqtt_runtime_setting.auto_stand_by_time = 1800;
    mqtt_runtime_setting.voice_touch_tone = 1;
    mqtt_runtime_setting.voice_prompt = 1;
    mqtt_runtime_setting.voice_volume = 3;
    mqtt_runtime_setting.auto_clear_bean = 1;
    mqtt_runtime_setting.steam_auto_clean = 1;
    mqtt_runtime_setting.factory_status = 0;
    mqtt_runtime_setting.auto_ota = 0;
    mqtt_runtime_setting.brew_water_volume = 100;
    mqtt_runtime_setting.water_intake_mode = 0;
    mqtt_normalize_runtime_setting();

    sp_pro_app_restore_default_settings();
    factory_data.water_supply_mode = 0;
    (void)mqtt_save_runtime_setting();
}

static void mqtt_set_clean_result_value(int cleaning_mode, int value)
{
    switch (cleaning_mode) {
    case CLEANING_MODE_BREW:
        s_clean_result.back_wash = value;
        break;
    case CLEANING_MODE_STEAM:
        s_clean_result.steam_pole_cleaning = value;
        break;
    case CLEANING_MODE_DESCALING:
        s_clean_result.descaling_water = value;
        break;
    default:
        break;
    }
}

bool mqtt_save_runtime_setting(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    mqtt_runtime_setting_blob_t blob = {
        .magic = MQTT_RUNTIME_SETTING_MAGIC,
        .version = MQTT_RUNTIME_SETTING_VERSION,
        .reserved = 0U,
        .setting = mqtt_runtime_setting,
    };

    mqtt_normalize_runtime_setting();

    err = nvs_open(MQTT_RUNTIME_SETTING_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime setting nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(nvs_handle, MQTT_RUNTIME_SETTING_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        static const char *const keys[] = {
            MQTT_RUNTIME_SETTING_NVS_KEY,
        };
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "runtime setting",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   mqtt_runtime_setting_rewrite_to_nvs,
                                                   &blob);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime setting save failed: %s", esp_err_to_name(err));
        return false;
    }

    s_runtime_setting_loaded = true;
    ESP_LOGI(TAG, "runtime setting saved to NVS");
    return true;
}

void mqtt_mark_clean_result_incomplete(int cleaning_mode)
{
    mqtt_set_clean_result_value(cleaning_mode, 1);
    ESP_LOGI(TAG,
             "Mark cleanResult incomplete, cleaningMode=%d",
             cleaning_mode);
}

void mqtt_notify_clean_result_complete(int cleaning_mode)
{
    mqtt_set_clean_result_value(cleaning_mode, 0);

    ESP_LOGI(TAG,
             "Queue cleanResult report, cleaningMode=%d",
             cleaning_mode);
    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS |
                                                    MQTT_DEVICE_STATUS_SECTION_CLEAN_RESULT,
                                                "clean_result",
                                                0U);
}

static void mqtt_set_sensor_status(sensor_info_t *sensor, const char *name, int status)
{
    if (!sensor)
        return;

    snprintf(sensor->name, sizeof(sensor->name), "%s", name ? name : "");
    snprintf(sensor->status, sizeof(sensor->status), "%d", status);
}

static int mqtt_get_maintain_notice_status(maint_type_t type)
{
    return sp_pro_app_get_maintain_notice_status(type);
}

static void mqtt_set_sensor_status_testable(sensor_info_t *sensor, const char *name, int status)
{
    mqtt_set_sensor_status(sensor, name, status);
}

int mqtt_get_task_status(void)
{
    app_state_t app_state = sp_pro_app_get_state();

    /* Only report OTA taskStatus=8 when the controller is actually applying
     * the upgrade payload. Downloaded / verified / wait-confirm phases should
     * fall back to the normal machine task status. */
    if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_YMODEM)
    {
        return 8;
    }

    /* Power-off / standby must report sleep even if the controller still reports ready. */
    if (app_state == ST_OFF || app_state == ST_STANDBY) {
        return MQTT_TASK_STATUS_SLEEP;
    }

    /* Child-lock must report 22 even if the UI is still on a ready-like page. */
    if (app_state == ST_LOCK || sp_pro_app_is_child_lock_enabled()) {
        return 22;
    }

    if (machine_status.ctr_status == 0) {
        return 1;
    }

    switch (app_state)
    {
    case ST_READY:
    case ST_WIFI:
        return 0;

    case ST_MASTER:
    case ST_ESPRESSO:
    case ST_AMERICANO:
    case ST_COLD_BREW:
    case ST_WATER:
    case ST_STEAM:
    case ST_GRIND:
        return 257;

    case ST_CLEAN_BREW:
    case ST_MAINT_BREW:
    case ST_MAINT_DES:
    case ST_MAINT_STEAM:
    case ST_MAINT_DRAIN:
        return 1793;

    case ST_ON:
    case ST_CLEAR_BEAN:
    case ST_DRINK_SET:
    case ST_SETTING:
    case ST_CALIBRATION:
    case ST_DETECTION:
    case ST_ALARM:
    case ST_OTA:
    case ST_AUTO_TEST:
        return 2;

    default:
        return 2;
    }
}

static int mqtt_fill_default_sensors(sensor_info_t *sensors, int max_sensors)
{
    if (!sensors || max_sensors < MQTT_STATUS_SENSOR_MAX)
        return 0;

    int idx = 0;

    mqtt_set_sensor_status_testable(&sensors[idx++], "taskStatus", mqtt_get_task_status());
    mqtt_set_sensor_status_testable(&sensors[idx++], "bean_detect_flag", machine_status.bean_detect_flag);
    mqtt_set_sensor_status_testable(&sensors[idx++], "beanbox_in_place", machine_status.beanbox_in_place ? 0 : 1);
    mqtt_set_sensor_status_testable(&sensors[idx++], "grind_handle_postion_flag", machine_status.grind_handle_postion_flag ? 0 : 1);
    mqtt_set_sensor_status_testable(&sensors[idx++], "brew_handle_postion_flag", machine_status.brew_handle_postion_flag ? 0 : 1);
    mqtt_set_sensor_status_testable(&sensors[idx++], "water_box_shortage_flag", machine_status.water_box_shortage_flag);
    mqtt_set_sensor_status_testable(&sensors[idx++], "WATER_PUMP_ERROR", (machine_status.error_code & WATER_PUMP_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "WATER_WAY_ERROR", (machine_status.error_code & WATER_WAY_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "BREW_HEAT_PLATE_ERROR", (machine_status.error_code & BREW_HEAT_PLATE_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "BREW_HEAT_PLATE_FAST_TEMP_ERROR", (machine_status.error_code & BREW_HEAT_PLATE_FAST_TEMP_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "BREW_HEAT_PLATE_HIGH_TEMP_ERROR", (machine_status.error_code & BREW_HEAT_PLATE_HIGH_TEMP_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "STEAM_HEAT_PLATE_ERROR", (machine_status.error_code & STEAM_HEAT_PLATE_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "STEAM_HEAT_PLATE_FAST_TEMP_ERROR", (machine_status.error_code & STEAM_HEAT_PLATE_FAST_TEMP_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "STEAM_HEAT_PLATE_HIGH_TEMP_ERROR", (machine_status.error_code & STEAM_HEAT_PLATE_HIGH_TEMP_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "LOW_MACHINE_TEMP_ERROR", (machine_status.error_code & LOW_MACHINE_TEMP_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "NTC_COFFEE_ERROR", (machine_status.error_code & NTC_COFFEE_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "NTC_FOAM_ERROR", (machine_status.error_code & NTC_FOAM_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "NTC_BREW_ERROR", (machine_status.error_code & NTC_BREW_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "NTC_STEAM_ERROR", (machine_status.error_code & NTC_STEAM_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "NTC_RELIEF_ERROR", (machine_status.error_code & NTC_RELIEF_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "PRESSURE_SIGNAL_ERROR", (machine_status.error_code & PRESSURE_SIGNAL_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "PRESSURE_VALUE_ERROR", (machine_status.error_code & PRESSURE_VALUE_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "E_FAST_ERROR", (machine_status.error_code & E_FAST_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "BEANBOX_ERROR", (machine_status.error_code & BEANBOX_ERROR) ? 1 : 0);
    mqtt_set_sensor_status_testable(&sensors[idx++], "needCleanBrewing", mqtt_get_maintain_notice_status(MAINT_TYPE_BREW));
    mqtt_set_sensor_status_testable(&sensors[idx++], "needCleanDescaling", mqtt_get_maintain_notice_status(MAINT_TYPE_DES));
    mqtt_set_sensor_status_testable(&sensors[idx++], "needCleanSteam", mqtt_get_maintain_notice_status(MAINT_TYPE_STEAM));
    mqtt_set_sensor_status_testable(&sensors[idx++], "steamPlateHeating", machine_status.steam_flag == STEAM_UNREADY ? 1 : 0);

    return idx;
}

static void mqtt_fill_default_calibration(calibration_info_t *calibration)
{
    if (!calibration)
        return;

    snprintf(calibration->powder_k, sizeof(calibration->powder_k), "%.8f", factory_data.powder_k_value);
    snprintf(calibration->powder_b, sizeof(calibration->powder_b), "%.8f", factory_data.powder_b_value);
    snprintf(calibration->liquid_k, sizeof(calibration->liquid_k), "%.8f", MQTT_CALIBRATION_DEFAULT_LIQUID_K);
    snprintf(calibration->liquid_b, sizeof(calibration->liquid_b), "%.8f", MQTT_CALIBRATION_DEFAULT_LIQUID_B);
    snprintf(calibration->flow_meter_adc, sizeof(calibration->flow_meter_adc), "%.8f", MQTT_CALIBRATION_DEFAULT_FLOWMETER_ADC);
}

static void mqtt_fill_default_setting(setting_info_t *setting)
{
    if (!setting)
        return;

    mqtt_sync_runtime_setting_from_device();
    *setting = mqtt_runtime_setting;
}

static void mqtt_fill_default_wifi(wifi_info_t *wifi)
{
    if (!wifi)
        return;

    wifi_ap_record_t ap_info_local = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info_local) == ESP_OK)
    {
        wifi->signal = wifi_transform_rssi(ap_info_local.rssi);
        snprintf(wifi->name, sizeof(wifi->name), "%.31s", (char *)ap_info_local.ssid);
    }
    else
    {
        wifi->signal = 0;
        mqtt_copy_string(wifi->name, sizeof(wifi->name), wifi_ssid, "");
    }
}

static void mqtt_report_device_status_internal(uint32_t section_mask,
                                               const char *reason)
{
    device_status_message_t status = {0};
    sensor_info_t default_sensors[MQTT_STATUS_SENSOR_MAX] = {0};
    sensor_info_t filtered_sensors[1] = {0};
    bool provider_ok = false;
    bool include_formula_overall;
    bool include_statistics;
    uint32_t effective_section_mask = section_mask;

    if (s_device_status_provider)
        provider_ok = s_device_status_provider(&status);

    if (!provider_ok)
    {
        mqtt_get_now_ms_string(status.msg_id, sizeof(status.msg_id));
        mqtt_copy_string(status.version, sizeof(status.version), MQTT_PROTOCOL_VERSION, MQTT_PROTOCOL_VERSION);
        snprintf(status.sub_topic, sizeof(status.sub_topic), "stateserver/kalerm/iot/status/%s", mqtt_config.client_id);

        status.device_info.sensors = default_sensors;
        status.device_info.sensors_count = mqtt_fill_default_sensors(default_sensors, MQTT_STATUS_SENSOR_MAX);
        status.device_info.formula_overall.version = 0;
        status.device_info.formula_overall.force_update = mqtt_is_formula_force_update_pending();
        mqtt_fill_default_calibration(&status.device_info.calibration);
        mqtt_fill_default_setting(&status.device_info.setting);
        mqtt_fill_default_wifi(&status.device_info.wifi);
    }
    else
    {
        if (status.msg_id[0] == 0)
            mqtt_get_now_ms_string(status.msg_id, sizeof(status.msg_id));
        if (status.version[0] == 0)
            mqtt_copy_string(status.version, sizeof(status.version), MQTT_PROTOCOL_VERSION, MQTT_PROTOCOL_VERSION);
        if (status.sub_topic[0] == 0)
            snprintf(status.sub_topic, sizeof(status.sub_topic), "stateserver/kalerm/iot/status/%s", mqtt_config.client_id);
        if (!status.device_info.sensors || status.device_info.sensors_count <= 0)
        {
            status.device_info.sensors = default_sensors;
            status.device_info.sensors_count = mqtt_fill_default_sensors(default_sensors, MQTT_STATUS_SENSOR_MAX);
        }
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_SENSORS) == 0U) {
        status.device_info.sensors = NULL;
        status.device_info.sensors_count = 0;
    } else if ((section_mask & MQTT_DEVICE_STATUS_SECTION_SENSOR_TASKSTATUS_ONLY) != 0U &&
               status.device_info.sensors != NULL &&
               status.device_info.sensors_count > 0) {
        bool found = false;

        for (int i = 0; i < status.device_info.sensors_count; ++i) {
            if (strcmp(status.device_info.sensors[i].name, "taskStatus") == 0) {
                filtered_sensors[0] = status.device_info.sensors[i];
                status.device_info.sensors = filtered_sensors;
                status.device_info.sensors_count = 1;
                found = true;
                break;
            }
        }

        if (!found) {
            status.device_info.sensors = NULL;
            status.device_info.sensors_count = 0;
        }
    }

    include_formula_overall =
        ((section_mask & MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL) != 0U) &&
        !MQTT_TEST_TRIM_STATUS_FORMULA_OVERALL;
    include_statistics =
        ((section_mask & MQTT_DEVICE_STATUS_SECTION_STATISTICS) != 0U) &&
        !MQTT_TEST_TRIM_STATUS_STATISTICS;

    if (!include_formula_overall) {
        effective_section_mask &= ~MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL;
    }

    if (!include_statistics) {
        effective_section_mask &= ~MQTT_DEVICE_STATUS_SECTION_STATISTICS;
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_CLEAN_RESULT) == 0U) {
        memset(&status.device_info.clean_result, 0, sizeof(status.device_info.clean_result));
    } else {
        status.device_info.clean_result = s_clean_result;
    }

    if (include_formula_overall) {
        if (!formula_store_get_overall_snapshot(&status.device_info.formula_overall) &&
            status.device_info.formula_overall.version <= 0) {
            if (formula_store_ensure_local_defaults()) {
                formula_store_get_overall_snapshot(&status.device_info.formula_overall);
            }
        }

        if (status.device_info.formula_overall.version <= 0) {
            status.device_info.formula_overall.force_update = mqtt_is_formula_force_update_pending();
        }
    } else {
        memset(&status.device_info.formula_overall, 0, sizeof(status.device_info.formula_overall));
    }

    if (include_statistics) {
        device_statistics_fill_snapshot(&status.device_info.statistics);
    } else {
        memset(&status.device_info.statistics, 0, sizeof(status.device_info.statistics));
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_CALIBRATION) == 0U) {
        memset(&status.device_info.calibration, 0, sizeof(status.device_info.calibration));
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_SETTING) == 0U) {
        memset(&status.device_info.setting, 0, sizeof(status.device_info.setting));
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_WIFI) == 0U) {
        memset(&status.device_info.wifi, 0, sizeof(status.device_info.wifi));
    }

    ESP_LOGI(TAG,
             "Report device status mode=%s sections=0x%02x sensors_count=%d include_formula=%d include_statistics=%d",
             reason ? reason : "unknown",
             (unsigned int)effective_section_mask,
             status.device_info.sensors_count,
             include_formula_overall,
             include_statistics);

    if (include_formula_overall &&
        effective_section_mask == MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL &&
        s_next_formula_overall_report_msg_id[0] != '\0') {
        mqtt_copy_string(status.msg_id,
                         sizeof(status.msg_id),
                         s_next_formula_overall_report_msg_id,
                         NULL);
        ESP_LOGI(TAG,
                 "Reuse formulaOverall report msgId=%s for reason=%s",
                 status.msg_id,
                 reason ? reason : "unknown");
        s_next_formula_overall_report_msg_id[0] = '\0';
    }

    publish_device_status_to_mqtt_internal(&status, effective_section_mask);
}

void mqtt_report_device_status(void)
{
    mqtt_report_device_status_internal(MQTT_DEVICE_STATUS_SECTION_MASK_FULL, "full");
}

void mqtt_report_device_status_brief(void)
{
    mqtt_report_device_status_internal(MQTT_DEVICE_STATUS_SECTION_MASK_BRIEF, "brief");
}

void mqtt_report_device_status_sections(uint32_t section_mask, const char *reason)
{
    mqtt_report_device_status_internal(section_mask, reason);
}

void publish_device_status_to_mqtt(const device_status_message_t *status)
{
    publish_device_status_to_mqtt_internal(status,
                                           MQTT_DEVICE_STATUS_SECTION_MASK_FULL);
}

static void publish_device_status_to_mqtt_internal(const device_status_message_t *status,
                                                   uint32_t section_mask)
{
    if (!status)
        return;

    char msg_id[sizeof(status->msg_id)] = {0};
    char version[sizeof(status->version)] = {0};
    char sub_topic[sizeof(status->sub_topic)] = {0};
    bool force_update = formula_store_get_force_update() || mqtt_is_formula_force_update_pending();

    if (status->msg_id[0] != 0)
        mqtt_copy_string(msg_id, sizeof(msg_id), status->msg_id, NULL);
    else
        mqtt_get_now_ms_string(msg_id, sizeof(msg_id));

    mqtt_copy_string(version, sizeof(version), status->version, MQTT_PROTOCOL_VERSION);

    if (status->sub_topic[0] != 0)
        mqtt_copy_string(sub_topic, sizeof(sub_topic), status->sub_topic, NULL);
    else
        snprintf(sub_topic, sizeof(sub_topic), "stateserver/kalerm/iot/status/%s", mqtt_config.client_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msgId", msg_id);
    cJSON_AddStringToObject(root, "version", version);

    char timestamp[32];
    mqtt_get_now_ms_string(timestamp, sizeof(timestamp));
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "subTopic", sub_topic);

    cJSON *device_info = cJSON_CreateObject();

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_SENSORS) != 0U) {
        cJSON *sensors_arr = cJSON_CreateArray();
        for (int i = 0; i < status->device_info.sensors_count; i++)
        {
            cJSON *sensor = cJSON_CreateObject();
            cJSON_AddStringToObject(sensor, "name", status->device_info.sensors[i].name);
            cJSON_AddStringToObject(sensor, "status", status->device_info.sensors[i].status);
            cJSON_AddItemToArray(sensors_arr, sensor);
        }
        cJSON_AddItemToObject(device_info, "sensors", sensors_arr);
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL) != 0U) {
        cJSON *formula_overall = cJSON_CreateObject();
        cJSON_AddNumberToObject(formula_overall, "version", status->device_info.formula_overall.version);
        cJSON_AddBoolToObject(formula_overall, "forceUpdate", force_update);

        cJSON *formula_intel_list = cJSON_CreateArray();
        for (int i = 0; i < status->device_info.formula_overall.formula_intel_list_count; i++)
        {
            cJSON_AddItemToArray(formula_intel_list,
                                 mqtt_create_formula_report_json(&status->device_info.formula_overall.formula_intel_list[i], false));
        }
        cJSON_AddItemToObject(formula_overall, "formulaIntelList", formula_intel_list);

        cJSON *formula_list = cJSON_CreateArray();
        for (int i = 0; i < status->device_info.formula_overall.formula_list_count; i++)
        {
            cJSON_AddItemToArray(formula_list,
                                 mqtt_create_formula_report_json(&status->device_info.formula_overall.formula_list[i], true));
        }
        cJSON_AddItemToObject(formula_overall, "formulaList", formula_list);
        cJSON_AddItemToObject(device_info, "formulaOverall", formula_overall);
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_CLEAN_RESULT) != 0U) {
        cJSON *clean_result = cJSON_CreateObject();
        cJSON_AddNumberToObject(clean_result, "steamPoleCleaning", status->device_info.clean_result.steam_pole_cleaning);
        cJSON_AddNumberToObject(clean_result, "backWash", status->device_info.clean_result.back_wash);
        cJSON_AddNumberToObject(clean_result, "descalingWater", status->device_info.clean_result.descaling_water);
        cJSON_AddItemToObject(device_info, "cleanResult", clean_result);
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_CALIBRATION) != 0U) {
        cJSON *calibration = cJSON_CreateObject();
        cJSON_AddStringToObject(calibration, "powderK", status->device_info.calibration.powder_k);
        cJSON_AddStringToObject(calibration, "powderB", status->device_info.calibration.powder_b);
        cJSON_AddStringToObject(calibration, "liquidK", status->device_info.calibration.liquid_k);
        cJSON_AddStringToObject(calibration, "liquidB", status->device_info.calibration.liquid_b);
        cJSON_AddStringToObject(calibration, "flowMeterAdc", status->device_info.calibration.flow_meter_adc);
        cJSON_AddItemToObject(device_info, "calibration", calibration);
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_STATISTICS) != 0U) {
        cJSON *statistics = cJSON_CreateObject();

        cJSON *material_stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(material_stats, "totalGrind", status->device_info.statistics.material_statistics.total_grind);
        cJSON_AddNumberToObject(material_stats, "totalExtraction", status->device_info.statistics.material_statistics.total_extraction);
        cJSON_AddNumberToObject(material_stats, "steamTime", status->device_info.statistics.material_statistics.steam_time);
        cJSON_AddNumberToObject(material_stats, "totalWater", status->device_info.statistics.material_statistics.total_water);
        cJSON_AddItemToObject(statistics, "materialStatistics", material_stats);

        cJSON *maintain_stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(maintain_stats, "breawingHeadCleaningCount", status->device_info.statistics.maintain_statistics.breawing_head_cleaning_count);
        cJSON_AddNumberToObject(maintain_stats, "descalingCount", status->device_info.statistics.maintain_statistics.descaling_count);
        cJSON_AddNumberToObject(maintain_stats, "steamPoleCleaningCount", status->device_info.statistics.maintain_statistics.steam_pole_cleaning_count);
        cJSON_AddItemToObject(statistics, "maintainStatistics", maintain_stats);

        cJSON *beverage_stats = cJSON_CreateObject();
        cJSON *beverage_data_arr = cJSON_CreateArray();
        for (int i = 0; i < status->device_info.statistics.beverage_statistics.data_count; i++)
        {
            cJSON *period_data = cJSON_CreateObject();
            cJSON_AddNumberToObject(period_data, "period", status->device_info.statistics.beverage_statistics.data[i].period);
            cJSON *data_arr = cJSON_CreateArray();
            for (int j = 0; j < status->device_info.statistics.beverage_statistics.data[i].data_count; j++)
            {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "formulaName", status->device_info.statistics.beverage_statistics.data[i].data[j].formula_name);
                cJSON_AddNumberToObject(item, "drinkId", status->device_info.statistics.beverage_statistics.data[i].data[j].drink_id);
                cJSON_AddNumberToObject(item, "drinkCount", status->device_info.statistics.beverage_statistics.data[i].data[j].drink_count);
                cJSON_AddItemToArray(data_arr, item);
            }
            cJSON_AddItemToObject(period_data, "data", data_arr);
            cJSON_AddItemToArray(beverage_data_arr, period_data);
        }
        cJSON_AddItemToObject(beverage_stats, "data", beverage_data_arr);
        cJSON_AddNumberToObject(beverage_stats, "total", status->device_info.statistics.beverage_statistics.total);
        cJSON_AddItemToObject(statistics, "beverageStatistics", beverage_stats);

        cJSON *clean_stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(clean_stats, "backWashCount", status->device_info.statistics.clean_statistics.back_wash_count);
        cJSON_AddNumberToObject(clean_stats, "backWashTotalCount", status->device_info.statistics.clean_statistics.back_wash_total_count);
        cJSON_AddNumberToObject(clean_stats, "steamPoleCleaningCount", status->device_info.statistics.clean_statistics.steam_pole_cleaning_count);
        cJSON_AddNumberToObject(clean_stats, "steamPoleCleaningTotalCount", status->device_info.statistics.clean_statistics.steam_pole_cleaning_total_count);
        cJSON_AddNumberToObject(clean_stats, "descalingWaterCount", status->device_info.statistics.clean_statistics.descaling_water_count);
        cJSON_AddNumberToObject(clean_stats, "descalingWaterTotalCount", status->device_info.statistics.clean_statistics.descaling_water_total_count);
        cJSON_AddItemToObject(statistics, "cleanStatistics", clean_stats);

        cJSON_AddItemToObject(device_info, "statistics", statistics);
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_SETTING) != 0U) {
        cJSON *setting = cJSON_CreateObject();
        cJSON_AddNumberToObject(setting, "autoPowerOffTime", status->device_info.setting.auto_power_off_time);
        cJSON_AddNumberToObject(setting, "autoStandByTime", status->device_info.setting.auto_stand_by_time);
        cJSON_AddNumberToObject(setting, "voiceTouchTone", status->device_info.setting.voice_touch_tone);
        cJSON_AddNumberToObject(setting, "voicePrompt", status->device_info.setting.voice_prompt);
        cJSON_AddNumberToObject(setting, "voiceVolume", status->device_info.setting.voice_volume);
        cJSON_AddNumberToObject(setting, "autoClearBean", status->device_info.setting.auto_clear_bean);
        cJSON_AddNumberToObject(setting, "steamAutoClean", status->device_info.setting.steam_auto_clean);
        cJSON_AddNumberToObject(setting, "factoryStatus", status->device_info.setting.factory_status);
        cJSON_AddNumberToObject(setting, "autoOta", status->device_info.setting.auto_ota);
        cJSON_AddNumberToObject(setting, "brewWaterVolume", status->device_info.setting.brew_water_volume);
        cJSON_AddNumberToObject(setting, "waterIntakeMode", status->device_info.setting.water_intake_mode);
        cJSON_AddItemToObject(device_info, "setting", setting);
    }

    if ((section_mask & MQTT_DEVICE_STATUS_SECTION_WIFI) != 0U) {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddNumberToObject(wifi, "signal", status->device_info.wifi.signal);
        cJSON_AddStringToObject(wifi, "name", status->device_info.wifi.name);
        cJSON_AddItemToObject(device_info, "wifi", wifi);
    }

    cJSON_AddItemToObject(data, "deviceInfo", device_info);
    cJSON_AddItemToObject(root, "data", data);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL)
    {
        mqtt_publish_message(json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}






