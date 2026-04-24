#include "ble_mesh_scale_client_mod.h"
#include "ble_mesh_client_drv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_ble_mesh_sensor_model_api.h" 
#include "sysdef.h"
#include "mqtt.h"  
 
#include "cJSON.h" 
#define TAG "SCALE_CLIENT_MOD"
#define SENSOR_PROPERTY_WEIGHT 0x2A98 // Modified to match received Property ID

#define BUTTON_GPIO         GPIO_NUM_0
#define SCALE_GROUP_ADDR    0xC001
#define APP_KEY_INDEX       0
#define MAX_ONOFF_RETRY     5
#define FLAG_STABLE_BIT     (1 << 0)

static bool current_onoff_state = false;
static uint8_t onoff_retry_count = 0;
static uint16_t scale_mesh_address = 0xffff;    //秤的mesh地址  0xffff为未找到秤



static void scale_onoff_status_handler(uint16_t src_addr, bool onoff)
{
    ESP_LOGI(TAG, "OnOff Set successful, status from 0x%04x: is %s", src_addr, onoff ? "ON" : "OFF");
    current_onoff_state = onoff;
    onoff_retry_count = 0;
}

static void scale_onoff_timeout_handler(uint16_t dst_addr)
{
    ESP_LOGW(TAG, "OnOff Set TIMEOUT for 0x%04x", dst_addr);
    if (onoff_retry_count < MAX_ONOFF_RETRY) {
        onoff_retry_count++;
        ESP_LOGW(TAG, "Retrying OnOff Set %s... (Attempt %d)", !current_onoff_state ? "ON" : "OFF", onoff_retry_count);
        ble_mesh_client_drv_send_onoff(dst_addr, APP_KEY_INDEX, !current_onoff_state);
    } else {
        ESP_LOGE(TAG, "Max retries reached for OnOff Set. Giving up.");
        onoff_retry_count = 0;
    }
}

static void scale_sensor_data_handler(uint16_t src_addr, uint16_t dst_addr, struct net_buf_simple *data)
{
    if (data == NULL || data->len < 10) {
        ESP_LOGW(TAG, "Data length too short. Len: %d. Expected >= 10 for custom format.", data->len);
        if(data && data->len > 0) {
             ESP_LOGI(TAG, "Raw Data (Len %d):", data->len);
             esp_log_buffer_hex(TAG, data->data, data->len);
        }
        return;
    }

    uint8_t *raw_bytes = data->data;
    ESP_LOGI(TAG, "Raw Data (Len %d):", data->len);
    esp_log_buffer_hex(TAG, raw_bytes, data->len);

    // Custom Server Format:
    // Byte 0: 0x89 (Format B, Len 9)
    // Byte 1-2: Property ID (Little Endian)
    // Byte 3: Frame Count
    // Byte 4-5: Version (Little Endian)
    // Byte 6-9: Net Weight (float)
    
    // 1. Check Header
    // if (raw_bytes[0] != 0x89) { ESP_LOGW(TAG, "Unexpected header: 0x%02X", raw_bytes[0]); }
    // We proceed anyway to try parsing
    
    uint16_t property_id = (uint16_t)raw_bytes[1] | ((uint16_t)raw_bytes[2] << 8);

    if (scale_mesh_address != src_addr) 
    {
        if(scale_mesh_address != 0xffff)
        {
            ble_mesh_client_drv_unregister_onoff_status_cb(scale_mesh_address, scale_onoff_status_handler);
            ble_mesh_client_drv_unregister_onoff_timeout_cb(scale_mesh_address, scale_onoff_timeout_handler);
        }
        ble_mesh_client_drv_register_onoff_status_cb(src_addr, scale_onoff_status_handler);
        ble_mesh_client_drv_register_onoff_timeout_cb(src_addr, scale_onoff_timeout_handler);
        scale_mesh_address = src_addr;
        ESP_LOGI(TAG, "Scale mesh address found: 0x%04x", scale_mesh_address);
    }

    if (property_id != SENSOR_PROPERTY_WEIGHT) {
        ESP_LOGW(TAG, "Received data for unexpected Property ID: 0x%04X (Expected: 0x%04X)", property_id, SENSOR_PROPERTY_WEIGHT);
        return;
    }

    uint8_t frame_count = raw_bytes[3];
    uint16_t version = (uint16_t)raw_bytes[4] | ((uint16_t)raw_bytes[5] << 8);
    
    float weight_f;
    memcpy(&weight_f, &raw_bytes[6], 4);
    
    ESP_LOGI(TAG, "Sensor data from 0x%04x: Frame=%u, Ver=0x%04X, Weight=%.2f g",
             src_addr, frame_count, version, weight_f);
}

/* 按鍵任務 */
static void button_task_func(void *arg)
{
    uint32_t press_start_time = 0;
    bool action_triggered = false;
    while (1) {
        if (gpio_get_level(BUTTON_GPIO) == 0) { 
            if (press_start_time == 0) {
                press_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            } else if (!action_triggered) {
                uint32_t press_duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - press_start_time;
                if (press_duration >= 100) {
                    if(scale_mesh_address == 0xffff)
                    {
                        ESP_LOGW(TAG, "No scale found to control");
                    }
                    else if(onoff_retry_count == 0)
                    {
                        ble_mesh_client_drv_send_onoff(scale_mesh_address, APP_KEY_INDEX, !current_onoff_state);
                        ESP_LOGW(TAG, "Scale OnOff Set %s", !current_onoff_state ? "ON" : "OFF");
                        onoff_retry_count = 1;
                    }
                    action_triggered = true;
                }
            }
        } else {
            press_start_time = 0;
            action_triggered = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* 初始化函式 */
void ble_mesh_scale_client_mod_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, 
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ble_mesh_client_drv_register_sensor_cb(SCALE_GROUP_ADDR, scale_sensor_data_handler);

    xTaskCreate(button_task_func, "button_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Scale Client Mod Initialized.");
}
