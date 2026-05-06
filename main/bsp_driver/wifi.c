#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble.h"
#include "mqtt.h"
#include "uart_hmi.h"
#include "ota_ctr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_retry.h"
#include "ram_diag.h"
#include <string.h>
#include "system_runtime.h"

static const char *TAG = "wifi";
#define NVS_WIFI_CONFIG "wifi_config"
#define WIFI_SSID_MAX_LEN    33
#define WIFI_SCAN_TASK_STACK_SIZE_DEFAULT 4096
#define WIFI_SCAN_TASK_STACK_SIZE_TEST    2560
#define WIFI_SCAN_TASK_STACK_SIZE         WIFI_SCAN_TASK_STACK_SIZE_TEST
#define WIFI_SCAN_TASK_PRIORITY   5
#define WIFI_TEST_DISABLE_PS      0

uint16_t number = DEFAULT_SCAN_LIST_SIZE;
wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
uint8_t wifi_scan_flag=0;

typedef struct {
    const char *ssid;
    const char *password;
} wifi_nvs_save_ctx_t;

static esp_err_t wifi_write_credentials_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    const wifi_nvs_save_ctx_t *save_ctx = (const wifi_nvs_save_ctx_t *)ctx;
    esp_err_t err;

    if (!save_ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_set_str(nvs_handle, "wifi_ssid", save_ctx->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "wifi_password", save_ctx->password);
    }
    return err;
}
static volatile wifi_scan_state_t s_wifi_scan_state = WIFI_SCAN_STATE_IDLE;
static volatile bool s_wifi_reprovision_mode = false;
static void wifi_scan_mark_state(wifi_scan_state_t state);
static void WIFI_scan_task(void *pvParameters);
static TaskHandle_t s_wifi_scan_task_handle = NULL;
static bool s_runtime_wifi_online = false;
void read_wifi_info_from_nvs(char *ssid, char *password);

esp_netif_t *sta_netif;

char wifi_scan_str_temp[200];
char wifi_ssid[32];
char wifi_password[64];
char wifi_ip[64]="",wifi_mask[64]="",wifi_gw[64]="";
wifi_config_t wifi_config = {
    .sta = {
        .ssid = "",
        .password = "",
    },
};

static void wifi_log_memory_snapshot(const char *phase)
{
    ram_diag_snapshot(phase ? phase : "wifi/snapshot");
}

static void wifi_copy_credentials(const char *ssid, const char *password)
{
    const char *safe_ssid = ssid ? ssid : "";
    const char *safe_password = password ? password : "";

    strncpy(wifi_ssid, safe_ssid, sizeof(wifi_ssid) - 1);
    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';

    strncpy(wifi_password, safe_password, sizeof(wifi_password) - 1);
    wifi_password[sizeof(wifi_password) - 1] = '\0';

    strncpy((char *)wifi_config.sta.ssid, safe_ssid, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';

    strncpy((char *)wifi_config.sta.password, safe_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    if (wifi_password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WEP;
    }
}

static void wifi_apply_credentials_and_connect(const char *ssid, const char *password)
{
    esp_err_t err;

    wifi_copy_credentials(ssid, password);
    s_runtime_wifi_online = false;
    sys_pra.wifi_state = WIFI_DISCONNECTED;

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    save_wifi_info_to_nvs(wifi_ssid, wifi_password);
    read_wifi_info_from_nvs(wifi_ssid, wifi_password);

    ESP_LOGI(TAG,
             "Applied Wi-Fi credentials and restarted STA ssid:%s reprovision=%d",
             wifi_ssid,
             s_wifi_reprovision_mode);
}

static void format_auth_mode(char *buffer, int authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        strcpy(buffer, "OPEN");
        break;
    case WIFI_AUTH_OWE:
        strcpy(buffer, "OWE");
        break;
    case WIFI_AUTH_WEP:
        strcpy(buffer, "WEP");
        break;
    case WIFI_AUTH_WPA_PSK:
        strcpy(buffer, "WPAPSK");
        break;
    case WIFI_AUTH_WPA2_PSK:
        strcpy(buffer, "WPA2PSK");
        break;
    case WIFI_AUTH_WPA_WPA2_PSK:
        strcpy(buffer, "WPAPSKWPA2PSK");
        break;
    case WIFI_AUTH_ENTERPRISE:
        strcpy(buffer, "ENTERPRISE");
        break;
    case WIFI_AUTH_WPA3_PSK:
        strcpy(buffer, "WPA3PSK");
        break;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        strcpy(buffer, "WPA2WPA3PSK");
        break;
    case WIFI_AUTH_WPA3_ENT_192:
        strcpy(buffer, "WPA3_ENT_192");
        break;
    case WIFI_AUTH_WPA3_EXT_PSK:
        strcpy(buffer, "WPA3_EXT_PSK");
        break;
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
        strcpy(buffer, "WPA3_EXT_PSK_MIXED_MODE");
        break;
    default:
        strcpy(buffer, "UNKNOWN");
        break;
    }
}

static void format_cipher_type(char *buffer, int pairwise_cipher, int group_cipher)
{
    if (pairwise_cipher == WIFI_CIPHER_TYPE_NONE || group_cipher == WIFI_CIPHER_TYPE_NONE) {
        strcat(buffer, "NONE");
    } else {
        if (pairwise_cipher == WIFI_CIPHER_TYPE_CCMP) {
            strcat(buffer, "AES");
        } else if (pairwise_cipher == WIFI_CIPHER_TYPE_TKIP) {
            strcat(buffer, "TKIP");
        } else if (pairwise_cipher == WIFI_CIPHER_TYPE_TKIP_CCMP) {
            strcat(buffer, "TKIPCCMP");
        }
    }
}
void mac_to_str(const uint8_t *mac, char *str)
{
    if (mac == NULL || str == NULL) {
        return;
    }
    sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
int wifi_transform_rssi(int rssi_dbm)
{
    int ret;

    ret = (rssi_dbm+95)*2; 

    if (ret < 70) 
        ret = ret -(15 - ret/5);
    if(ret < 0) 
        ret = 0;
    else if(ret >100) 
        ret = 100;

    return ret;
}

bool is_wifi_connected(void)
{
    wifi_ap_record_t current_ap;

    if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK) {
        return true;
    }

    return false;
}

void reconnect_to_wifi(void)
{
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconnect to Wi-Fi (%d)", ret);
    }
}

void wifi_scan(void)
{
    wifi_scan_flag = 1;
    esp_wifi_stop();
    esp_wifi_start();
    if (is_wifi_connected()) {
        /* Disconnect before starting an active scan. */
        ESP_ERROR_CHECK(esp_wifi_disconnect());
    }

    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(ap_info));

    esp_wifi_scan_start(NULL, true);
    ESP_LOGI(TAG, "Max AP number ap_info can hold = %u", number);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGI(TAG, "Total APs scanned = %u, actual AP number ap_info holds = %u", ap_count, number);
    ESP_LOGI(TAG, "Ch,SSID,BSSID,Security,Indicator");
    uart_send_str("Ch,SSID,BSSID,Security,Indicator\r\n");

    char security[64];
    char mac[20];
    for (int i = 0; i < number; i++) {
        memset(security, 0, sizeof(security));
        format_auth_mode(security, ap_info[i].authmode);
        if (ap_info[i].authmode != WIFI_AUTH_WEP) {
            strcat(security, "/");
            format_cipher_type(security, ap_info[i].pairwise_cipher, ap_info[i].group_cipher);
        }
        mac_to_str(ap_info[i].bssid, mac);
        sprintf(wifi_scan_str_temp,
                "%d,%s,%s,%s,%d\r\n",
                ap_info[i].primary,
                ap_info[i].ssid,
                mac,
                security,
                wifi_transform_rssi(ap_info[i].rssi));
        uart_send_str(wifi_scan_str_temp);
        vTaskDelay(1);
    }

    uart_send_str("\r\n");
    if (!s_wifi_reprovision_mode) {
        reconnect_to_wifi();
    }
    wifi_scan_flag = 2;
}

esp_netif_dns_info_t main_dns;
esp_netif_dns_info_t backup_dns;

static bool wifi_should_retry_connect(void)
{
    if (s_wifi_reprovision_mode) {
        return false;
    }

    if (!wifi_has_saved_credentials()) {
        return false;
    }

    return s_wifi_scan_state != WIFI_SCAN_STATE_SCANNING;
}

static bool wifi_should_report_disconnect(void)
{
    return sys_pra.app_mode == APP_MODE_RUNTIME_BRIDGE &&
           sys_pra.power_off_flag == 0 &&
           s_wifi_scan_state != WIFI_SCAN_STATE_SCANNING &&
           s_runtime_wifi_online;
}

static bool wifi_should_request_mqtt_config_mode(void)
{
    if (sys_pra.app_mode == APP_MODE_YMODEM) {
        return false;
    }

    return sys_pra.mqtt_state != MQTT_CONNECTED &&
           sys_pra.mqtt_state != MQTT_CONNECTING;
}

bool wifi_is_runtime_online(void)
{
    return s_runtime_wifi_online;
}

bool wifi_has_saved_credentials(void)
{
    return wifi_ssid[0] != '\0';
}

bool wifi_is_reprovision_mode(void)
{
    return s_wifi_reprovision_mode;
}

void wifi_enter_reprovision_mode(void)
{
    esp_err_t err;
    bool connected = is_wifi_connected();

    s_wifi_reprovision_mode = true;
    s_runtime_wifi_online = false;
    sys_pra.wifi_state = WIFI_DISCONNECTED;
    mqtt_app_stop();

    if (connected) {
        err = esp_wifi_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "esp_wifi_disconnect failed entering reprovision: %s",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "Skip Wi-Fi disconnect entering reprovision: already disconnected");
    }

    ESP_LOGI(TAG, "Enter Wi-Fi reprovision mode");
}

void wifi_exit_reprovision_mode(bool reconnect_saved)
{
    s_wifi_reprovision_mode = false;

    if (reconnect_saved && wifi_has_saved_credentials()) {
        reconnect_to_wifi();
    }

    ESP_LOGI(TAG,
             "Exit Wi-Fi reprovision mode reconnect_saved=%d has_saved=%d",
             reconnect_saved,
             wifi_has_saved_credentials());
}

static void wifi_update_dns_info(const ip_event_got_ip_t *event)
{
    esp_err_t err = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &main_dns);
    if (err == ESP_OK) {
        esp_ip4addr_ntoa(&main_dns.ip.u_addr.ip4, wifi_scan_str_temp, sizeof(wifi_scan_str_temp));
        ESP_LOGI(TAG, "Primary DNS Server: %s", wifi_scan_str_temp);
        if (memcmp(wifi_scan_str_temp, "0.0.0.0", 8) == 0) {
            ESP_LOGE(TAG, "DNS ERR 0.0.0.0!");

            esp_netif_dns_info_t dns_info;
            dns_info.ip.u_addr.ip4.addr = event->ip_info.gw.addr;
            esp_err_t set_dns_err = esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
            if (set_dns_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set main DNS server info: %s", esp_err_to_name(set_dns_err));
            } else {
                ESP_LOGI(TAG, "Main DNS server set to default: %s", wifi_gw);
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to get primary DNS server info");
    }

    err = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &backup_dns);
    if (err == ESP_OK) {
        esp_ip4addr_ntoa(&backup_dns.ip.u_addr.ip4, wifi_scan_str_temp, sizeof(wifi_scan_str_temp));
        ESP_LOGI(TAG, "Secondary DNS Server: %s", wifi_scan_str_temp);
    } else {
        ESP_LOGE(TAG, "Failed to get secondary DNS server info");
    }
}

void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG,
                 "Wi-Fi STA start: has_saved=%d reprovision=%d ble_active=%d",
                 wifi_has_saved_credentials(),
                 s_wifi_reprovision_mode,
                 ble_app_is_active());
        if (wifi_should_retry_connect()) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_app_stop();

        if (wifi_should_retry_connect()) {
            esp_wifi_connect();
        }

        sys_pra.wifi_state = WIFI_DISCONNECTED;
        if (wifi_should_report_disconnect()) {
            uart_send_str("+EVENT=CON_OFF\r\n");
        }
        s_runtime_wifi_online = false;
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI", "ALL DONE");
        esp_ip4addr_ntoa(&event->ip_info.ip, wifi_ip, sizeof(wifi_ip));
        ESP_LOGI(TAG, "Got IP Address:  %s", wifi_ip);
        esp_ip4addr_ntoa(&event->ip_info.netmask, wifi_mask, sizeof(wifi_mask));
        ESP_LOGI(TAG, "Got Subnet Mask:  %s", wifi_mask);
        esp_ip4addr_ntoa(&event->ip_info.gw, wifi_gw, sizeof(wifi_gw));
        ESP_LOGI(TAG, "Got Gateway:  %s", wifi_gw);

        sys_pra.wifi_state = WIFI_CONNECTED;
        s_runtime_wifi_online = true;
        wifi_update_dns_info(event);
        if (ble_app_is_active()) {
            ESP_LOGI(TAG, "Wi-Fi got IP, release BLE stack to reclaim internal RAM");
            ble_app_shutdown();
        } else {
            ESP_LOGI(TAG, "Wi-Fi got IP with BLE already inactive");
        }

        if (wifi_should_request_mqtt_config_mode()) {
            sys_pra.app_mode = APP_MODE_MQTT_CONFIG;
        }

        ctr_ota_notify_wifi_ready();
    }
}

void read_wifi_info_from_nvs(char *ssid, char *password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_CONFIG, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            if (ssid) {
                ssid[0] = '\0';
            }
            if (password) {
                password[0] = '\0';
            }
            ESP_LOGI(TAG,
                     "No saved Wi-Fi credentials in NVS namespace '%s', wait for provisioning",
                     NVS_WIFI_CONFIG);
        } else {
            ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        }
        return;
    }
    
    size_t ssid_len = 0;
    err = nvs_get_str(nvs_handle, "wifi_ssid", NULL, &ssid_len);
    if (err == ESP_OK && ssid_len > 0) {
        nvs_get_str(nvs_handle, "wifi_ssid", ssid, &ssid_len);
    } else {
        ssid[0] = '\0';
    }

    size_t password_len = 0;
    err = nvs_get_str(nvs_handle, "wifi_password", NULL, &password_len);
    if (err == ESP_OK && password_len > 0) {
        nvs_get_str(nvs_handle, "wifi_password", password, &password_len);
    } else {
        password[0] = '\0';
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Wifi info read ssid:%s pass:%s", ssid, password);
}

void save_wifi_info_to_nvs(const char *ssid, const char *password) {
    nvs_handle_t nvs_handle;
    wifi_nvs_save_ctx_t save_ctx = {
        .ssid = ssid,
        .password = password,
    };
    esp_err_t err = nvs_open(NVS_WIFI_CONFIG, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }

    err = wifi_write_credentials_to_nvs(nvs_handle, &save_ctx);

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        static const char *const keys[] = {
            "wifi_ssid",
            "wifi_password",
        };
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "wifi credentials",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   wifi_write_credentials_to_nvs,
                                                   &save_ctx);
    }

    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) saving Wi-Fi credentials to NVS", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Wifi info saved to NVS %s %s", ssid, password);
}

void wifi_ssid_set(char *ssid) 
{
    wifi_apply_credentials_and_connect(ssid, wifi_password);
}

void wifi_password_set(char *password) 
{
    wifi_apply_credentials_and_connect(wifi_ssid, password);
}

void wifi_credentials_set(const char *ssid, const char *password)
{
    wifi_apply_credentials_and_connect(ssid, password);
}

void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);


    read_wifi_info_from_nvs(wifi_ssid, wifi_password);
    wifi_config.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password));
    if(wifi_ssid[0]==0)
    {
        wifi_config.sta.threshold.authmode=WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "No saved Wi-Fi credentials, wait for provisioning");
    }
    else if(wifi_password[0]==0)
    {
        wifi_config.sta.threshold.authmode=WIFI_AUTH_OPEN;
    }
    else
    {
        wifi_config.sta.threshold.authmode=WIFI_AUTH_WEP;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

#if WIFI_TEST_DISABLE_PS
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Wi-Fi power save test override: WIFI_PS_NONE");
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_LOGI(TAG, "Wi-Fi power save: WIFI_PS_MIN_MODEM");
#endif

    if (s_wifi_scan_task_handle == NULL) {
        if (xTaskCreate(&WIFI_scan_task,
                        "wifi_scan_task",
                        WIFI_SCAN_TASK_STACK_SIZE,
                        NULL,
                        WIFI_SCAN_TASK_PRIORITY,
                        &s_wifi_scan_task_handle) != pdPASS) {
            s_wifi_scan_task_handle = NULL;
            ESP_LOGE(TAG,
                     "failed to create wifi_scan_task worker, stack=%u bytes",
                     (unsigned)WIFI_SCAN_TASK_STACK_SIZE);
        } else {
            ESP_LOGI(TAG,
                     "wifi_scan_task worker created, stack=%u bytes",
                     (unsigned)WIFI_SCAN_TASK_STACK_SIZE);
        }
    }
}


static char WIFI_scan_ssid_storage[WIFI_SCAN_RESULT_MAX][WIFI_SSID_MAX_LEN];
char *WIFI_scan_ssid[WIFI_SCAN_RESULT_MAX]={0};
int WIFI_CNT=-1;

wifi_scan_state_t WIFI_scan_get_state(void)
{
    return s_wifi_scan_state;
}

void WIFI_scan_free(void)
{
    for(uint8_t i=0;i<sizeof(WIFI_scan_ssid)/sizeof(WIFI_scan_ssid[0]);i++)
    {
        WIFI_scan_ssid_storage[i][0] = '\0';
        WIFI_scan_ssid[i]=0;
    }
    WIFI_CNT=0;
    if (s_wifi_scan_state != WIFI_SCAN_STATE_SCANNING) {
        wifi_scan_mark_state(WIFI_SCAN_STATE_IDLE);
    }
}

static void wifi_scan_mark_state(wifi_scan_state_t state)
{
    s_wifi_scan_state = state;
    switch (state) {
    case WIFI_SCAN_STATE_SCANNING:
        wifi_scan_flag = 1;
        break;
    case WIFI_SCAN_STATE_READY:
        wifi_scan_flag = 2;
        break;
    case WIFI_SCAN_STATE_FAILED:
    case WIFI_SCAN_STATE_IDLE:
    default:
        wifi_scan_flag = 0;
        break;
    }
}

void WIFI_scan_task(void *pvParameters)
{
    UBaseType_t watermark;

    (void)pvParameters;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "wifi_scan_task start, stack=%u bytes, high_water=%u words",
             (unsigned)WIFI_SCAN_TASK_STACK_SIZE,
             (unsigned)watermark);

    for (;;) {
        esp_err_t err;
        uint8_t wifi_cnt = 0;
        uint16_t ap_count = 0;
        uint8_t duplicate = 0;

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        wifi_scan_mark_state(WIFI_SCAN_STATE_SCANNING);
        esp_wifi_stop();
        esp_wifi_start();
        WIFI_CNT=-1;

        WIFI_scan_free();
        if (is_wifi_connected()) {
            /* Disconnect before starting an active scan. */
            ESP_ERROR_CHECK(esp_wifi_disconnect());
        }

        wifi_log_memory_snapshot("wifi/scan_start");
        number = DEFAULT_SCAN_LIST_SIZE;
        memset(ap_info, 0, sizeof(ap_info));
        ESP_LOGI(TAG, "Max AP number ap_info can hold = %u", number);
        err = esp_wifi_scan_start(NULL, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
            WIFI_CNT = 0;
            wifi_scan_mark_state(WIFI_SCAN_STATE_FAILED);
            watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGW(TAG,
                     "wifi_scan_task scan_start failure, high_water=%u words",
                     (unsigned)watermark);
            if (!s_wifi_reprovision_mode) {
                reconnect_to_wifi();
            }
            continue;
        }

        err = esp_wifi_scan_get_ap_records(&number, ap_info);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
            WIFI_CNT = 0;
            wifi_scan_mark_state(WIFI_SCAN_STATE_FAILED);
            watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGW(TAG,
                     "wifi_scan_task get_ap_records failure, high_water=%u words",
                     (unsigned)watermark);
            if (!s_wifi_reprovision_mode) {
                reconnect_to_wifi();
            }
            continue;
        }
        err = esp_wifi_scan_get_ap_num(&ap_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
            WIFI_CNT = 0;
            wifi_scan_mark_state(WIFI_SCAN_STATE_FAILED);
            watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGW(TAG,
                     "wifi_scan_task get_ap_num failure, high_water=%u words",
                     (unsigned)watermark);
            if (!s_wifi_reprovision_mode) {
                reconnect_to_wifi();
            }
            continue;
        }
        ESP_LOGI(TAG, "Total APs scanned = %u, actual AP number ap_info holds = %u", ap_count, number);
        for (int i = 0; i < number; i++)
        {
            duplicate = 0;
            for(uint8_t x=0;x<wifi_cnt;x++)
            {
                if(strcmp(WIFI_scan_ssid[x],(char*)ap_info[i].ssid)==0)
                {
                    duplicate = 1;   /* duplicate SSID */
                }
            }
            if (duplicate == 1)
            {
                continue;
            }

            size_t ssid_len = strnlen((char *)ap_info[i].ssid, sizeof(ap_info[i].ssid));
            if (ssid_len == 0) {
                continue;
            }
            memcpy(WIFI_scan_ssid_storage[wifi_cnt], ap_info[i].ssid, ssid_len);
            WIFI_scan_ssid_storage[wifi_cnt][ssid_len] = '\0';
            WIFI_scan_ssid[wifi_cnt] = WIFI_scan_ssid_storage[wifi_cnt];
            wifi_cnt++;

            if(wifi_cnt>=WIFI_SCAN_RESULT_MAX)
            {
                break;
            }
            vTaskDelay(1);
        }
        if (!s_wifi_reprovision_mode && wifi_has_saved_credentials()) {
            reconnect_to_wifi();
        }
        ESP_LOGI(TAG, "Wifi scan finish , wifi_cnt=%d", wifi_cnt);
        WIFI_CNT=wifi_cnt;
        wifi_scan_mark_state(WIFI_SCAN_STATE_READY);
        wifi_log_memory_snapshot("wifi/scan_done");
        watermark = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG,
                 "wifi_scan_task scan complete, high_water=%u words",
                 (unsigned)watermark);
    }
}


void WIFI_scan_thread_start(void)
{
    if (s_wifi_scan_state == WIFI_SCAN_STATE_SCANNING) {
        ESP_LOGI(TAG, "wifi scan already running");
        return;
    }
    if (s_wifi_scan_task_handle == NULL) {
        ESP_LOGE(TAG, "wifi scan worker is not ready");
        WIFI_CNT = 0;
        wifi_scan_mark_state(WIFI_SCAN_STATE_FAILED);
        return;
    }
    WIFI_CNT = -1;
    wifi_scan_mark_state(WIFI_SCAN_STATE_SCANNING);
    xTaskNotifyGive(s_wifi_scan_task_handle);
}

esp_err_t WIFI_scan_request(void)
{
    if (s_wifi_scan_state == WIFI_SCAN_STATE_SCANNING) {
        return ESP_OK;
    }

    WIFI_scan_thread_start();
    return (s_wifi_scan_state == WIFI_SCAN_STATE_FAILED) ? ESP_FAIL : ESP_OK;
}



