#ifndef MQTT_PROTOCOL_CORE_H
#define MQTT_PROTOCOL_CORE_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_protocol.h"

#define MQTT_PROTOCOL_VERSION "H-1.0"
#define MQTT_STATUS_SENSOR_MAX 30
#define MQTT_TASK_STATUS_SLEEP 3
#define MQTT_DEBUG_DUMP 0

extern setting_info_t mqtt_runtime_setting;
extern version_info_t g_device_version;

void mqtt_get_hmi_version_from_app(char *buffer, size_t size);
void mqtt_get_now_ms_string(char *buffer, size_t size);
void mqtt_copy_string(char *dst, size_t size, const char *src, const char *fallback);
void mqtt_build_client_topic(char *buffer, size_t size, const char *topic_pattern);
cJSON *mqtt_create_message_root(const char *msg_id, const char *timestamp, cJSON **data_out);
void mqtt_add_sub_topic(cJSON *data, const char *sub_topic);
void mqtt_publish_json_root(cJSON *root);

int json_get_int(cJSON *obj, const char *key, int def);
const char *json_get_string(cJSON *obj, const char *key, const char *def);
cJSON *json_get_object(cJSON *obj, const char *key);
cJSON *json_get_array(cJSON *obj, const char *key);
long long json_get_int64(cJSON *obj, const char *key, long long def);
char *mqtt_strdup(const char *src);
void mqtt_set_ack_result(mqtt_ack_result_t *ack_result, int ack_code, const char *msg);

cJSON *mqtt_parse_message_root(const char *payload,
                               int len,
                               const char **msg_id_out,
                               cJSON **data_out);
bool mqtt_has_ota_message(cJSON *data);

bool mqtt_handle_ota_data_from_mqtt(const char *msg_id, cJSON *data);
bool mqtt_handle_formula_ack_data_from_mqtt(cJSON *data);
bool mqtt_handle_formula_setting_data_from_mqtt(const char *msg_id, cJSON *data);
bool mqtt_parse_player_formula_from_json(cJSON *param, formula_info_t *formula);
bool mqtt_handle_resource_data_from_mqtt(cJSON *data);
bool mqtt_handle_control_data_from_mqtt(const char *msg_id, cJSON *data);

#endif /* MQTT_PROTOCOL_CORE_H */
