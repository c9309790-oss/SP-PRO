#ifndef IOT_MQTT_PROTOCOL_H
#define IOT_MQTT_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>
#include "service_domain_types.h"
#include "controller_status_types.h"

/* Test-only MQTT internal command gate. Set to 0 for production builds. */
#ifndef MQTT_INTERNAL_TEST_ENABLE
#define MQTT_INTERNAL_TEST_ENABLE 0
#endif

/* Control types and command codes. */
typedef enum {
    CMD_TYPE_OTHER  = 2,
    CMD_TYPE_STATUS = 3,
    CMD_TYPE_MASTER = 8,
    CMD_TYPE_GRIND  = 9,
} cmd_type_t;

/* Command codes are defined as strings in the protocol. */
#define CMD_NO_REMOTE_POWER_ON       "532"
#define CMD_NO_REMOTE_POWER_OFF      "533"
#define CMD_NO_FACTORY_RESET         "534"
#define CMD_NO_RESET_DRINKS          "537"
#define CMD_NO_CHILD_LOCK            "538"
#define CMD_NO_REMOTE_PARAM_SETTING  "540"
#define CMD_NO_INTERNAL_TEST         "599"
#define CMD_NO_STATUS_REPORT         "1"

/* Setting types for CMD_NO_REMOTE_PARAM_SETTING (540). */
typedef enum {
    SETTING_TYPE_AUTO_POWER_OFF = 100,
    SETTING_TYPE_AUTO_STANDBY = 101,
    SETTING_TYPE_SOUND = 103,
    SETTING_TYPE_AUTO_CLEAR_BEAN = 105,
    SETTING_TYPE_STEAM_AUTO_CLEAN = 106,
    SETTING_TYPE_AUTO_OTA = 108,
    SETTING_TYPE_BREW_WATER_VOLUME = 109,
    SETTING_TYPE_WATER_INTAKE_MODE = 110,
    SETTING_TYPE_CLEAR_WATERWAY = 111,
    SETTING_TYPE_REMOTE_CLEAN = 112,
    SETTING_TYPE_CANCEL_GRIND = 113,
    SETTING_TYPE_CANCEL_EXTRACTION = 114,
    SETTING_TYPE_CANCEL_BREW_CLEAN = 115,
    SETTING_TYPE_BREW_CLEAN = 116,
} setting_type_t;

typedef enum {
    CLEANING_MODE_BREW = 0,
    CLEANING_MODE_STEAM = 1,
    CLEANING_MODE_DESCALING = 2,
} clean_mode_t;

typedef enum {
    OTA_RESULT_SUCCESS = 0,
    OTA_RESULT_FAILED = 1,
    OTA_RESULT_DOWNLOAD_OK = 10,
    OTA_RESULT_DOWNLOAD_ERR = 11,
    OTA_RESULT_VERIFY_ERR = 12,
    OTA_RESULT_BURN_ERR = 13,
    OTA_RESULT_VERSION_ERR = 14
} ota_result_t;

typedef enum {
    OTA_WAIT_CONFIRM = 0,
    OTA_YES_CONFIRM  = 1,
} ota_status_t;

/* Shared payload types. */
typedef struct {
    char name[40];
    char status[16];
} sensor_info_t;

typedef struct {
    char powder_k[16];
    char powder_b[16];
    char liquid_k[16];
    char liquid_b[16];
    char flow_meter_adc[16];
} calibration_info_t;

typedef struct {
    int auto_power_off_time;
    int auto_stand_by_time;
    int voice_touch_tone;
    int voice_prompt;
    int voice_volume;
    int auto_clear_bean;
    int steam_auto_clean;
    int factory_status;
    int auto_ota;
    int brew_water_volume;
    int water_intake_mode;
} setting_info_t;

typedef struct {
    int signal;
    char name[32];
} wifi_info_t;

typedef struct {
    sensor_info_t *sensors;
    int sensors_count;
    formula_overall_t formula_overall;
    clean_result_info_t clean_result;
    calibration_info_t calibration;
    statistics_info_t statistics;
    setting_info_t setting;
    wifi_info_t wifi;
} device_info_t;

typedef struct {
    char msg_id[64];
    char version[16];
    char timestamp[32];
    char sub_topic[128];
    device_info_t device_info;
} device_status_message_t;

typedef enum {
    MQTT_DEVICE_STATUS_SECTION_SENSORS         = (1U << 0),
    MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL = (1U << 1),
    MQTT_DEVICE_STATUS_SECTION_CLEAN_RESULT    = (1U << 2),
    MQTT_DEVICE_STATUS_SECTION_CALIBRATION     = (1U << 3),
    MQTT_DEVICE_STATUS_SECTION_STATISTICS      = (1U << 4),
    MQTT_DEVICE_STATUS_SECTION_SETTING         = (1U << 5),
    MQTT_DEVICE_STATUS_SECTION_WIFI            = (1U << 6),
    MQTT_DEVICE_STATUS_SECTION_SENSOR_TASKSTATUS_ONLY = (1U << 7),
} mqtt_device_status_section_t;

#define MQTT_DEVICE_STATUS_SECTION_MASK_BRIEF \
    (MQTT_DEVICE_STATUS_SECTION_SENSORS | MQTT_DEVICE_STATUS_SECTION_CALIBRATION | \
     MQTT_DEVICE_STATUS_SECTION_SETTING | MQTT_DEVICE_STATUS_SECTION_WIFI)

#define MQTT_DEVICE_STATUS_SECTION_MASK_FULL \
    (MQTT_DEVICE_STATUS_SECTION_MASK_BRIEF | MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL | \
     MQTT_DEVICE_STATUS_SECTION_CLEAN_RESULT | MQTT_DEVICE_STATUS_SECTION_STATISTICS)

typedef struct {
    char name[40];
    int status;
} ack_sensor_info_t;

typedef struct {
    int ack_code;
    char msg[128];
} mqtt_ack_result_t;

typedef struct {
    char item_type[32];
    char item_language[32];
    char item_type_name[64];
    char item_title[128];
    char item_cover[256];
    long long item_cover_size;
    char item_url[256];
    long long item_size;
    char item_md5[64];
} mqtt_resource_item_t;

typedef struct {
    int resource_type;
    char device_type[32];
    long long latest_update_time;
    mqtt_resource_item_t *resource_item_list;
    int resource_item_list_count;
} mqtt_resource_response_t;

typedef struct {
    char ack_code[16];
    char **resource_url_list;
    int resource_url_count;
} mqtt_resource_retry_response_t;

typedef struct
{
    char device_type[32];
    char current_ctr_fw_version[16];
    char iot_fun_version[32];
    char iot_cfg_func_version[32];
    char grinder_group_version[8];
} version_info_t;

/* Callback types. */
typedef bool (*mqtt_device_status_provider_t)(device_status_message_t *status);
typedef bool (*mqtt_formula_overall_handler_t)(const formula_overall_t *formula_overall, mqtt_ack_result_t *ack_result);
typedef void (*mqtt_resource_response_handler_t)(const mqtt_resource_response_t *response);
typedef void (*mqtt_resource_retry_handler_t)(const mqtt_resource_retry_response_t *response);

extern version_info_t g_device_version;

/* Publish helpers. */
void update_ctr_version(const char *version);
void publish_ota_info_ack_to_mqtt(const char *msg_id, const char *task_id);
void publish_ota_result_to_mqtt(ota_result_t result, const char *msg);
bool publish_last_ota_result_to_mqtt(void);
void publish_confirm_ack_to_mqtt(const char *msg_id, int ack_code);
void publish_ota_status_to_mqtt(ota_status_t status);
void publish_ctr_ota_check_status_to_mqtt(void);
void publish_device_status_to_mqtt(const device_status_message_t *status);
void publish_formula_overall_ack_to_mqtt(const char *msg_id, int ack_code, const char *msg);
void publish_drink_ack_to_mqtt(const char *msg_id, int ack_code, const ack_sensor_info_t *sensors, int sensors_count, const char *msg);
void publish_cleaning_ack_to_mqtt(const char *msg_id, int ack_code, const int *results, int results_count, const char *msg);
void publish_version_info_to_mqtt(version_info_t *ver);
void publish_drink_record_to_mqtt(drink_record_t *rec);
void publish_extraction_curve_records_to_mqtt(extraction_curve_record_t *records, int record_count, bool curve_update);
void publish_event_record_to_mqtt(event_record_t *rec);
void publish_log_info_to_mqtt(const char *log_detail);
void publish_resource_request_to_mqtt(int resource_type, const char *language);
void publish_resource_fail_to_mqtt(const char *const *resource_fail_list, int resource_fail_count);

/* Message handling. */
void handle_mqtt_message_from_mqtt(const char *payload, int len);

/* Runtime hooks. */
bool mqtt_start_drink_action(formula_info_t *formula);
void mqtt_register_device_status_provider(mqtt_device_status_provider_t provider);
void mqtt_register_formula_overall_handler(mqtt_formula_overall_handler_t handler);
void mqtt_register_resource_response_handler(mqtt_resource_response_handler_t handler);
void mqtt_register_resource_retry_handler(mqtt_resource_retry_handler_t handler);
void mqtt_report_device_status(void);
void mqtt_report_device_status_brief(void);
void mqtt_report_device_status_sections(uint32_t section_mask, const char *reason);
bool mqtt_request_immediate_device_status_sections_report(uint32_t section_mask, const char *reason);
bool mqtt_schedule_device_status_sections_report(uint32_t section_mask, const char *reason, uint32_t delay_ms);
void mqtt_set_next_formula_overall_report_msg_id(const char *msg_id);
const setting_info_t *mqtt_get_runtime_setting(void);
int mqtt_get_task_status(void);
void mqtt_sync_runtime_setting_from_device(void);
void mqtt_normalize_runtime_setting(void);
bool mqtt_save_runtime_setting(void);
void mqtt_set_formula_force_update_pending(bool pending);
bool mqtt_is_formula_force_update_pending(void);
void mqtt_restore_local_defaults(void);
void mqtt_track_remote_controller_completion(const char *msg_id, int setting_type);
void mqtt_clear_remote_controller_completion(int setting_type);
void mqtt_notify_remote_controller_complete(int setting_type, bool success);
void mqtt_handle_remote_maintenance_machine_status(const MACHINE_STATUS *status);
void mqtt_mark_clean_result_incomplete(int cleaning_mode);
void mqtt_notify_clean_result_complete(int cleaning_mode);

/* Memory helpers. */
void mqtt_free_formula_overall(formula_overall_t *formula_overall);
void mqtt_free_resource_response(mqtt_resource_response_t *response);
void mqtt_free_resource_retry_response(mqtt_resource_retry_response_t *response);

#endif /* IOT_MQTT_PROTOCOL_H */
