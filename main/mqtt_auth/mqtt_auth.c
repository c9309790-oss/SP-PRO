#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>
#include <cJSON.h>
#include "mqtt_auth.h"
#include "mqtt.h"
#include "mbedtls/base64.h"
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include "RSA_sign.h"
#include "uart_hmi.h"
#include "uart_ctr.h"
#include "esp_crt_bundle.h"
#define AUTH_JSON_LEN_MAX 1024
#define PRIVATEKEY_LEN_MAX 1024
#define MQTT_AUTH_TASK_STACK_SIZE_DEFAULT 8192
#define MQTT_AUTH_TASK_STACK_SIZE_TEST    6144
#define MQTT_AUTH_TASK_STACK_SIZE         MQTT_AUTH_TASK_STACK_SIZE_TEST

/* 0: idle, 1: auth task running */
static uint8_t auth_flag=0;

// 日志标签
static const char *TAG = "mqtt_auth";
static char *mqtt_auth_server_json_buffer;


void xor(uint8_t* key, const char* sn, int key_len) {
    int sn_len = strlen(sn);
    
    if (sn_len > key_len) {
        ESP_LOGE(TAG,"Error: sn length is greater than key length.\n");
        return;
    }
    for (int i = 0; i < key_len; i++) {
        key[i] ^= sn[i % sn_len];  // 使用取余操作符简化sn_idx的循环
    }
}


static bool parse_mqtt_auth_response(void) {
    bool auth_ready = false;
    int sign_ret;
    int b64_ret;
    uint8_t *privateKey = NULL;
    size_t privateKey_base64_dec_len = 0;
    uint8_t password_base64[256] = {0};
    size_t password_base64_len = 0;
    uint8_t password[256] = {0};
    size_t password_len = 0;
    char context[128] = "";
    char Auth_mqtt_user_name[64] = "", Auth_mqtt_password[64] = "";
    cJSON *json = NULL;
    cJSON *data = NULL;
    cJSON *sn_item = NULL;
    cJSON *privateKey_item = NULL;
    cJSON *nonce_item = NULL;

    privateKey = calloc(1, PRIVATEKEY_LEN_MAX);
    if (privateKey == NULL) {
        ESP_LOGE(TAG, "Failed to allocate private key buffer");
        return false;
    }

    ESP_LOGI(TAG, "Auth server raw response: %s", mqtt_auth_server_json_buffer ? mqtt_auth_server_json_buffer : "<null>");

    json = cJSON_Parse(mqtt_auth_server_json_buffer);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        goto cleanup;
    }

    // 提取数据
    data = cJSON_GetObjectItem(json, "data");
    if (data == NULL) {
        ESP_LOGE(TAG, "No 'data' field in JSON");
        goto cleanup;
    }

    // 获取SN
    sn_item = cJSON_GetObjectItem(data, "sn");
    if (sn_item != NULL && cJSON_IsString(sn_item) && sn_item->valuestring != NULL && sn_item->valuestring[0] != '\0') {
        ESP_LOGI(TAG, "Real SN from auth server: %s", sn_item->valuestring);
    } else {
        ESP_LOGE(TAG, "No 'sn' field in JSON or not a string");
        goto cleanup;
    }

    // 获取privateKey并解码
    privateKey_item = cJSON_GetObjectItem(data, "privateKey");
    if (privateKey_item != NULL && cJSON_IsString(privateKey_item) && privateKey_item->valuestring != NULL && privateKey_item->valuestring[0] != '\0') {
        const char *privateKey_base64 = privateKey_item->valuestring;
        size_t privateKey_len = strlen(privateKey_base64);

        // 执行解码
        if (mbedtls_base64_decode(privateKey, PRIVATEKEY_LEN_MAX, &privateKey_base64_dec_len, (const unsigned char *)privateKey_base64, privateKey_len) != 0) {
            ESP_LOGE(TAG, "Failed to decode base64 private key");
            goto cleanup;
        }
        
        ESP_LOGI(TAG, "Private Key decoded successfully");


    } else {
        ESP_LOGE(TAG, "No 'privateKey' field in JSON or not a string");
        goto cleanup;
    }

    // 获取nonce
    nonce_item = cJSON_GetObjectItem(data, "nonce");
    if (nonce_item != NULL && cJSON_IsString(nonce_item) && nonce_item->valuestring != NULL && nonce_item->valuestring[0] != '\0') {
        ESP_LOGI(TAG, "Nonce: %s", nonce_item->valuestring);
    } else {
        ESP_LOGE(TAG, "No 'nonce' field in JSON or not a string");
        goto cleanup;
    }

    xor(privateKey, sn_item->valuestring, (int)privateKey_base64_dec_len);
    snprintf(context, sizeof(context), "sn%snonce%s", sn_item->valuestring, nonce_item->valuestring);
    sign_ret = rsa_sign(context,
                        strlen(context),
                        privateKey,
                        privateKey_base64_dec_len,
                        password,
                        sizeof(password),
                        &password_len);
    if (sign_ret != 0 || password_len == 0) {
        ESP_LOGE(TAG,
                 "Failed to sign MQTT auth context, sign_ret=%d password_len=%u",
                 sign_ret,
                 (unsigned)password_len);
        goto cleanup;
    }
    b64_ret = mbedtls_base64_encode(password_base64, sizeof(password_base64), &password_base64_len, password, password_len);
    if (b64_ret != 0) {
        ESP_LOGE(TAG, "Failed to base64-encode auth password, ret=%d", b64_ret);
        goto cleanup;
    }
    ESP_LOGI(TAG, "Auth password(base64 raw): %s", password_base64);

    if (password_base64_len < 40) {
        ESP_LOGE(TAG, "Auth password(base64 raw) is too short, len=%u", (unsigned)password_base64_len);
        goto cleanup;
    }

    snprintf(Auth_mqtt_user_name, sizeof(Auth_mqtt_user_name), "embedded|nonce$%s", nonce_item->valuestring);
    memcpy(Auth_mqtt_password, &password_base64[10], 30);
    Auth_mqtt_password[30] = '\0';
    ESP_LOGI(TAG, "Generated MQTT username: %s", Auth_mqtt_user_name);
    ESP_LOGI(TAG, "Generated MQTT password: %s", Auth_mqtt_password);
    if(Auth_mqtt_user_name[0]!=0 && Auth_mqtt_password[0]!=0)
    {
        char temp[300] = "";
        mqtt_config_t mqtt_config_tmp = {0};
        memcpy(&mqtt_config_tmp, &mqtt_config, sizeof(mqtt_config));
        snprintf(temp, sizeof(temp), "%s/kalerm/iot/command/controlserver/home", mqtt_config_tmp.client_id);
        strcpy(mqtt_config_tmp.subscribe_topic, temp);
        snprintf(temp, sizeof(temp), "stateserver/kalerm/iot/home/%s", mqtt_config_tmp.client_id);
        strcpy(mqtt_config_tmp.publish_topic, temp);
        strcpy(mqtt_config_tmp.username, Auth_mqtt_user_name);
        strcpy(mqtt_config_tmp.password, Auth_mqtt_password);
        ESP_LOGI(TAG,
                 "Apply auth MQTT config, SN=%s, username=%s, password=%s",
                 mqtt_config_tmp.client_id,
                 mqtt_config_tmp.username,
                 mqtt_config_tmp.password);
        if (mqtt_params_update(&mqtt_config_tmp) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist authenticated MQTT config");
            goto cleanup;
        }
        if (mqtt_mark_auth_generated(true) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist auth_generated=true after auth success");
            goto cleanup;
        }
        mqtt_handle_auth_ready();
        auth_ready = true;
        ESP_LOGI(TAG,
                 "Auth success persisted, continue MQTT startup with dynamic credentials without reboot, SN=%s",
                 mqtt_config_tmp.client_id);
    }
    // 清理
cleanup:
    free(privateKey);
    cJSON_Delete(json);
    return auth_ready;
}

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    static int output_len = 0;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;

        case HTTP_EVENT_ON_CONNECTED:
            memset(mqtt_auth_server_json_buffer, 0, AUTH_JSON_LEN_MAX);
            output_len = 0;  // 重置output_len
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;

        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;

        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s", evt->header_key);
            break;

        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (output_len + evt->data_len < AUTH_JSON_LEN_MAX) {
                memcpy(mqtt_auth_server_json_buffer + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
            } else {
                ESP_LOGE(TAG, "Buffer overflow, data too large!");
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            output_len = 0;
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED %s", mqtt_auth_server_json_buffer);
            output_len = 0;
            break;

        default:
            break;
    }
    return ESP_OK;
}

void mqtt_auth_task(void *pvParameters) {
    char Mqtt_auth_url[200]={0};
    UBaseType_t watermark;
    
    esp_http_client_config_t config = {
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .host = "iotcloud.kalerm.com",
        // .ciphersuites_list = klm_ETSI_EN_303645_ciphersuites_list,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    (void)pvParameters;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "mqtt_auth_task start, stack=%u bytes, high_water=%u words",
             (unsigned)MQTT_AUTH_TASK_STACK_SIZE,
             (unsigned)watermark);

    mqtt_auth_server_json_buffer=malloc(AUTH_JSON_LEN_MAX);
    if(mqtt_auth_server_json_buffer==NULL)
    {
        ESP_LOGE(TAG, "mqtt_auth_server_json_buffer malloc err!");
        auth_flag = 0;
        vTaskDelete(NULL);
    }

    snprintf(Mqtt_auth_url, sizeof(Mqtt_auth_url), "https://iotcloud.kalerm.com/api/auth/mqtt/getConnectInfo?sn=%s", mqtt_config.client_id);
    config.url = Mqtt_auth_url;
    ESP_LOGI(TAG, "Request one-device-one-secret auth url=%s", Mqtt_auth_url);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed in mqtt_auth_task");
        auth_flag = 0;
        free(mqtt_auth_server_json_buffer);
        mqtt_auth_server_json_buffer = NULL;
        vTaskDelete(NULL);
    }

    esp_http_client_set_header(client, "Authorization", "Basic a2FsZXJtOmthbGVybUAyMDIz");

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = (int)esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTPS Status = %d", status_code);
        if (status_code == 200) {
            (void)parse_mqtt_auth_response();
            auth_flag = 0;
        } else {
            ESP_LOGE(TAG,
                     "Auth HTTP status is not 200, status=%d body=%s",
                     status_code,
                     mqtt_auth_server_json_buffer ? mqtt_auth_server_json_buffer : "<null>");
            auth_flag = 0;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        auth_flag = 0;
    }

    esp_http_client_cleanup(client);
    free(mqtt_auth_server_json_buffer);
    mqtt_auth_server_json_buffer = NULL;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "mqtt_auth_task done, high_water=%u words auth_flag=%u",
             (unsigned)watermark,
             (unsigned)auth_flag);
    vTaskDelete(NULL);
}

void mqtt_auth_init() {
    if(mqtt_config.client_id[0]==0)
    {
        ESP_LOGE(TAG, "mqtt_config.client_id is NULL!");
        return;
    }
    if (auth_flag == 0) {
        BaseType_t ok;
        auth_flag = 1;
        ESP_LOGI(TAG,
                 "Start one-device-one-secret auth, SN=%s, current username=%s, current password=%s",
                 mqtt_config.client_id,
                 mqtt_config.username,
                 mqtt_config.password);
        ok = xTaskCreate(&mqtt_auth_task,
                         "mqtt_auth_task",
                         MQTT_AUTH_TASK_STACK_SIZE,
                         NULL,
                         5,
                         NULL);
        if (ok != pdPASS) {
            auth_flag = 0;
            ESP_LOGE(TAG,
                     "Failed to create mqtt_auth_task, stack=%u bytes",
                     (unsigned)MQTT_AUTH_TASK_STACK_SIZE);
        } else {
            ESP_LOGI(TAG,
                     "mqtt_auth_task created, stack=%u bytes",
                     (unsigned)MQTT_AUTH_TASK_STACK_SIZE);
        }
    }
}

bool mqtt_auth_is_in_progress(void)
{
    return auth_flag == 1U;
}
