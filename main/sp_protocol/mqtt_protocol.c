#include "mqtt_protocol_core.h"

static const char *TAG = "MQTT_PROTOCOL";

static bool mqtt_handle_device_data_from_mqtt(const char *msg_id, cJSON *data)
{
    if (mqtt_handle_formula_ack_data_from_mqtt(data))
        return true;

    if (mqtt_handle_formula_setting_data_from_mqtt(msg_id, data))
        return true;

    if (mqtt_handle_resource_data_from_mqtt(data))
        return true;

    if (mqtt_handle_control_data_from_mqtt(msg_id, data))
        return true;

    return false;
}

void handle_mqtt_message_from_mqtt(const char *payload, int len)
{
    const char *msg_id = NULL;
    cJSON *data = NULL;
    cJSON *root = mqtt_parse_message_root(payload, len, &msg_id, &data);
    if (!root)
        return;

    if (mqtt_handle_ota_data_from_mqtt(msg_id, data) ||
        mqtt_handle_device_data_from_mqtt(msg_id, data))
    {
        cJSON_Delete(root);
        return;
    }

    ESP_LOGW(TAG, "Unhandled MQTT message payload");
    cJSON_Delete(root);
}


