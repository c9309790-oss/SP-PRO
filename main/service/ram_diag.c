#include "ram_diag.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RAM_DIAG";

void ram_diag_snapshot(const char *tag)
{
    const char *name = tag ? tag : "snapshot";
    UBaseType_t stack_words = uxTaskGetStackHighWaterMark(NULL);
    const char *task_name = pcTaskGetName(NULL);

    ESP_LOGI(TAG,
             "[RAM] tag=%s total_free=%u total_min=%u internal_free=%u internal_min=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_min=%u psram_largest=%u",
             name,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG,
             "[RAM_TASK] tag=%s task=%s stack_high_water=%u words",
             name,
             task_name ? task_name : "unknown",
             (unsigned)stack_words);
}
