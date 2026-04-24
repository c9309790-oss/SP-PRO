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

static esp_err_t ble_pairing_send_json(const char *json_str)
{
    if (!json_str) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_ble_gatts_send_indicate(heart_rate_profile_tab[0].gatts_if,
                                       heart_rate_profile_tab[0].conn_id,
                                       heart_rate_handle_table[IDX_CHAR_VAL_B],
                                       strlen(json_str), (uint8_t *)json_str, false);
}

static uint8_t ble_pairing_send_wifi_get_page(void)
{
    if (WIFI_CNT <= 0) {
        char str[96];
        int len = snprintf(str, sizeof(str),
                           "{\"Dev\":\"%s\",\"Type\":\"WIFI_GET\",\"Len\":0,\"WIFI_SSIDs\":[]}",
                           MODULE_VER);
        if (len > 0) {
            ble_pairing_send_json(str);
        }
        ESP_LOGI(TAG, "WIFI_CNT %d", WIFI_CNT);
        return 0;
    }

    cJSON *ret_root = cJSON_CreateObject();
    cJSON *J_wifi_list = cJSON_CreateArray();
    char *str = NULL;

    cJSON_AddStringToObject(ret_root,"Dev",MODULE_VER);
    cJSON_AddStringToObject(ret_root,"Type","WIFI_GET");
    cJSON_AddNumberToObject(ret_root,"Len",WIFI_CNT);
    ESP_LOGI(TAG,"WIFI_CNT %d",WIFI_CNT);

    for(int i=wifi_ret_index*5;i<wifi_ret_index*5+5;i++)
    {
        if(i>=WIFI_CNT)
        {
            break;
        }
        ESP_LOGI(TAG,"WIFI_scan_ssid %p %s",WIFI_scan_ssid[i],WIFI_scan_ssid[i]);
        if(WIFI_scan_ssid[i]!=0)
        {
            cJSON_AddItemToArray(J_wifi_list, cJSON_CreateString(WIFI_scan_ssid[i]));
        }
    }
    if((wifi_ret_index+1)*5>=WIFI_CNT)
    {
        wifi_ret_index=0;
    }
    else
    {
        wifi_ret_index++;
    }

    cJSON_AddItemToObject(ret_root,"WIFI_SSIDs",J_wifi_list);
    str = cJSON_PrintUnformatted(ret_root);
    ble_pairing_send_json(str);
    free(str);
    cJSON_Delete(ret_root);
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
    ESP_LOGI(TAG, "WIFI_GET deferred response triggered, scan_state=%d", scan_state);
    ble_pairing_send_wifi_get_page();
}

// Static function implementations
static uint8_t J_Read_SN(cJSON * root_j)
{
    char str_temp[10]= {0};
    cJSON * ret_root;
    char* str = NULL;

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
    esp_ble_gatts_send_indicate(heart_rate_profile_tab[0].gatts_if, heart_rate_profile_tab[0].conn_id, heart_rate_handle_table[IDX_CHAR_VAL_B],
                                    strlen(str), (uint8_t *)str, false);
    free(str);
    cJSON_Delete(ret_root);
    wifi_ret_index = 0;
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
    cJSON * ret_root;
    char* str = NULL;
    (void)root_j;

    ret_root= cJSON_CreateObject();
    cJSON_AddStringToObject(ret_root,"Dev",MODULE_VER);

    cJSON_AddStringToObject(ret_root,"Type","WIFI_RES");
    cJSON_AddNumberToObject(ret_root,"Len",wifi_ret_index);

    //cJSON_AddBoolToObject(ret_root,"WIFI_RES",sys_pra.mqtt_state==MQTT_CONNECTED);
    cJSON_AddBoolToObject(ret_root,"WIFI_RES",1);
    cJSON_AddBoolToObject(ret_root,"DHCP_RES",sys_pra.wifi_state==WIFI_CONNECTED);
    
    str = cJSON_Print(ret_root);
    ESP_LOGI(TAG, "%s",str);
    esp_ble_gatts_send_indicate(heart_rate_profile_tab[0].gatts_if, heart_rate_profile_tab[0].conn_id, heart_rate_handle_table[IDX_CHAR_VAL_B],
                                strlen(str), (uint8_t *)str, false);
    free(str);
    cJSON_Delete(ret_root);
    if(sys_pra.mqtt_state == MQTT_CONNECTED)
    {
        WIFI_scan_free();
    }

    return 0;
}

static uint8_t J_PIN_CHECK(cJSON * root_j)
{
    cJSON * ret_root,*J_PIN;
    char* str = NULL;

    J_PIN=cJSON_GetObjectItem(root_j, "PIN");
    ret_root= cJSON_CreateObject();
    cJSON_AddStringToObject(ret_root,"Dev",MODULE_VER);
    cJSON_AddStringToObject(ret_root,"Type","PIN_CHECK");
    cJSON_AddBoolToObject(ret_root,"Return",1);
    str = cJSON_Print(ret_root);
    esp_ble_gatts_send_indicate(heart_rate_profile_tab[0].gatts_if, heart_rate_profile_tab[0].conn_id, heart_rate_handle_table[IDX_CHAR_VAL_B],
                                strlen(str), (uint8_t *)str, false);
    free(str);
    cJSON_Delete(ret_root);
    return 0;
}
