#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "mqtt.h"
#include "mqtt_protocol_core.h"

version_info_t g_device_version = {
    .device_type = "SP1Pro",
    .current_ctr_fw_version = "0.0.1",
    .iot_fun_version = "F516ahnGhgb1C4tI",
    .iot_cfg_func_version = "2T95GnJ2s9Ne92TJ",
    .grinder_group_version = "0"
};

void mqtt_get_hmi_version_from_app(char *buffer, size_t size)
{
    if (!buffer || size == 0) {
        return;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *app_version = "0.0.1";

    if (app_desc && app_desc->version[0] != '\0') {
        app_version = app_desc->version;
    }

    snprintf(buffer,
             size,
             "%.*s",
             (int)size - 1,
             app_version);
}

void mqtt_get_now_ms_string(char *buffer, size_t size)
{
    int64_t ms = esp_timer_get_time() / 1000;
    snprintf(buffer, size, "%lld", ms);
}

void mqtt_copy_string(char *dst, size_t size, const char *src, const char *fallback)
{
    const char *value = (src && src[0] != 0) ? src : fallback;
    if (!value)
        value = "";

    snprintf(dst, size, "%s", value);
}

void mqtt_build_client_topic(char *buffer, size_t size, const char *topic_pattern)
{
    if (!buffer || size == 0)
        return;

    snprintf(buffer, size, topic_pattern, mqtt_config.client_id);
}

cJSON *mqtt_create_message_root(const char *msg_id, const char *timestamp, cJSON **data_out)
{
    char msg_id_buffer[32] = {0};
    char timestamp_buffer[32] = {0};
    const char *msg_id_value = msg_id;
    const char *timestamp_value = timestamp;
    cJSON *root = cJSON_CreateObject();

    if (!root)
        return NULL;

    if (!msg_id_value || msg_id_value[0] == 0)
    {
        mqtt_get_now_ms_string(msg_id_buffer, sizeof(msg_id_buffer));
        msg_id_value = msg_id_buffer;
    }

    if (!timestamp_value)
    {
        mqtt_get_now_ms_string(timestamp_buffer, sizeof(timestamp_buffer));
        timestamp_value = timestamp_buffer;
    }

    cJSON_AddStringToObject(root, "msgId", msg_id_value);
    cJSON_AddStringToObject(root, "version", MQTT_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "timestamp", timestamp_value);

    if (data_out)
    {
        *data_out = cJSON_CreateObject();
        if (!*data_out)
        {
            cJSON_Delete(root);
            return NULL;
        }

        cJSON_AddItemToObject(root, "data", *data_out);
    }

    return root;
}

void mqtt_add_sub_topic(cJSON *data, const char *sub_topic)
{
    if (!data || !sub_topic || sub_topic[0] == 0)
        return;

    cJSON_AddStringToObject(data, "subTopic", sub_topic);
}

void mqtt_publish_json_root(cJSON *root)
{
    if (!root)
        return;

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL)
    {
        mqtt_publish_message(json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

int json_get_int(cJSON *obj, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item))
        return item->valueint;
    return def;
}

const char *json_get_string(cJSON *obj, const char *key, const char *def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item))
        return item->valuestring;
    return def;
}

cJSON *json_get_object(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsObject(item))
        return item;
    return NULL;
}

cJSON *json_get_array(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsArray(item))
        return item;
    return NULL;
}

long long json_get_int64(cJSON *obj, const char *key, long long def)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item))
        return (long long)item->valuedouble;
    return def;
}

char *mqtt_strdup(const char *src)
{
    size_t len = src ? strlen(src) : 0;
    char *copy = calloc(len + 1, sizeof(char));
    if (!copy)
        return NULL;

    if (src)
        snprintf(copy, len + 1, "%s", src);

    return copy;
}

void mqtt_set_ack_result(mqtt_ack_result_t *ack_result, int ack_code, const char *msg)
{
    if (!ack_result)
        return;

    ack_result->ack_code = ack_code;
    snprintf(ack_result->msg, sizeof(ack_result->msg), "%s", msg ? msg : "");
}

cJSON *mqtt_parse_message_root(const char *payload,
                               int len,
                               const char **msg_id_out,
                               cJSON **data_out)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root)
    {
        ESP_LOGE("MQTT_PROTOCOL", "Failed to parse MQTT JSON payload");
        return NULL;
    }

    cJSON *data = json_get_object(root, "data");
    if (!data)
    {
        cJSON_Delete(root);
        return NULL;
    }

    if (msg_id_out)
        *msg_id_out = json_get_string(root, "msgId", NULL);

    if (data_out)
        *data_out = data;

    return root;
}

bool mqtt_has_ota_message(cJSON *data)
{
    return data &&
           (cJSON_GetObjectItem(data, "remoteUpdate") ||
            cJSON_GetObjectItem(data, "confirmUpdate") ||
            cJSON_GetObjectItem(data, "softwareUpdateResultAck"));
}
