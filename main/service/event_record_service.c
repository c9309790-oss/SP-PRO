#include "event_record_service.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "record_publish_port.h"

#define EVENT_RECORD_LOGIC_TASK_MS 50U

static const char *TAG = "EVENT_RECORD";

static void event_record_publish_single(const char *key, int value)
{
    event_record_t record = {0};

    if (!key) {
        return;
    }

    snprintf(record.events[0].key, sizeof(record.events[0].key), "%s", key);
    snprintf(record.events[0].value, sizeof(record.events[0].value), "%d", value);
    record.event_cnt = 1;

    ESP_LOGI(TAG, "publish event key=%s value=%d", key, value);
    record_publish_event(&record);
}

void event_record_publish_brew_cleaning(int ml)
{
    if (ml < 0) {
        ml = 0;
    }

    event_record_publish_single("brewCleaning", ml);
}

void event_record_publish_steam_cleaning(int ml)
{
    if (ml < 0) {
        ml = 0;
    }

    event_record_publish_single("steamCleaning", ml);
}

void event_record_publish_empty_water(int ml)
{
    if (ml < 0) {
        ml = 0;
    }

    event_record_publish_single("emptyWater", ml);
}

void event_record_publish_descaling_ticks(uint32_t ticks)
{
    uint32_t seconds = (ticks * EVENT_RECORD_LOGIC_TASK_MS) / 1000U;
    event_record_publish_single("descaling", (int)seconds);
}
