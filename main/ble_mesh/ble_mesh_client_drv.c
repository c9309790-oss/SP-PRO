#include "ble_mesh_client_drv.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_sensor_model_api.h"
#include "esp_log.h"
#include "string.h"

#define TAG "MESH_CLIENT_DRV"
#define CID_ESP 0x02E5

#define MAX_CBS 3

typedef struct {
    uint16_t filter_addr;
    client_sensor_cb_t cb;
} sensor_cb_node_t;

typedef struct {
    uint16_t filter_addr;
    client_onoff_status_cb_t cb;
} onoff_status_cb_node_t;

typedef struct {
    uint16_t filter_addr;
    client_onoff_timeout_cb_t cb;
} onoff_timeout_cb_node_t;

static sensor_cb_node_t s_sensor_callbacks[MAX_CBS];
static onoff_status_cb_node_t s_onoff_status_callbacks[MAX_CBS];
static onoff_timeout_cb_node_t s_onoff_timeout_callbacks[MAX_CBS];

static esp_ble_mesh_client_t g_onoff_client;
static esp_ble_mesh_client_t g_sensor_client;
static uint8_t dev_uuid[16] = {0xdd, 0xdd};

/* 配置伺服器 */
static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

/* 發佈上下文 */
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(sensor_cli_pub, 2 + 8, ROLE_NODE);

/* 模型定義 */
static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, &g_onoff_client),
    ESP_BLE_MESH_MODEL_SENSOR_CLI(&sensor_cli_pub, &g_sensor_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
};

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    if (event == ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT) {
        ESP_LOGI(TAG, "Provisioned: addr=0x%04x", param->node_prov_complete.addr);
    }
}

static void generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                                esp_ble_mesh_generic_client_cb_param_t *param)
{
    uint16_t addr;
    switch (event) {
    case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
    case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
        if (param->error_code == 0) {
            bool onoff = param->status_cb.onoff_status.present_onoff;
            addr = param->params->ctx.addr;
            for (int i = 0; i < MAX_CBS; i++) {
                if (s_onoff_status_callbacks[i].cb && s_onoff_status_callbacks[i].filter_addr == addr) {
                    s_onoff_status_callbacks[i].cb(addr, onoff);
                }
            }
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
        addr = param->params->ctx.addr;
        for (int i = 0; i < MAX_CBS; i++) {
            if (s_onoff_timeout_callbacks[i].cb && s_onoff_timeout_callbacks[i].filter_addr == addr) {
                s_onoff_timeout_callbacks[i].cb(addr);
            }
        }
        break;
    default:
        break;
    }
}

static void sensor_client_cb(esp_ble_mesh_sensor_client_cb_event_t event,
                             esp_ble_mesh_sensor_client_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_SENSOR_CLIENT_PUBLISH_EVT) {
        struct net_buf_simple *buf = param->status_cb.sensor_status.marshalled_sensor_data;
        uint16_t src = param->params->ctx.addr;
        uint16_t dst = param->params->ctx.recv_dst;
        for (int i = 0; i < MAX_CBS; i++) {
            if (s_sensor_callbacks[i].cb && s_sensor_callbacks[i].filter_addr == dst) {
                struct net_buf_simple dup_buf;
                memcpy(&dup_buf, buf, sizeof(dup_buf));
                s_sensor_callbacks[i].cb(src, dst, &dup_buf);
            }
        }
    }
}

void ble_mesh_client_drv_register_sensor_cb(uint16_t group_addr, client_sensor_cb_t cb) {
    for (int i = 0; i < MAX_CBS; i++) {
        if (s_sensor_callbacks[i].cb == NULL) {
            s_sensor_callbacks[i].filter_addr = group_addr;
            s_sensor_callbacks[i].cb = cb;
            return;
        }
    }
}

void ble_mesh_client_drv_register_onoff_status_cb(uint16_t node_addr, client_onoff_status_cb_t cb) {
    for (int i = 0; i < MAX_CBS; i++) {
        if (s_onoff_status_callbacks[i].cb == NULL) {
            s_onoff_status_callbacks[i].filter_addr = node_addr;
            s_onoff_status_callbacks[i].cb = cb;
            return;
        }
    }
}

void ble_mesh_client_drv_register_onoff_timeout_cb(uint16_t node_addr, client_onoff_timeout_cb_t cb) {
    for (int i = 0; i < MAX_CBS; i++) {
        if (s_onoff_timeout_callbacks[i].cb == NULL) {
            s_onoff_timeout_callbacks[i].filter_addr = node_addr;
            s_onoff_timeout_callbacks[i].cb = cb;
            return;
        }
    }
}

void ble_mesh_client_drv_unregister_onoff_status_cb(uint16_t node_addr, client_onoff_status_cb_t cb) {
    for (int i = 0; i < MAX_CBS; i++) {
        if (s_onoff_status_callbacks[i].cb == cb && s_onoff_status_callbacks[i].filter_addr == node_addr) {
            s_onoff_status_callbacks[i].filter_addr = 0;
            s_onoff_status_callbacks[i].cb = NULL;
            return;
        }
    }
}

void ble_mesh_client_drv_unregister_onoff_timeout_cb(uint16_t node_addr, client_onoff_timeout_cb_t cb) {
    for (int i = 0; i < MAX_CBS; i++) {
        if (s_onoff_timeout_callbacks[i].cb == cb && s_onoff_timeout_callbacks[i].filter_addr == node_addr) {
            s_onoff_timeout_callbacks[i].filter_addr = 0;
            s_onoff_timeout_callbacks[i].cb = NULL;
            return;
        }
    }
}

void ble_mesh_client_drv_send_onoff(uint16_t node_addr, uint8_t app_key_idx, bool onoff)
{
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    esp_ble_mesh_client_common_param_t common = {0};
    common.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET;
    common.model = g_onoff_client.model;
    common.ctx.net_idx = 0;
    common.ctx.app_idx = app_key_idx;
    common.ctx.addr = node_addr;
    common.ctx.send_ttl = 3;
    common.msg_timeout = 300;
    set_state.onoff_set.op_en = false;
    set_state.onoff_set.onoff = onoff ? 1 : 0;
    set_state.onoff_set.tid = 0;
    esp_err_t err = esp_ble_mesh_generic_client_set_state(&common, &set_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send OnOff Set: %s", esp_err_to_name(err));
    }
}

esp_err_t ble_mesh_client_drv_init(void) {
    memset(s_sensor_callbacks, 0, sizeof(s_sensor_callbacks));
    memset(s_onoff_status_callbacks, 0, sizeof(s_onoff_status_callbacks));
    memset(s_onoff_timeout_callbacks, 0, sizeof(s_onoff_timeout_callbacks));

    esp_ble_mesh_register_prov_callback(prov_cb);
    esp_ble_mesh_register_generic_client_callback(generic_client_cb);
    esp_ble_mesh_register_sensor_client_callback(sensor_client_cb);
    
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));
    ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));

    onoff_cli_pub.retransmit = ESP_BLE_MESH_TRANSMIT(2, 2);  // count=2, interval_steps=2
    ESP_LOGI(TAG, "BLE Mesh Client Driver initialized");
    return ESP_OK;
}