#ifndef SYSTEM_RUNTIME_H
#define SYSTEM_RUNTIME_H

typedef enum {
    APP_MODE_BT_PAIRING = 0,     /* BLE pairing / provisioning mode. */
    APP_MODE_MQTT_CONFIG,        /* Waiting for Wi-Fi and MQTT startup. */
    APP_MODE_MQTT_CLIENT,        /* Reserved legacy MQTT client mode. */
    APP_MODE_BLE_MESH_CONFIG,    /* Transitional BLE Mesh setup mode. */
    APP_MODE_RUNTIME_BRIDGE,     /* Normal runtime bridge between HMI, CTR, and MQTT. */
    APP_MODE_YMODEM,             /* UART YMODEM transfer mode. */
} app_mode_t;

typedef enum {
    MQTT_UNINITIALIZED = 0,  /* MQTT client not started yet. */
    MQTT_CONNECTING,         /* MQTT client is connecting. */
    MQTT_CONNECTED,          /* MQTT session is active. */
    MQTT_DISCONNECTING,      /* MQTT client is stopping. */
    MQTT_DISCONNECTED,       /* MQTT client is offline. */
} MQTT_STATUS_TYPE;

typedef enum {
    WIFI_DISCONNECTED = 0,  /* STA is offline. */
    WIFI_CONNECTED,         /* STA has an IP address. */
} WIFI_STATUS_TYPE;

typedef struct {
    WIFI_STATUS_TYPE wifi_state;
    MQTT_STATUS_TYPE mqtt_state;
    app_mode_t app_mode;
    int power_off_flag;
} SYSTEM_PARA_TYP;

extern SYSTEM_PARA_TYP sys_pra;
extern const int klm_ETSI_EN_303645_ciphersuites_list[];

#endif /* SYSTEM_RUNTIME_H */
