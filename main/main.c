#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "mbedtls/ssl_ciphersuites.h"
#include <string.h>
#include "cJSON.h"
#include "board_config.h"
#include "system_runtime.h"
#include "formula_store.h"
#include "device_statistics_store.h"
#include "ble.h"
#include "factory_cfg.h"
#include "mqtt.h"
#include "mqtt_protocol_core.h"
#include "ota_ctr.h"
#include "ram_diag.h"
#include "spiffs.h"
#include "sp_pro_app.h"
#include "uart_ctr.h"
#include "uart_hmi.h"
#include "wifi.h"
#include "controller_status_types.h"

static const char *TAG = "main";
extern FLASH_FACTORY_DATA factory_data;

static void *cjson_psram_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size,
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_8BIT);
}

static void cjson_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

static void init_cjson_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_psram_malloc,
        .free_fn = cjson_psram_free,
    };

    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON hooks initialized: prefer PSRAM allocations");
}

SYSTEM_PARA_TYP sys_pra = {
    .wifi_state = WIFI_DISCONNECTED,
    .mqtt_state = MQTT_UNINITIALIZED,
    .app_mode = APP_MODE_BT_PAIRING,
};

const int klm_ETSI_EN_303645_ciphersuites_list[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    0
};

/* Shared RX buffer used by bridge handlers for UART/BLE text packets. */
EXT_RAM_BSS_ATTR char rec_pack[MAX_OF_FOUR(KLM_UART_BUF_SIZE,
                                           KLM_MQTT_RX_BUF_SIZE,
                                           KLM_BLE_RX_BUF_SIZE,
                                           KLM_TCP_RX_BUF_SIZE) + KLM_MSG_HEAD_LEN_MAX];

/* HMI UART queue carrying raw text commands. */
extern QueueHandle_t uart_rx_queue[1];

static void app_mode_handle_hmi_uart_recv(void)
{
    static uint8_t cmd_mode = 0;
    static TickType_t cmd_mode_start_tick = 0;

    if (xQueueReceive(uart_rx_queue[0], rec_pack, 1) != pdPASS) {
        return;
    }

    // ESP_LOGI(TAG, "uart rec:%s", rec_pack);

    if (cmd_mode == 0U) {
        if (strncmp(rec_pack, "+++", 3) == 0) {
            uart_send_str("a");
            cmd_mode = 1;
            cmd_mode_start_tick = xTaskGetTickCount();
        }
        return;
    }

    if ((xTaskGetTickCount() - cmd_mode_start_tick) > pdMS_TO_TICKS(5000)) {
        cmd_mode = 0;
        ESP_LOGI(TAG, "Cmd mode timeout, exit");
        return;
    }

    if (strncmp(rec_pack, "a\r", 2) == 0) {
        uart_send_str("+ok\r\n");
        cmd_mode = 0;
    } else {
        cmd_mode = 0;
        ESP_LOGW(TAG, "Invalid cmd mode input, exit");
    }
}

static void __attribute__((unused)) app_mode_handle_ble_recv(void)
{
    memset(rec_pack, 0, sizeof(rec_pack));
    if (!ble_rx_queue) {
        return;
    }
    if (xQueueReceive(ble_rx_queue, rec_pack, 0) == pdPASS) {
        ESP_LOGI(TAG, "ble rec:%s", rec_pack);
        uart_send_str(rec_pack);
    }
}

static void app_mode_handle_mqtt_config(void)
{
    if (ctr_ota_should_block_mqtt_autostart()) {
        return;
    }

    if ((sys_pra.mqtt_state == MQTT_UNINITIALIZED || sys_pra.mqtt_state == MQTT_DISCONNECTED) &&
        sys_pra.wifi_state == WIFI_CONNECTED) {
        if (ble_app_is_connected()) {
            return;
        }
        (void)mqtt_app_start();
    }
}

static void app_mode_handle_ble_mesh_config(void)
{
    ESP_LOGI(TAG, "APP_MODE_BLE_MESH_CONFIG -> APP_MODE_RUNTIME_BRIDGE");
    /* BLE Mesh initialization can be restored here if this mode is used again. */
    sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
}

static void app_mode_handle_runtime_bridge(void)
{
    /* HMI text commands are still handled here. MQTT commands now go directly
     * from mqtt.c into mqtt_protocol, so main.c no longer needs a second MQTT
     * queue bridge. */
    app_mode_handle_hmi_uart_recv();
}

static void app_mode_service(void)
{
    switch (sys_pra.app_mode) {
    case APP_MODE_YMODEM:
        return;

    case APP_MODE_BT_PAIRING:
        app_mode_handle_ble_recv();
        break;

    case APP_MODE_MQTT_CONFIG:
        app_mode_handle_mqtt_config();
        break;

    case APP_MODE_BLE_MESH_CONFIG:
        app_mode_handle_ble_mesh_config();
        break;

    case APP_MODE_RUNTIME_BRIDGE:
        app_mode_handle_runtime_bridge();
        break;

    default:
        break;
    }
}

#define TASK_INFO_BUFFER_SIZE 2048
static EXT_RAM_BSS_ATTR char taskListBuffer[TASK_INFO_BUFFER_SIZE];

char *print_task_stack_info(void)
{
    taskListBuffer[0] = '\0';
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    vTaskList(taskListBuffer);
    ESP_LOGW(TAG, "Task List:\n%s\n", taskListBuffer);
#else
    snprintf(taskListBuffer, sizeof(taskListBuffer),
             "Task list unavailable: CONFIG_FREERTOS_USE_TRACE_FACILITY is disabled");
    ESP_LOGW(TAG, "%s", taskListBuffer);
#endif

    uint32_t remain_heap = esp_get_free_heap_size() / 1024;
    ESP_LOGW(TAG, "Remaining heap tight:%lu k", remain_heap);
    snprintf(&taskListBuffer[strlen(taskListBuffer)],
             sizeof(taskListBuffer) - strlen(taskListBuffer),
             " Remaining heap tight:%lu k",
             remain_heap);
    return taskListBuffer;
}

void bsp_init(void)
{
    ESP_LOGI(TAG, "app_main start");
    ram_diag_snapshot("boot/bsp_init_start");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    (void)factory_cfg_init();
    factory_cfg_log_current_sn();
    ram_diag_snapshot("boot/bsp_init_done");
}

static void sync_factory_cfg_mains_frequency_to_factory_data(void)
{
    int mains_frequency = 0;

    if (!factory_cfg_get_mains_frequency(&mains_frequency)) {
        ESP_LOGW(TAG, "factory_cfg mains frequency not available, keep FLASH_FACTORY_DATA=%d", factory_data.mains_frequency);
        return;
    }

    if (factory_data.mains_frequency == mains_frequency) {
        ESP_LOGI(TAG,
                 "FLASH_FACTORY_DATA mains profile already synced: %d",
                 mains_frequency);
        return;
    }

    ESP_LOGI(TAG,
             "Apply factory_cfg mains profile to FLASH_FACTORY_DATA: %d -> %d",
             factory_data.mains_frequency,
             mains_frequency);
    factory_data.mains_frequency = mains_frequency;
    ctr_factory_data_persist();
}

void app_main(void)
{
    char hmi_version_from_app[16] = {0};
    char factory_sn_for_log[FACTORY_CFG_SN_MAX_LEN] = {0};
    TickType_t factory_info_log_start_tick = 0;
    uint8_t factory_info_log_count = 0;
    bool has_saved_wifi = false;

    ctr_power_io_enable();
    ram_diag_snapshot("boot/app_main_entry");
    bsp_init();
    init_cjson_hooks();
    ram_diag_snapshot("boot/after_cjson_hooks");
    mqtt_preload_identity();
    mqtt_get_hmi_version_from_app(hmi_version_from_app, sizeof(hmi_version_from_app));
    ESP_LOGI(TAG, "HMI version from app image: %s", hmi_version_from_app);
    ctr_uart_init(115200, PIN_CTR_RS485_RX, PIN_CTR_RS485_TX);
    sync_factory_cfg_mains_frequency_to_factory_data();
    hmi_uart_init(UART_HMI_BAUDRATE, PIN_SCREEN_TTL_RX, PIN_SCREEN_TTL_TX);
    ram_diag_snapshot("boot/after_uart_init");

#ifdef KLM_FLASH_LOG_ENABLE
    init_spiffs();
#endif

    esp_log_set_vprintf(custom_log_write);
    wifi_init();
    ram_diag_snapshot("boot/after_wifi_init");
    has_saved_wifi = wifi_has_saved_credentials();
    if (has_saved_wifi) {
        sys_pra.app_mode = APP_MODE_MQTT_CONFIG;
        ESP_LOGI(TAG,
                 "Boot with saved Wi-Fi credentials, skip BLE init until user enters reprovision");
    } else {
        sys_pra.app_mode = APP_MODE_BT_PAIRING;
        ESP_LOGI(TAG,
                 "Boot without saved Wi-Fi credentials, keep BLE available for first-time provisioning");
    }
    if (mqtt_params_load(&mqtt_config) != ESP_OK) {
        ESP_LOGW(TAG, "MQTT params not ready at boot, SN may be empty until factory/NVS data is available");
    }
    if (mqtt_config.client_id[0] == '\0') {
        strncpy(mqtt_config.client_id, MQTT_DEFAULT_CLIENT_ID, sizeof(mqtt_config.client_id) - 1);
        mqtt_config.client_id[sizeof(mqtt_config.client_id) - 1] = '\0';
        strncpy(mqtt_config.subscribe_topic, MQTT_DEFAULT_SUBSCRIBE_TOPIC, sizeof(mqtt_config.subscribe_topic) - 1);
        mqtt_config.subscribe_topic[sizeof(mqtt_config.subscribe_topic) - 1] = '\0';
        strncpy(mqtt_config.publish_topic, MQTT_DEFAULT_PUBLISH_TOPIC, sizeof(mqtt_config.publish_topic) - 1);
        mqtt_config.publish_topic[sizeof(mqtt_config.publish_topic) - 1] = '\0';
        strncpy(mqtt_config.username, MQTT_DEFAULT_USERNAME, sizeof(mqtt_config.username) - 1);
        mqtt_config.username[sizeof(mqtt_config.username) - 1] = '\0';
        strncpy(mqtt_config.password, MQTT_DEFAULT_PASSWORD, sizeof(mqtt_config.password) - 1);
        mqtt_config.password[sizeof(mqtt_config.password) - 1] = '\0';
        ESP_LOGI(TAG,
                 "Apply temporary MQTT identity fallback for test, SN=%s, username=%s, password=%s",
                 mqtt_config.client_id,
                 mqtt_config.username,
                 mqtt_config.password);
    }
    formula_store_init();
    device_statistics_store_init();
    ram_diag_snapshot("boot/after_store_init");
    if (!has_saved_wifi) {
        ram_diag_snapshot("boot/before_ble_init");
        ble_init();
        ram_diag_snapshot("boot/after_ble_init");
    } else {
        ESP_LOGI(TAG, "BLE init deferred at boot because saved Wi-Fi credentials exist");
        ram_diag_snapshot("boot/ble_deferred");
    }

    sp_pro_app_init();
    ram_diag_snapshot("boot/after_app_init");
    voice_manager_init();
    ram_diag_snapshot("boot/after_voice_init");
    print_task_stack_info();
    ram_diag_snapshot("boot/after_task_stack_dump");
    ctr_ota_init();
    ram_diag_snapshot("boot/after_ctr_ota_init");
    factory_info_log_start_tick = xTaskGetTickCount();

    while (1) {
        if (factory_info_log_count < 3 &&
            (xTaskGetTickCount() - factory_info_log_start_tick) >=
                pdMS_TO_TICKS(5000 + (factory_info_log_count * 3000))) {
            if (!factory_cfg_get_sn(factory_sn_for_log, sizeof(factory_sn_for_log))) {
                strncpy(factory_sn_for_log, "N/A", sizeof(factory_sn_for_log) - 1);
                factory_sn_for_log[sizeof(factory_sn_for_log) - 1] = '\0';
            }
            ESP_LOGI(TAG,
                     "[FACTORY_INFO] SN=%s APP=%s",
                     factory_sn_for_log,
                     hmi_version_from_app[0] ? hmi_version_from_app : "N/A");
            factory_info_log_count++;
        }
        app_mode_service();
        vTaskDelay(1);
    }
}
