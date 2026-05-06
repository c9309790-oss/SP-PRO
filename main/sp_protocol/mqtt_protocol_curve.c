#include <stdlib.h>

#include "mqtt_protocol_core.h"
#include "extraction_curve_service.h"

static const char *TAG = "MQTT_CURVE";

bool mqtt_handle_extraction_curve_ack_data_from_mqtt(cJSON *data)
{
#if !EXTRACTION_CURVE_FEATURE_ENABLE
    (void)data;
    return false;
#else
    cJSON *curve_ids;
    size_t count;
    uint32_t *ids;

    if (!data) {
        return false;
    }

    curve_ids = json_get_array(data, "curveIds");
    if (!curve_ids) {
        return false;
    }

    count = (size_t)cJSON_GetArraySize(curve_ids);
    if (count == 0U) {
        ESP_LOGI(TAG, "curve ack received with empty curveIds");
        return true;
    }

    ids = calloc(count, sizeof(uint32_t));
    if (!ids) {
        ESP_LOGE(TAG, "curve ack alloc failed count=%u", (unsigned int)count);
        return true;
    }

    for (size_t i = 0U; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(curve_ids, (int)i);
        if (cJSON_IsNumber(item)) {
            ids[i] = (uint32_t)item->valuedouble;
        }
    }

    ESP_LOGI(TAG, "curve ack received count=%u", (unsigned int)count);
    extraction_curve_handle_ack(ids, count);
    free(ids);
    return true;
#endif
}
