/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
*
* This demo showcases creating a GATT database using a predefined attribute table.
* It acts as a GATT server and can send adv data, be connected by client.
* Run the gatt_client demo, the client demo will automatically connect to the gatt_server_service_table demo.
* Client demo will enable GATT server's notify after connection. The two devices will then exchange
* data.
*
****************************************************************************/


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "board_config.h"
#include "mqtt.h"
#include "ble_pairing.h"
#include "ble.h"
#include "ram_diag.h"
#include "system_runtime.h"
#include "uart_hmi.h"

#define GATTS_TABLE_TAG "GATTS_TABLE_SP_PRO"

#define NVS_BLE_CONFIG "ble_config"
#define PROFILE_APP_IDX             0
#define ESP_APP_ID                  0x55
#define SVC_INST_ID                 0

/* The max length of characteristic value. When the GATT client performs a write or prepare write operation,
*  the data length must be less than GATTS_DEMO_CHAR_VAL_LEN_MAX.
*/
#define GATTS_DEMO_CHAR_VAL_LEN_MAX 500
#define PREPARE_BUF_MAX_SIZE        1024
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))


#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

static uint8_t adv_config_done       = 0;
static bool s_legacy_advertising_enabled = true;
static bool s_ble_advertising_active = false;
static bool s_ble_advertising_starting = false;
static bool s_ble_advertising_stopping = false;
static bool s_ble_connected = false;
static bool s_ble_stack_active = false;
static bool s_ble_classic_mem_released = false;
static const char *TAG = "ble";
#define BLE_TEST_DISABLE_UNUSED_RX_QUEUE 1

static void ble_build_expected_name(char *ble_name, size_t ble_name_size)
{
    size_t client_id_len;

    if (!ble_name || ble_name_size == 0U) {
        return;
    }

    ble_name[0] = '\0';
    client_id_len = strlen(mqtt_config.client_id);
    if (client_id_len == 0U) {
        snprintf(ble_name, ble_name_size, "%s", "KF-SA01C-WCC-0407");
        return;
    }

    if ((strlen("KF-SA01C-WCC-") + client_id_len) < ble_name_size) {
        snprintf(ble_name, ble_name_size, "%s%s", "KF-SA01C-WCC-", mqtt_config.client_id);
        return;
    }

    if (client_id_len >= 4U) {
        snprintf(ble_name,
                 ble_name_size,
                 "%s%s",
                 "KF-SA01C-WCC-",
                 mqtt_config.client_id + client_id_len - 4U);
        return;
    }

    snprintf(ble_name, ble_name_size, "%s%s", "KF-SA01C-WCC-", mqtt_config.client_id);
}

static void ble_log_memory_snapshot(const char *phase)
{
    ram_diag_snapshot(phase ? phase : "ble/snapshot");
}

uint16_t heart_rate_handle_table[HRS_IDX_NB];

QueueHandle_t ble_rx_queue;
char ble_name_str[30]="",ble_on_off_str[30]="",ble_pin_on_off_str[30]="";
typedef struct {
    uint8_t                 *prepare_buf;
    int                     prepare_len;
} prepare_type_env_t;

static prepare_type_env_t prepare_write_env;

static uint8_t service_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

/* The length of adv data must be less than 31 bytes */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006, //slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval        = 0x0010, //slave connection max interval, Time = max_interval * 1.25 msec
    .appearance          = 0x00,
    .manufacturer_len    = 0,    //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //test_manufacturer,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(service_uuid),
    .p_service_uuid      = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0, //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //&test_manufacturer[0],
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(service_uuid),
    .p_service_uuid      = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};


static esp_ble_adv_params_t adv_params = {
    .adv_int_min         = 0x20,
    .adv_int_max         = 0x40,
    .adv_type            = ADV_TYPE_IND,
    .own_addr_type       = BLE_ADDR_TYPE_PUBLIC,
    .channel_map         = ADV_CHNL_ALL,
    .adv_filter_policy   = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_err_t ble_start_advertising_if_needed(const char *reason)
{
    esp_err_t ret;

    if (!s_legacy_advertising_enabled) {
        ESP_LOGI(GATTS_TABLE_TAG,
                 "skip advertising start disabled reason=%s",
                 reason ? reason : "unknown");
        return ESP_OK;
    }

    if (s_ble_connected) {
        ESP_LOGI(GATTS_TABLE_TAG,
                 "skip advertising start connected reason=%s",
                 reason ? reason : "unknown");
        return ESP_OK;
    }

    if (s_ble_advertising_active || s_ble_advertising_starting) {
        ESP_LOGI(GATTS_TABLE_TAG,
                 "skip advertising start duplicated reason=%s active=%d starting=%d",
                 reason ? reason : "unknown",
                 (int)s_ble_advertising_active,
                 (int)s_ble_advertising_starting);
        return ESP_OK;
    }

    s_ble_advertising_starting = true;
    s_ble_advertising_stopping = false;
    ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret != ESP_OK) {
        s_ble_advertising_starting = false;
    }
    return ret;
}



static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
					esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
gatts_profile_inst_typ heart_rate_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

/* Service */
static const uint16_t GATTS_SERVICE_UUID_TEST      = 0xFEE7;
static const uint16_t GATTS_CHAR_UUID_TEST_A       = 0xFEC7;
static const uint16_t GATTS_CHAR_UUID_TEST_B       = 0xFEC8;
static const uint16_t GATTS_CHAR_UUID_TEST_C       = 0xFED6;
static const uint16_t GATTS_CHAR_UUID_TEST_D       = 0xFED5;

static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
//static const uint8_t char_prop_read                =  ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t char_prop_read_notify         =  ESP_GATT_CHAR_PROP_BIT_READ| ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_indicte             =  ESP_GATT_CHAR_PROP_BIT_INDICATE;
static const uint8_t char_prop_write               = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t char_prop_write_without_response = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
//static const uint8_t char_prop_read_write_notify   = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t heart_measurement_ccc[2]      = {0x00, 0x00};
static const uint8_t char_value[4]                 = {0x11, 0x22, 0x33, 0x44};


/* Full Database Description - Used to add attributes into the database */
static const esp_gatts_attr_db_t gatt_db_with_encrypt[HRS_IDX_NB] =
{
    // Service Declaration
    [IDX_SVC]        =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(GATTS_SERVICE_UUID_TEST), (uint8_t *)&GATTS_SERVICE_UUID_TEST}},

    /* Characteristic Declaration */
    [IDX_CHAR_A]     =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write_without_response}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_A] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_A, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},

    /* Characteristic Declaration */
    [IDX_CHAR_B]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_B]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_B, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},

    /* Client Characteristic Configuration Descriptor */
    [IDX_CHAR_CFG_B]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      sizeof(uint16_t), sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc}},

    /* Characteristic Declaration */
    [IDX_CHAR_C]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_indicte}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_C]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_C, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},

    /* Client Characteristic Configuration Descriptor */
    [IDX_CHAR_CFG_C]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      sizeof(uint16_t), sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc}},

    /* Characteristic Declaration */
    [IDX_CHAR_D]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_D]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_D, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},
};

static const esp_gatts_attr_db_t gatt_db_without_encrypt[HRS_IDX_NB] =
{
    // Service Declaration
    [IDX_SVC]        =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(GATTS_SERVICE_UUID_TEST), (uint8_t *)&GATTS_SERVICE_UUID_TEST}},

    /* Characteristic Declaration */
    [IDX_CHAR_A]     =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write_without_response}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_A] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_A, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},

    /* Characteristic Declaration */
    [IDX_CHAR_B]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_B]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_B, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},

    /* Client Characteristic Configuration Descriptor */
    [IDX_CHAR_CFG_B]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      sizeof(uint16_t), sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc}},

    /* Characteristic Declaration */
    [IDX_CHAR_C]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_indicte}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_C]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_C, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},

    /* Client Characteristic Configuration Descriptor */
    [IDX_CHAR_CFG_C]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      sizeof(uint16_t), sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc}},

    /* Characteristic Declaration */
    [IDX_CHAR_D]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write}},

    /* Characteristic Value */
    [IDX_CHAR_VAL_D]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_D, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      KLM_BLE_RX_BUF_SIZE, sizeof(char_value), (uint8_t *)char_value}},
};



static char *esp_key_type_to_str(esp_ble_key_type_t key_type);
static char *esp_auth_req_to_str(esp_ble_auth_req_t auth_req);
static void show_bonded_devices(void);


static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    ESP_LOGV(GATTS_TABLE_TAG, "GAP_EVT, event %d", event);

    switch (event) {

        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~ADV_CONFIG_FLAG);
            if (adv_config_done == 0){
                ble_start_advertising_if_needed("adv_data_ready");
            }
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
            if (adv_config_done == 0){
                ble_start_advertising_if_needed("scan_rsp_ready");
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            /* advertising start complete event to indicate advertising start successfully or failed */
            s_ble_advertising_starting = false;
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                s_ble_advertising_active = false;
                ESP_LOGE(GATTS_TABLE_TAG, "advertising start failed");
            } else if (!s_legacy_advertising_enabled) {
                s_ble_advertising_active = false;
                s_ble_advertising_stopping = true;
                esp_ble_gap_stop_advertising();
                ESP_LOGI(GATTS_TABLE_TAG, "advertising start completed after disabled, request stop");
            } else {
                s_ble_advertising_active = true;
                ESP_LOGI(GATTS_TABLE_TAG, "advertising start successfully");
            }
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            s_ble_advertising_starting = false;
            s_ble_advertising_stopping = false;
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(GATTS_TABLE_TAG, "Advertising stop failed");
            }
            else {
                s_ble_advertising_active = false;
                ESP_LOGI(GATTS_TABLE_TAG, "Stop adv successfully");
            }
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
            break;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:                           
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GAP_BLE_PASSKEY_REQ_EVT");
            break;
            
        case ESP_GAP_BLE_OOB_REQ_EVT: {
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GAP_BLE_OOB_REQ_EVT");
            uint8_t tk[16] = {1}; //If you paired with OOB, both devices need to use the same tk
            esp_ble_oob_req_reply(param->ble_security.ble_req.bd_addr, tk, sizeof(tk));
            break;
        }
        
        case ESP_GAP_BLE_LOCAL_IR_EVT:                               
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GAP_BLE_LOCAL_IR_EVT");
            break;
            
        case ESP_GAP_BLE_LOCAL_ER_EVT:                               
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GAP_BLE_LOCAL_ER_EVT");
            break;
            
        case ESP_GAP_BLE_NC_REQ_EVT:
            esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GAP_BLE_NC_REQ_EVT, the passkey Notify number:%" PRIu32, param->ble_security.key_notif.passkey);
            break;
            
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
            
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:  
            ESP_LOGI(GATTS_TABLE_TAG, "The passkey Notify number:%06" PRIu32, param->ble_security.key_notif.passkey);
            char pin_str[32];
            snprintf(pin_str, sizeof(pin_str), "+EVENT=BLE_PIN,%06" PRIu32 "\r\n", param->ble_security.key_notif.passkey);
            uart_send_str(pin_str);
            break;
            
        case ESP_GAP_BLE_KEY_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "key type = %s", esp_key_type_to_str(param->ble_security.ble_key.key_type));
            break;
            
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            esp_bd_addr_t bd_addr;
            memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            ESP_LOGI(GATTS_TABLE_TAG, "remote BD_ADDR: %08x%04x",\
                    (bd_addr[0] << 24) + (bd_addr[1] << 16) + (bd_addr[2] << 8) + bd_addr[3],
                    (bd_addr[4] << 8) + bd_addr[5]);
            ESP_LOGI(GATTS_TABLE_TAG, "address type = %d", param->ble_security.auth_cmpl.addr_type);
            ESP_LOGI(GATTS_TABLE_TAG, "pair status = %s",param->ble_security.auth_cmpl.success ? "success" : "fail");
            if(!param->ble_security.auth_cmpl.success) {
                ESP_LOGI(GATTS_TABLE_TAG, "fail reason = 0x%x",param->ble_security.auth_cmpl.fail_reason);
                // 直接使用当前连接的参数断开连接
                esp_ble_gap_disconnect(param->ble_security.auth_cmpl.bd_addr);
            } else {
                uart_send_str("+EVENT=BLE_AUTH_CMPL\r\n");
                ESP_LOGI(GATTS_TABLE_TAG, "auth mode = %s",esp_auth_req_to_str(param->ble_security.auth_cmpl.auth_mode));
            }
            show_bonded_devices();
            break;
        }
        
        case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT: {
            ESP_LOGD(GATTS_TABLE_TAG, "ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT status = %d", param->remove_bond_dev_cmpl.status);
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GAP_BLE_REMOVE_BOND_DEV");
            ESP_LOGI(GATTS_TABLE_TAG, "-----ESP_GAP_BLE_REMOVE_BOND_DEV----");
            esp_log_buffer_hex(GATTS_TABLE_TAG, (void *)param->remove_bond_dev_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            ESP_LOGI(GATTS_TABLE_TAG, "------------------------------------");
            break;
        }
        default:
            break;
    }
}

void example_prepare_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param)
{
    ESP_LOGI(GATTS_TABLE_TAG, "prepare write, handle = %d, value len = %d", param->write.handle, param->write.len);
    esp_gatt_status_t status = ESP_GATT_OK;
    if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
        status = ESP_GATT_INVALID_OFFSET;
    } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
        status = ESP_GATT_INVALID_ATTR_LEN;
    }
    if (status == ESP_GATT_OK && prepare_write_env->prepare_buf == NULL) {
        prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE * sizeof(uint8_t));
        prepare_write_env->prepare_len = 0;
        if (prepare_write_env->prepare_buf == NULL) {
            ESP_LOGE(GATTS_TABLE_TAG, "%s, Gatt_server prep no mem", __func__);
            status = ESP_GATT_NO_RESOURCES;
        }
    }

    /*send response when param->write.need_rsp is true */
    if (param->write.need_rsp){
        esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
        if (gatt_rsp != NULL){
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
            esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);
            if (response_err != ESP_OK) {
               ESP_LOGE(GATTS_TABLE_TAG, "Send response error");
            }
            free(gatt_rsp);
        }else{
            ESP_LOGE(GATTS_TABLE_TAG, "%s, malloc failed", __func__);
            status = ESP_GATT_NO_RESOURCES;
        }
    }
    if (status != ESP_GATT_OK){
        return;
    }
    memcpy(prepare_write_env->prepare_buf + param->write.offset,
           param->write.value,
           param->write.len);
    prepare_write_env->prepare_len += param->write.len;

}

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && prepare_write_env->prepare_buf){
        esp_log_buffer_hex(GATTS_TABLE_TAG, prepare_write_env->prepare_buf, prepare_write_env->prepare_len);
    }else{
        ESP_LOGI(GATTS_TABLE_TAG,"ESP_GATT_PREP_WRITE_CANCEL");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT:{
            esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(ble_name_str);
            if (set_dev_name_ret){
                ESP_LOGE(GATTS_TABLE_TAG, "set device name failed, error code = %x", set_dev_name_ret);
            }

            //config adv data
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret){
                ESP_LOGE(GATTS_TABLE_TAG, "config adv data failed, error code = %x", ret);
            }
            adv_config_done |= ADV_CONFIG_FLAG;
            //config scan response data
            ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
            if (ret){
                ESP_LOGE(GATTS_TABLE_TAG, "config scan response data failed, error code = %x", ret);
            }
            adv_config_done |= SCAN_RSP_CONFIG_FLAG;

            esp_err_t create_attr_ret;
            if(memcmp(ble_pin_on_off_str, "on", 2)==0) {
                ESP_LOGI(GATTS_TABLE_TAG, "Using encrypted GATT table");
                create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db_with_encrypt, gatts_if, HRS_IDX_NB, SVC_INST_ID);
            } else {
                ESP_LOGI(GATTS_TABLE_TAG, "Using non-encrypted GATT table");
                create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db_without_encrypt, gatts_if, HRS_IDX_NB, SVC_INST_ID);
            }
            
            if (create_attr_ret){
                ESP_LOGE(GATTS_TABLE_TAG, "create attr table failed, error code = %x", create_attr_ret);
            }
        }
       	    break;
        case ESP_GATTS_READ_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_READ_EVT");
       	    break;
        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep){
                // the data length of gattc write  must be less than GATTS_DEMO_CHAR_VAL_LEN_MAX.
                ESP_LOGI(GATTS_TABLE_TAG, "GATT_WRITE_EVT, handle = %d, value len = %d, value : %.*s", param->write.handle, param->write.len, param->write.len, param->write.value);

                if (heart_rate_handle_table[IDX_CHAR_CFG_B] == param->write.handle && param->write.len == 2)
                {
                    uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
                    if (descr_value == 0x0001){
                        ESP_LOGI(GATTS_TABLE_TAG, "IDX_CHAR_CFG_B notify enable");
                    }else if (descr_value == 0x0002){
                        ESP_LOGI(GATTS_TABLE_TAG, "IDX_CHAR_CFG_B indicate enable");
                    }
                    else if (descr_value == 0x0000){
                        ESP_LOGI(GATTS_TABLE_TAG, "IDX_CHAR_CFG_B notify/indicate disable ");
                    }else{
                        ESP_LOGE(GATTS_TABLE_TAG, "IDX_CHAR_CFG_B unknown descr value");
                        esp_log_buffer_hex(GATTS_TABLE_TAG, param->write.value, param->write.len);
                    }

                }
                else if(heart_rate_handle_table[IDX_CHAR_CFG_C] == param->write.handle && param->write.len == 2)
                {
                    uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
                    if (descr_value == 0x0001){
                        ESP_LOGI(GATTS_TABLE_TAG, "IDX_CHAR_CFG_C notify enable");
                    }else if (descr_value == 0x0002){
                        ESP_LOGI(GATTS_TABLE_TAG, "IDX_CHAR_CFG_C indicate enable");
                    }
                    else if (descr_value == 0x0000){
                        ESP_LOGI(GATTS_TABLE_TAG, "IDX_CHAR_CFG_C notify/indicate disable ");
                    }else{
                        ESP_LOGE(GATTS_TABLE_TAG, "IDX_CHAR_CFG_C unknown descr value");
                        esp_log_buffer_hex(GATTS_TABLE_TAG, param->write.value, param->write.len);
                    }
                }
                else{
                        //if(memcmp(mqtt_config.simple_protocol,"off",4)!=0 && mqtt_config.simple_protocol[0]!=0)
                        {
                            char json_buf[KLM_BLE_RX_BUF_SIZE + 1];
                            size_t json_len = param->write.len;
                            if (json_len > KLM_BLE_RX_BUF_SIZE) {
                                json_len = KLM_BLE_RX_BUF_SIZE;
                            }
                            memcpy(json_buf, param->write.value, json_len);
                            json_buf[json_len] = '\0';
                            //todo 
                            //SP-Pro need to be optimized
                            //iot_simple_protocol_json_to_int((char*)param->write.value,&ble_rx_buf[5],IOT_SIMPLE_PROTOCOL_FROM_TO_BLE);
                            ESP_LOGI(GATTS_TABLE_TAG, "Go Into Ble Pairing Process");
                            BlePairing_GetInstance()->handle(NULL, json_buf);

                        }
                    }        
                /* send response when param->write.need_rsp is true*/
                if (param->write.need_rsp){
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
                }
            }else{
                /* handle prepare write */
                example_prepare_write_event_env(gatts_if, &prepare_write_env, param);
            }
      	    break;
        case ESP_GATTS_EXEC_WRITE_EVT:
            // the length of gattc prepare write data must be less than GATTS_DEMO_CHAR_VAL_LEN_MAX.
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_EXEC_WRITE_EVT");
            example_exec_write_event_env(&prepare_write_env, param);
            break;
        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
            break;
        case ESP_GATTS_CONF_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONF_EVT, status = %d, attr_handle %d", param->conf.status, param->conf.handle);
            break;
        case ESP_GATTS_START_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "SERVICE_START_EVT, status %d, service_handle %d", param->start.status, param->start.service_handle);
            break;
        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONNECT_EVT, conn_id = %d", param->connect.conn_id);
            s_ble_connected = true;
            heart_rate_profile_tab[PROFILE_APP_IDX].conn_id = param->connect.conn_id;
            esp_log_buffer_hex(GATTS_TABLE_TAG, param->connect.remote_bda, 6);
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            /* For the iOS system, please refer to Apple official documents about the BLE connection parameters restrictions. */
            conn_params.latency = 0;
            conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
            //start sent the update connection parameters to the peer device.
            esp_ble_gap_update_conn_params(&conn_params);
            /* 启动加密请求 */
            if(memcmp(ble_pin_on_off_str, "on", 2)==0) {
                esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
            }
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);    
            s_ble_connected = false;
            if (s_legacy_advertising_enabled) {
                ble_start_advertising_if_needed("gatts_disconnect");
            }
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:{
            if (param->add_attr_tab.status != ESP_GATT_OK){
                ESP_LOGE(GATTS_TABLE_TAG, "create attribute table failed, error code=0x%x", param->add_attr_tab.status);
            }
            else if (param->add_attr_tab.num_handle != HRS_IDX_NB){
                ESP_LOGE(GATTS_TABLE_TAG, "create attribute table abnormally, num_handle (%d) \
                        doesn't equal to HRS_IDX_NB(%d)", param->add_attr_tab.num_handle, HRS_IDX_NB);
            }
            else {
                ESP_LOGI(GATTS_TABLE_TAG, "create attribute table successfully, the number handle = %d",param->add_attr_tab.num_handle);
                memcpy(heart_rate_handle_table, param->add_attr_tab.handles, sizeof(heart_rate_handle_table));
                esp_ble_gatts_start_service(heart_rate_handle_table[IDX_SVC]);
            }
            break;
        }
        case ESP_GATTS_STOP_EVT:
        case ESP_GATTS_OPEN_EVT:
        case ESP_GATTS_CANCEL_OPEN_EVT:
        case ESP_GATTS_CLOSE_EVT:
        case ESP_GATTS_LISTEN_EVT:
        case ESP_GATTS_CONGEST_EVT:
        case ESP_GATTS_UNREG_EVT:
        case ESP_GATTS_DELETE_EVT:
        default:
            break;
    }
}


static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{

    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            heart_rate_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        } else {
            ESP_LOGE(GATTS_TABLE_TAG, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
            if (gatts_if == ESP_GATT_IF_NONE || gatts_if == heart_rate_profile_tab[idx].gatts_if) {
                if (heart_rate_profile_tab[idx].gatts_cb) {
                    heart_rate_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

void read_ble_name_from_nvs(char *ble_name) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("NVS_BLE_CONFIG", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }
    
    size_t name_len = 0;
    err = nvs_get_str(nvs_handle, "ble_name", NULL, &name_len);
    if (err == ESP_OK && name_len > 0) {
        nvs_get_str(nvs_handle, "ble_name", ble_name, &name_len);
    } else {
        ble_name[0] = '\0';
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "ble info read name:%s", ble_name);
}

void save_ble_name_to_nvs(char *ble_name) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("NVS_BLE_CONFIG", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "ble_name", ble_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting ble name in NVS", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing changes to NVS", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "ble name saved to NVS %s", ble_name);
}

void read_ble_on_off_from_nvs(char *ble_on_off) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("NVS_BLE_CONFIG", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }
    
    size_t name_len = 0;
    err = nvs_get_str(nvs_handle, "ble_on_off", NULL, &name_len);
    if (err == ESP_OK && name_len > 0) {
        nvs_get_str(nvs_handle, "ble_on_off", ble_on_off, &name_len);
    } else {
        ble_on_off[0] = '\0';
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "ble info read on_off:%s", ble_on_off);
}

void save_ble_on_off_to_nvs(char *ble_on_off) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("NVS_BLE_CONFIG", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "ble_on_off", ble_on_off);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting ble name in NVS", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing changes to NVS", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    strcpy(ble_on_off_str, ble_on_off);
    ESP_LOGI(TAG, "ble on_off saved to NVS %s", ble_on_off);
}


void read_ble_pin_on_off_from_nvs(char *ble_pin_on_off) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("NVS_BLE_CONFIG", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }
    
    size_t name_len = 0;
    err = nvs_get_str(nvs_handle, "ble_pin_on_off", NULL, &name_len);
    if (err == ESP_OK && name_len > 0) {
        nvs_get_str(nvs_handle, "ble_pin_on_off", ble_pin_on_off, &name_len);
    } else {
        ble_pin_on_off[0] = '\0';
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "ble info read pin_on_off:%s", ble_pin_on_off);
}
void save_ble_pin_on_off_to_nvs(char *ble_pin_on_off) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("NVS_BLE_CONFIG", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "ble_pin_on_off", ble_pin_on_off);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting ble name in NVS", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing changes to NVS", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    strcpy(ble_pin_on_off_str, ble_pin_on_off);
    ESP_LOGI(TAG, "ble ble_pin_on_off saved to NVS %s", ble_pin_on_off);
}

void ble_init(void)
{
    esp_err_t ret;

    if (s_ble_stack_active) {
        ESP_LOGI(TAG, "BLE stack already active, skip init");
        return;
    }

    ble_log_memory_snapshot("ble/before_init");
#if BLE_TEST_DISABLE_UNUSED_RX_QUEUE
    ble_rx_queue = NULL;
    ESP_LOGI(TAG,
             "BLE RX queue disabled for memory test, len=%u item=%u",
             (unsigned)KLM_BLE_QUEUE_LENGTH,
             (unsigned)KLM_BLE_RX_BUF_SIZE);
#else
    ble_rx_queue = xQueueCreate(KLM_BLE_QUEUE_LENGTH, KLM_BLE_RX_BUF_SIZE);
    if (!ble_rx_queue) {
        ESP_LOGE(TAG,
                 "Failed to create BLE RX queue, len=%u item=%u",
                 (unsigned)KLM_BLE_QUEUE_LENGTH,
                 (unsigned)KLM_BLE_RX_BUF_SIZE);
    } else {
        ESP_LOGI(TAG,
                 "BLE RX queue created, len=%u item=%u",
                 (unsigned)KLM_BLE_QUEUE_LENGTH,
                 (unsigned)KLM_BLE_RX_BUF_SIZE);
    }
#endif

    {
        char expected_ble_name[sizeof(ble_name_str)] = {0};
        read_ble_name_from_nvs(ble_name_str);
        ble_build_expected_name(expected_ble_name, sizeof(expected_ble_name));
        if (expected_ble_name[0] != '\0' && strcmp(ble_name_str, expected_ble_name) != 0) {
            ESP_LOGI(TAG,
                     "Refresh BLE name to match current MQTT client_id/SN, old=%s new=%s",
                     ble_name_str[0] ? ble_name_str : "<empty>",
                     expected_ble_name);
            save_ble_name_to_nvs(expected_ble_name);
            read_ble_name_from_nvs(ble_name_str);
        }
    }
    read_ble_on_off_from_nvs(ble_on_off_str);
    read_ble_pin_on_off_from_nvs(ble_pin_on_off_str);
    if(ble_on_off_str[0]==0)
    {
        save_ble_on_off_to_nvs("on");
        read_ble_on_off_from_nvs(ble_on_off_str);
    }
    if(ble_pin_on_off_str[0]==0)
    {
        save_ble_pin_on_off_to_nvs("off");
        read_ble_pin_on_off_from_nvs(ble_pin_on_off_str);
    }

    if(memcmp(ble_on_off_str, "off", 3)==0)
    {
        ESP_LOGI(GATTS_TABLE_TAG, "BLE is off, forcing to on");
        save_ble_on_off_to_nvs("on");
        strcpy(ble_on_off_str, "on");
    }

    if (!s_ble_classic_mem_released) {
        ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG,
                     "release classic BT memory failed: %s",
                     esp_err_to_name(ret));
            return;
        }
        s_ble_classic_mem_released = true;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    s_ble_stack_active = true;
    ble_log_memory_snapshot("ble/after_enable");

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TABLE_TAG, "gatts register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TABLE_TAG, "gap register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TABLE_TAG, "gatts app register error, error code = %x", ret);
        return;
    }

    /* 设置安全参数 */
    if(memcmp(ble_pin_on_off_str, "off", 3)==0) {
        ESP_LOGI(GATTS_TABLE_TAG, "BLE security disabled");
    } else {
        ESP_LOGI(GATTS_TABLE_TAG, "Enabling BLE security");
        esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;     //just work 不提供MITM防护
        esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;           //设置没有IO能力，将工作在just work 模式
        if(memcmp(ble_pin_on_off_str, "on", 2)==0)
        {
            iocap = ESP_IO_CAP_OUT;           //设置IO能力为输出
            auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;     //绑定需要MITM保护
        }
        
        uint8_t key_size = 16;      //密钥大小应该在7~16字节之间
        uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
        uint8_t oob_support = ESP_BLE_OOB_DISABLE;
        
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
        
    }

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(512);
    if (local_mtu_ret){
        ESP_LOGE(GATTS_TABLE_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }

    
}

void ble_app_stop_advertising(void)
{
    esp_err_t ret;

    s_legacy_advertising_enabled = false;

    if (s_ble_advertising_starting && !s_ble_advertising_active) {
        s_ble_advertising_starting = false;
        s_ble_advertising_stopping = false;
        ESP_LOGI(GATTS_TABLE_TAG, "skip stop advertising: start not completed");
        return;
    }

    if (!s_ble_advertising_active && !s_ble_advertising_starting) {
        s_ble_advertising_stopping = false;
        ESP_LOGI(GATTS_TABLE_TAG, "skip stop advertising inactive");
        return;
    }

    if (s_ble_advertising_stopping) {
        if (!s_ble_advertising_active) {
            s_ble_advertising_stopping = false;
            ESP_LOGI(GATTS_TABLE_TAG, "clear stale advertising stop state");
            return;
        }
        ESP_LOGI(GATTS_TABLE_TAG, "skip stop advertising already stopping");
        return;
    }

    s_ble_advertising_stopping = true;
    ret = esp_ble_gap_stop_advertising();
    if (ret) {
        s_ble_advertising_stopping = false;
        ESP_LOGE(GATTS_TABLE_TAG, "stop advertising failed, error code = %x", ret);
    } else {
        s_ble_advertising_active = false;
        s_ble_advertising_starting = false;
        s_ble_advertising_stopping = false;
        ESP_LOGI(GATTS_TABLE_TAG, "stop advertising requested");
    }
}

void ble_app_shutdown(void)
{
    esp_err_t ret;

    if (!s_ble_stack_active) {
        ESP_LOGI(TAG, "BLE stack already inactive, skip shutdown");
        return;
    }

    ble_log_memory_snapshot("ble/before_shutdown");
    s_legacy_advertising_enabled = false;

    ret = esp_ble_gap_stop_advertising();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE stop advertising during shutdown returned: %s", esp_err_to_name(ret));
    }

    ret = esp_bluedroid_disable();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_bluedroid_disable returned: %s", esp_err_to_name(ret));
    }

    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_bluedroid_deinit returned: %s", esp_err_to_name(ret));
    }

    ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_controller_disable returned: %s", esp_err_to_name(ret));
    }

    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_controller_deinit returned: %s", esp_err_to_name(ret));
    }

#if !BLE_TEST_DISABLE_UNUSED_RX_QUEUE
    if (ble_rx_queue) {
        vQueueDelete(ble_rx_queue);
        ble_rx_queue = NULL;
    }
#endif

    s_ble_connected = false;
    s_ble_stack_active = false;
    s_ble_advertising_active = false;
    s_ble_advertising_starting = false;
    s_ble_advertising_stopping = false;
    ble_log_memory_snapshot("ble/after_shutdown");
}

void ble_app_start_advertising(void)
{
    if (!s_ble_stack_active) {
        ESP_LOGI(TAG, "BLE stack inactive, init on demand for advertising");
        ble_init();
        if (!s_ble_stack_active) {
            ESP_LOGE(TAG, "BLE stack init on demand failed, skip advertising");
            return;
        }
    }

    s_legacy_advertising_enabled = true;

    if (s_ble_connected) {
        ESP_LOGI(GATTS_TABLE_TAG, "skip advertising restart while BLE is connected");
        return;
    }

    esp_err_t ret = ble_start_advertising_if_needed("app_start");
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "advertising restart failed, error code = %x", ret);
    } else {
        ESP_LOGI(GATTS_TABLE_TAG, "advertising restart requested");
    }
}

bool ble_app_is_active(void)
{
    return s_ble_stack_active;
}

bool ble_app_is_connected(void)
{
    return s_ble_connected;
}






static char *esp_key_type_to_str(esp_ble_key_type_t key_type)
{
   char *key_str = NULL;
   switch(key_type) {
    case ESP_LE_KEY_NONE:
        key_str = "ESP_LE_KEY_NONE";
        break;
    case ESP_LE_KEY_PENC:
        key_str = "ESP_LE_KEY_PENC";
        break;
    case ESP_LE_KEY_PID:
        key_str = "ESP_LE_KEY_PID";
        break;
    case ESP_LE_KEY_PCSRK:
        key_str = "ESP_LE_KEY_PCSRK";
        break;
    case ESP_LE_KEY_PLK:
        key_str = "ESP_LE_KEY_PLK";
        break;
    case ESP_LE_KEY_LLK:
        key_str = "ESP_LE_KEY_LLK";
        break;
    case ESP_LE_KEY_LENC:
        key_str = "ESP_LE_KEY_LENC";
        break;
    case ESP_LE_KEY_LID:
        key_str = "ESP_LE_KEY_LID";
        break;
    case ESP_LE_KEY_LCSRK:
        key_str = "ESP_LE_KEY_LCSRK";
        break;
    default:
        key_str = "INVALID BLE KEY TYPE";
        break;
   }
   return key_str;
}

static char *esp_auth_req_to_str(esp_ble_auth_req_t auth_req)
{
   char *auth_str = NULL;
   switch(auth_req) {
    case ESP_LE_AUTH_NO_BOND:
        auth_str = "ESP_LE_AUTH_NO_BOND";
        break;
    case ESP_LE_AUTH_BOND:
        auth_str = "ESP_LE_AUTH_BOND";
        break;
    case ESP_LE_AUTH_REQ_MITM:
        auth_str = "ESP_LE_AUTH_REQ_MITM";
        break;
    case ESP_LE_AUTH_REQ_BOND_MITM:
        auth_str = "ESP_LE_AUTH_REQ_BOND_MITM";
        break;
    case ESP_LE_AUTH_REQ_SC_ONLY:
        auth_str = "ESP_LE_AUTH_REQ_SC_ONLY";
        break;
    case ESP_LE_AUTH_REQ_SC_BOND:
        auth_str = "ESP_LE_AUTH_REQ_SC_BOND";
        break;
    case ESP_LE_AUTH_REQ_SC_MITM:
        auth_str = "ESP_LE_AUTH_REQ_SC_MITM";
        break;
    case ESP_LE_AUTH_REQ_SC_MITM_BOND:
        auth_str = "ESP_LE_AUTH_REQ_SC_MITM_BOND";
        break;
    default:
        auth_str = "INVALID BLE AUTH REQ";
        break;
   }
   return auth_str;
}

static void show_bonded_devices(void)
{
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num == 0) {
        ESP_LOGI(GATTS_TABLE_TAG, "Bonded devices number zero\n");
        return;
    }

    esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
    if (!dev_list) {
        ESP_LOGI(GATTS_TABLE_TAG, "malloc failed, return\n");
        return;
    }
    esp_ble_get_bond_device_list(&dev_num, dev_list);
    ESP_LOGI(GATTS_TABLE_TAG, "Bonded devices number : %d", dev_num);

    ESP_LOGI(GATTS_TABLE_TAG, "Bonded devices list : %d", dev_num);
    for (int i = 0; i < dev_num; i++) {
        esp_log_buffer_hex(GATTS_TABLE_TAG, (void *)dev_list[i].bd_addr, sizeof(esp_bd_addr_t));
    }

    free(dev_list);
}


