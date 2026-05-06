#include "ble_pairing.h"
#include "sysdef.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "ble.h"
#include "wifi.h"
#include "mqtt.h"


static const char *TAG = "BLE_PAIRING";
#define WIFI_GET_PAGE_SIZE 3
#define WIFI_GET_JSON_BUF_SIZE 256
#define WIFI_RES_JSON_BUF_SIZE 128
#define BLE_NOTIFY_COOLDOWN_MS 200

// Private prototypes
static uint8_t J_Read_SN(cJSON * root_j);
static uint8_t J_WIFI_SET(cJSON * root_j);
static uint8_t J_WIFI_GET(cJSON * root_j);
static uint8_t J_WIFI_RES(cJSON * root_j);
static uint8_t J_PIN_CHECK(cJSON * root_j);


typedef struct{
    char * cmd_str;
    uint8_t (*run)(cJSON * root_j);
} JSON_CMD_Typ;

static const JSON_CMD_Typ cmd_list[]={
    {"Read-SN", J_Read_SN},
    {"WIFI_SET", J_WIFI_SET},
    {"WIFI_GET", J_WIFI_GET},
    {"WIFI_RES", J_WIFI_RES}, //获取设备网络状态
    {"PIN_CHECK", J_PIN_CHECK}, //PIN校验
};

static uint8_t wifi_ret_index = 0;
static bool s_wifi_get_pending = false;
static TickType_t s_last_notify_tick = 0;
static const char *s_last_notify_type = NULL;
static bool s_read_sn_scan_active = false;

static bool ble_pairing_append_json_escaped(char *dst, size_t dst_size, size_t *used, const char *src)
{
    static const char hex[] = "0123456789abcdef";

    if (!dst || dst_size == 0U || !used || !src) {
        return false;
    }

    while (*src != '\0') {
        unsigned char ch = (unsigned char)*src++;

        if (ch == '\"' || ch == '\\') {
            if (*used + 2U >= dst_size) {
                return false;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = (char)ch;
        } else if (ch < 0x20U) {
            if (*used + 6U >= dst_size) {
                return false;
            }
            dst[(*used)++] = '\\';
            dst[(*used)++] = 'u';
            dst[(*used)++] = '0';
            dst[(*used)++] = '0';
            dst[(*used)++] = hex[(ch >> 4) & 0x0FU];
            dst[(*used)++] = hex[ch & 0x0FU];
        } else {
            if (*used + 1U >= dst_size) {
                return false;
            }
            dst[(*used)++] = (char)ch;
        }
    }

    dst[*used] = '\0';
    return true;
}

static bool ble_pairing_notify_rate_limited(const char *type_name)
{
    TickType_t now;
    TickType_t delta;

    if (!type_name) {
        return false;
    }

    // WIFI_GET is page-based; repeated requests are expected while the app
    // fetches the full SSID list, so do not throttle it as a duplicate.
    if (strcmp(type_name, "WIFI_GET") == 0) {
        return false;
    }

    now = xTaskGetTickCount();
    if (s_last_notify_type == NULL) {
        return false;
    }

    delta = now - s_last_notify_tick;
    if ((strcmp(s_last_notify_type, type_name) == 0) &&
        (delta < pdMS_TO_TICKS(BLE_NOTIFY_COOLDOWN_MS))) {
        ESP_LOGW(TAG, "Skip BLE notify duplicate type=%s delta_ms=%u",
                 type_name,
                 (unsigned)(delta * portTICK_PERIOD_MS));
        return true;
    }

    return false;
}

static esp_err_t ble_pairing_send_json(const char *type_name, const char *json_str)
{
    esp_err_t err;

    if (!json_str) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ble_pairing_notify_rate_limited(type_name)) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ble_gatts_send_indicate(heart_rate_profile_tab[0].gatts_if,
                                      heart_rate_profile_tab[0].conn_id,
                                      heart_rate_handle_table[IDX_CHAR_VAL_B],
                                      strlen(json_str), (uint8_t *)json_str, false);
    ESP_LOGI(TAG, "BLE notify type=%s len=%u err=%s",
             type_name ? type_name : "<unknown>",
             (unsigned)strlen(json_str),
             esp_err_to_name(err));
    if (err == ESP_OK) {
        s_last_notify_tick = xTaskGetTickCount();
        s_last_notify_type = type_name;
    }
    return err;
}

static uint8_t ble_pairing_send_wifi_get_page(void)
{
    if (WIFI_CNT <= 0) {
        char str[96];
        int len = snprintf(str, sizeof(str),
                           "{\"Dev\":\"%s\",\"Type\":\"WIFI_GET\",\"Len\":0,\"WIFI_SSIDs\":[]}",
                           MODULE_VER);
        if (len > 0) {
            ble_pairing_send_json("WIFI_GET", str);
        }
        ESP_LOGI(TAG, "WIFI_CNT %d", WIFI_CNT);
        return 0;
    }

    char str[WIFI_GET_JSON_BUF_SIZE];
    int len = snprintf(str, sizeof(str),
                       "{\"Dev\":\"%s\",\"Type\":\"WIFI_GET\",\"Len\":%d,\"WIFI_SSIDs\":[",
                       MODULE_VER, WIFI_CNT);
    size_t used;
    int start_index = wifi_ret_index * WIFI_GET_PAGE_SIZE;
    int end_index = start_index + WIFI_GET_PAGE_SIZE;
    bool first = true;

    if (len <= 0 || (size_t)len >= sizeof(str)) {
        ESP_LOGE(TAG, "WIFI_GET header format failed");
        return 1;
    }

    used = (size_t)len;
    ESP_LOGI(TAG, "WIFI_CNT %d", WIFI_CNT);

    for (int i = start_index; i < end_index && i < WIFI_CNT; i++) {
        if (WIFI_scan_ssid[i] == NULL) {
            continue;
        }

        if (!first) {
            if (used + 1U >= sizeof(str)) {
                ESP_LOGE(TAG, "WIFI_GET buffer full before comma");
                return 1;
            }
            str[used++] = ',';
            str[used] = '\0';
        }

        if (used + 1U >= sizeof(str)) {
            ESP_LOGE(TAG, "WIFI_GET buffer full before quote");
            return 1;
        }
        str[used++] = '"';
        str[used] = '\0';

        if (!ble_pairing_append_json_escaped(str, sizeof(str), &used, WIFI_scan_ssid[i])) {
            ESP_LOGE(TAG, "WIFI_GET buffer full while appending ssid");
            return 1;
        }

        if (used + 1U >= sizeof(str)) {
            ESP_LOGE(TAG, "WIFI_GET buffer full after ssid");
            return 1;
        }
        str[used++] = '"';
        str[used] = '\0';
        first = false;
    }

    if (used + 2U >= sizeof(str)) {
        ESP_LOGE(TAG, "WIFI_GET buffer full before suffix");
        return 1;
    }
    str[used++] = ']';
    str[used++] = '}';
    str[used] = '\0';

    if ((wifi_ret_index + 1) * WIFI_GET_PAGE_SIZE >= WIFI_CNT) {
        wifi_ret_index = 0;
    } else {
        wifi_ret_index++;
    }

    ble_pairing_send_json("WIFI_GET", str);
    return 0;
}

// Method implementation
static esp_err_t BlePairing_handle(BlePairing *self, char *json_str) {
    if (!json_str) return ESP_FAIL;
    
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *Type_j = cJSON_GetObjectItem(root, "Type");
    if (Type_j && Type_j->valuestring) {
        for (int i = 0; i < sizeof(cmd_list)/sizeof(cmd_list[0]); i++) {
            if (strcmp(Type_j->valuestring, cmd_list[i].cmd_str) == 0) {
                cmd_list[i].run(root);
                cJSON_Delete(root);
                return ESP_OK;
            }
        }
    }
    cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
}

// Singleton instance
static BlePairing instance = {
    .handle = BlePairing_handle,
};

BlePairing* BlePairing_GetInstance(void) {
    return &instance;
}

void BlePairing_Poll(void)
{
    wifi_scan_state_t scan_state;

    if (!s_wifi_get_pending) {
        return;
    }

    scan_state = WIFI_scan_get_state();
    if (scan_state == WIFI_SCAN_STATE_IDLE || scan_state == WIFI_SCAN_STATE_SCANNING) {
        return;
    }

    s_wifi_get_pending = false;
    s_read_sn_scan_active = false;
    ESP_LOGI(TAG, "WIFI_GET deferred response triggered, scan_state=%d", scan_state);
    ble_pairing_send_wifi_get_page();
}

// Static function implementations
static uint8_t J_Read_SN(cJSON * root_j)
{
    char str_temp[10]= {0};
    cJSON * ret_root;
    char* str = NULL;
    wifi_scan_state_t scan_state;

    (void)root_j;
    scan_state = WIFI_scan_get_state();
    if (s_read_sn_scan_active || scan_state == WIFI_SCAN_STATE_SCANNING) {
        ESP_LOGW(TAG, "Read-SN ignored while Wi-Fi scan is active state=%d active=%d",
                 scan_state, s_read_sn_scan_active ? 1 : 0);
        return 0;
    }

    srand(xTaskGetTickCount());
    int16_t pin = rand()%10000;

    ret_root= cJSON_CreateObject();
    cJSON_AddStringToObject(ret_root,"Dev",MODULE_VER);
    cJSON_AddStringToObject(ret_root,"Type","Read-SN");
    if(mqtt_config.deviceType[0]!=0)
    {
        cJSON_AddStringToObject(ret_root,"deviceType",mqtt_config.deviceType);
    }
    else
    {
        cJSON_AddStringToObject(ret_root,"deviceType","SP1Pro");
    }
    cJSON_AddStringToObject(ret_root,"SN",mqtt_config.client_id);
    sprintf(str_temp,"%04d",pin);
    cJSON_AddStringToObject(ret_root,"PIN",str_temp);
    cJSON_AddBoolToObject(ret_root,"WIFI-Sta",sys_pra.wifi_state);
    str = cJSON_Print(ret_root);
    ESP_LOGI(TAG, "%s",str);
    ble_pairing_send_json("Read-SN", str);
    free(str);
    cJSON_Delete(ret_root);
    wifi_ret_index = 0;
    s_read_sn_scan_active = true;
    WIFI_scan_request();
    return 0;
}

static uint8_t J_WIFI_SET(cJSON * root_j)
{
    cJSON * SSID_j;
    cJSON * PW_j;
    SSID_j= cJSON_GetObjectItem(root_j, "SSID");
    PW_j= cJSON_GetObjectItem(root_j, "PW");
    if(SSID_j==NULL||PW_j==NULL)
    {
        return 1;
    }
    wifi_credentials_set(SSID_j->valuestring, PW_j->valuestring);

    return 0;
}

static uint8_t J_WIFI_GET(cJSON * root_j)
{
    wifi_scan_state_t scan_state;
    (void)root_j;
    scan_state = WIFI_scan_get_state();
    s_wifi_get_pending = true;

    if (scan_state == WIFI_SCAN_STATE_IDLE || scan_state == WIFI_SCAN_STATE_FAILED) {
        wifi_ret_index = 0;
        WIFI_scan_request();
        ESP_LOGI(TAG, "WIFI_GET queued, scan requested, scan_state=%d", scan_state);
        return 0;
    }

    if (scan_state == WIFI_SCAN_STATE_SCANNING) {
        ESP_LOGI(TAG, "WIFI_GET deferred while scan is running");
        return 0;
    }

    ESP_LOGI(TAG, "WIFI_GET queued, scan_state=%d", scan_state);

    return 0;
}

static uint8_t J_WIFI_RES(cJSON * root_j)
{
    char str[WIFI_RES_JSON_BUF_SIZE];
    int len;
    bool wifi_res = true;
    bool dhcp_res;
    (void)root_j;
    dhcp_res = (sys_pra.wifi_state == WIFI_CONNECTED);

    len = snprintf(str, sizeof(str),
                   "{\"Dev\":\"%s\",\"Type\":\"WIFI_RES\",\"Len\":%u,\"WIFI_RES\":%s,\"DHCP_RES\":%s}",
                   MODULE_VER,
                   (unsigned)wifi_ret_index,
                   wifi_res ? "true" : "false",
                   dhcp_res ? "true" : "false");
    if (len <= 0 || (size_t)len >= sizeof(str)) {
        ESP_LOGE(TAG, "WIFI_RES format failed len=%d", len);
        return 1;
    }

    ESP_LOGI(TAG, "WIFI_RES notify queued len=%d wifi_res=%d dhcp_res=%d",
             len, wifi_res ? 1 : 0, dhcp_res ? 1 : 0);
    ble_pairing_send_json("WIFI_RES", str);
    if(sys_pra.mqtt_state == MQTT_CONNECTED)
    {
        WIFI_scan_free();
    }

    return 0;
}

static uint8_t J_PIN_CHECK(cJSON * root_j)
{
    cJSON * ret_root;
    char* str = NULL;

    (void)cJSON_GetObjectItem(root_j, "PIN");
    ret_root= cJSON_CreateObject();
    cJSON_AddStringToObject(ret_root,"Dev",MODULE_VER);
    cJSON_AddStringToObject(ret_root,"Type","PIN_CHECK");
    cJSON_AddBoolToObject(ret_root,"Return",1);
    str = cJSON_Print(ret_root);
    ble_pairing_send_json("PIN_CHECK", str);
    free(str);
    cJSON_Delete(ret_root);
    return 0;
}
