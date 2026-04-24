/**
 * @file opus_player_test.c
 * @brief Opus 播放器功能测试组件实现
 */

#include "opus_player_test.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"

// 引入 opus_player 组件头文件
#include "opus_player.h"
#include "opus_player_common.h"

static const char *TAG = "opus_player_test";

// 配置宏
#define ENABLE_WIFI 0
#define WIFI_SSID "klm-rdtest"
#define WIFI_PASSWORD "klm@2024"
#define SERVER_URL "http://192.168.18.85:8080"

#if ENABLE_WIFI
/**
 * @brief WiFi 事件处理
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi 已断开，正在重试...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/**
 * @brief 初始化 WiFi
 */
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi 初始化完成。");
}
#endif

/**
 * @brief Opus 播放器测试任务函数
 */
static void opus_player_test_task(void *arg)
{
    ESP_LOGI(TAG, "Opus 播放器测试任务已启动");

#if ENABLE_WIFI
    wifi_init_sta();
#endif

    // 1. 获取播放器实例
    const opus_player_t *player = opus_player_get_instance();
    if (!player) {
        ESP_LOGE(TAG, "获取播放器实例失败");
        vTaskDelete(NULL);
        return;
    }

    // 2. 配置参数
    opus_player_config_t config = {
        .i2c_sda_io = 18,//
        .i2c_scl_io = 17,//
        .i2c_addr = 0x30,
        
        // I2S 配置
        .i2s_mclk_io = 38,//
        .i2s_bclk_io = 47,//
        .i2s_ws_io = 21,//
        .i2s_dout_io = 13,//
        .i2s_din_io = 0,
        
        // PA 配置
        .pa_gpio = 14,
        
        // 服务器配置
        .server_url = SERVER_URL,

        // 播放参数
        .volume = 90,
        
        // 回调函数
        .on_play_start = NULL,
        .on_play_finish = NULL,
        .on_play_error = NULL
    };

    // 3. 初始化播放器
    ESP_LOGI(TAG, "正在初始化 Opus 播放器...");
    esp_err_t ret = player->init_ex(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放器初始化失败: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    // 4. 手动触发同步
#if ENABLE_WIFI
    // 等待网络连接 (简单延时，确保 WiFi 已连接)
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "开始手动同步...");
    ret = player->sync();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "同步成功完成");
    } else {
        ESP_LOGW(TAG, "同步失败或无更新");
    }
#else
    ESP_LOGI(TAG, "WiFi 未启用，跳过同步");
#endif

    // 5. 获取文件列表并循环播放
    while (1) {
        opus_player_file_info_t *files = NULL;
        size_t count = 0;
        
        // 分配临时缓冲区用于获取文件列表
        files = heap_caps_malloc(sizeof(opus_player_file_info_t) * 20, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!files) {
            files = heap_caps_malloc(sizeof(opus_player_file_info_t) * 20, MALLOC_CAP_8BIT);
        }

        if (files) {
            ret = player->get_file_list(files, 20, &count);
            if (ret == ESP_OK && count > 0) {
                ESP_LOGI(TAG, "找到 %d 个文件，开始循环播放...", count);
                
                for (size_t i = 0; i < count; i++) {
                    ESP_LOGI(TAG, "正在播放 [%d/%d]: %s", i + 1, count, files[i].name);
                    
                    // 播放文件 (阻塞调用)
                    player->play(files[i].name, true);
                    
                    ESP_LOGI(TAG, "播放结束，等待下一首...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            } else {
                ESP_LOGW(TAG, "本地未找到文件");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            heap_caps_free(files);
        } else {
            ESP_LOGE(TAG, "分配文件列表内存失败");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(NULL);
}

void start_opus_player_test(void)
{
    // 创建 Opus 播放器测试任务
    // 栈大小设为 8192，优先级设为 5
    BaseType_t ret = xTaskCreate(opus_player_test_task, "test_opus", 8192, NULL, 5, NULL);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "Opus 播放器测试任务已创建");
    } else {
        ESP_LOGE(TAG, "创建 Opus 播放器测试任务失败");
    }
}
