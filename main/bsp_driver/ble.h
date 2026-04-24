#ifndef BLE_H
#define BLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#define PROFILE_NUM                 1

/* Attributes State Machine */
typedef enum
{
    IDX_SVC,
    IDX_CHAR_A,
    IDX_CHAR_VAL_A,

    IDX_CHAR_B,
    IDX_CHAR_VAL_B,
    IDX_CHAR_CFG_B,

    IDX_CHAR_C,
    IDX_CHAR_VAL_C,
    IDX_CHAR_CFG_C,

    IDX_CHAR_D,
    IDX_CHAR_VAL_D,

    HRS_IDX_NB,
}BLE_IDX_ENUM_type;


typedef struct  {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
}gatts_profile_inst_typ;

extern gatts_profile_inst_typ heart_rate_profile_tab[PROFILE_NUM];

extern QueueHandle_t ble_rx_queue;
extern uint16_t heart_rate_handle_table[HRS_IDX_NB];

extern char ble_name_str[30],ble_on_off_str[30],ble_pin_on_off_str[30];
void save_ble_name_to_nvs(char *ble_name);
void save_ble_on_off_to_nvs(char *ble_on_off);
void save_ble_pin_on_off_to_nvs(char *ble_on_off);
void ble_init(void);
void ble_app_start_advertising(void);
void ble_app_stop_advertising(void);
void ble_app_shutdown(void);
bool ble_app_is_active(void);
bool ble_app_is_connected(void);
#endif 

