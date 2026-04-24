#ifndef MQTT_H
#define MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_event.h"
#include "mqtt_client.h"

#ifndef MQTT_USE_FACTORY_CFG_SN
#define MQTT_USE_FACTORY_CFG_SN 1
#endif

#ifndef MQTT_ALLOW_COMPILE_TIME_IDENTITY_FALLBACK
#define MQTT_ALLOW_COMPILE_TIME_IDENTITY_FALLBACK 1
#endif

#define MQTT_CREDENTIAL_MODE_STATIC_DEFAULTS 0
#define MQTT_CREDENTIAL_MODE_DYNAMIC_AUTH    1

#ifndef MQTT_CREDENTIAL_MODE
#define MQTT_CREDENTIAL_MODE MQTT_CREDENTIAL_MODE_DYNAMIC_AUTH
#endif

#define MQTT_DEFAULT_PROFILE_TESTER   0
#define MQTT_DEFAULT_PROFILE_PERSONAL 1

#ifndef MQTT_DEFAULT_PROFILE
#define MQTT_DEFAULT_PROFILE MQTT_DEFAULT_PROFILE_PERSONAL
#endif

#define MQTT_TESTER_CLIENT_ID       "SPPRO67896789"
#define MQTT_TESTER_USERNAME        "embedded|nonce$fcex"
#define MQTT_TESTER_PASSWORD        "zxcYlEtarSJloBk3Fr1tCgUW18hDT7"

#define MQTT_PERSONAL_CLIENT_ID     "SPPRO1234567"
#define MQTT_PERSONAL_USERNAME      "embedded|nonce$cTXP"
#define MQTT_PERSONAL_PASSWORD      "MI31Np7wQYDk8dHB9KKSszW1ECYNs4"

#if MQTT_DEFAULT_PROFILE == MQTT_DEFAULT_PROFILE_PERSONAL
#define MQTT_DEFAULT_CLIENT_ID      MQTT_PERSONAL_CLIENT_ID
#define MQTT_DEFAULT_USERNAME       MQTT_PERSONAL_USERNAME
#define MQTT_DEFAULT_PASSWORD       MQTT_PERSONAL_PASSWORD
#else
#define MQTT_DEFAULT_CLIENT_ID      MQTT_TESTER_CLIENT_ID
#define MQTT_DEFAULT_USERNAME       MQTT_TESTER_USERNAME
#define MQTT_DEFAULT_PASSWORD       MQTT_TESTER_PASSWORD
#endif

#define MQTT_DEFAULT_SUBSCRIBE_TOPIC MQTT_DEFAULT_CLIENT_ID "/kalerm/iot/command/controlserver/home"
#define MQTT_DEFAULT_PUBLISH_TOPIC   "stateserver/kalerm/iot/home/" MQTT_DEFAULT_CLIENT_ID

typedef enum {
    MQTT_START_SKIPPED = 0,
    MQTT_START_STARTED,
    MQTT_START_AUTH_IN_PROGRESS,
} mqtt_start_result_t;

typedef struct {
    char broker_uri[128];
    char client_id[64];
    char username[64];
    char password[64];
    char subscribe_topic[128]; /* subscribe topic */
    char publish_topic[128];   /* publish topic */
    char qos[2];
    char keepalive[10];
    char deviceType[30];
    int auth_err;
} mqtt_config_t;

extern mqtt_config_t mqtt_config;
esp_err_t mqtt_params_load(mqtt_config_t *config);
mqtt_start_result_t mqtt_app_start(void);
void mqtt_app_stop(void);
void mqtt_app_shutdown(void);
void mqtt_preload_identity(void);
void mqtt_para_check(void);
void mqtt_poll(void);
void mqtt_publish_message(const char *message);
void mqtt_log_memory_probe(const char *phase);
void mqtt_notify_power_on_reupload_needed(void);
bool mqtt_schedule_device_status_sections_report(uint32_t section_mask, const char *reason, uint32_t delay_ms);
bool mqtt_request_immediate_device_status_sections_report(uint32_t section_mask, const char *reason);
esp_err_t mqtt_params_update(const mqtt_config_t *new_config);
esp_err_t mqtt_schedule_switch_config(const mqtt_config_t *new_config, uint32_t delay_ms);
bool mqtt_is_auth_generated(void);
bool mqtt_is_ui_connected(void);
esp_err_t mqtt_mark_auth_generated(bool generated);
void mqtt_handle_auth_ready(void);

#endif /* MQTT_H */
