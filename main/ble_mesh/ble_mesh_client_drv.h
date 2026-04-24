#ifndef _BLE_MESH_CLIENT_DRV_H_
#define _BLE_MESH_CLIENT_DRV_H_

#include "esp_ble_mesh_defs.h"
#include <stdbool.h>

typedef void (* client_sensor_cb_t)(uint16_t src_addr, uint16_t dst_addr, struct net_buf_simple *data);
typedef void (* client_onoff_status_cb_t)(uint16_t src_addr, bool onoff);
typedef void (* client_onoff_timeout_cb_t)(uint16_t dst_addr);

esp_err_t ble_mesh_client_drv_init(void);

void ble_mesh_client_drv_register_sensor_cb(uint16_t group_addr, client_sensor_cb_t cb);
void ble_mesh_client_drv_register_onoff_status_cb(uint16_t node_addr, client_onoff_status_cb_t cb);
void ble_mesh_client_drv_register_onoff_timeout_cb(uint16_t node_addr, client_onoff_timeout_cb_t cb);
void ble_mesh_client_drv_unregister_onoff_status_cb(uint16_t node_addr, client_onoff_status_cb_t cb);
void ble_mesh_client_drv_unregister_onoff_timeout_cb(uint16_t node_addr, client_onoff_timeout_cb_t cb);

void ble_mesh_client_drv_send_onoff(uint16_t node_addr, uint8_t app_key_idx, bool onoff);

#endif // _BLE_MESH_CLIENT_DRV_H_