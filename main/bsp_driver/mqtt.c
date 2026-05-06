#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mqtt_client.h"
#include "board_config.h"
#include "factory_cfg.h"
#include "mqtt.h"
#include "mqtt_protocol.h"
#include "mqtt_auth.h"
#include "ota_ctr.h"
#include "system_runtime.h"
#include "wifi.h"
#include "uart_ctr.h"
#include "esp_crt_bundle.h"
#include "nvs_retry.h"

#define NVS_MQTT_CONFIG "mqtt_config"
#define NVS_KEY_AUTH_GENERATED "auth_generated"
#define MQTT_TEST_USE_POLL_SWITCH_CONFIG 1
#if !MQTT_TEST_USE_POLL_SWITCH_CONFIG
#define MQTT_SWITCH_TASK_STACK_SIZE 4096
#define MQTT_SWITCH_TASK_PRIORITY 5
#endif
#define MQTT_STATUS_REPORT_DELAY_MS 3000
#define MQTT_BOOTSTRAP_SENSORS_DELAY_MS 1000
#define MQTT_BOOTSTRAP_FORMULA_DELAY_MS 1000
#define MQTT_BOOTSTRAP_META_DELAY_MS 2000

static const char *const s_mqtt_config_keys[] = {
    "broker_uri",
    "client_id",
    "username",
    "password",
    "subscribe_topic",
    "publish_topic",
    "qos",
    "keepalive",
    "deviceType",
};

static esp_err_t mqtt_mark_auth_generated_write_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    const bool *generated = (const bool *)ctx;

    if (!generated) {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_set_u8(nvs_handle, NVS_KEY_AUTH_GENERATED, *generated ? 1U : 0U);
}

static esp_err_t mqtt_params_write_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    const mqtt_config_t *config = (const mqtt_config_t *)ctx;
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_set_str(nvs_handle, "broker_uri", config->broker_uri);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "client_id", config->client_id);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "username", config->username);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "password", config->password);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "subscribe_topic", config->subscribe_topic);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "publish_topic", config->publish_topic);
    if (err == ESP_OK && config->qos[0] != 0) err = nvs_set_str(nvs_handle, "qos", config->qos);
    if (err == ESP_OK && config->keepalive[0] != 0) err = nvs_set_str(nvs_handle, "keepalive", config->keepalive);
    if (err == ESP_OK && config->deviceType[0] != 0) err = nvs_set_str(nvs_handle, "deviceType", config->deviceType);
    return err;
}
#define MQTT_CLIENT_IN_BUFFER_SIZE 4096
#define MQTT_CLIENT_OUT_BUFFER_SIZE 2048
#define MQTT_CLIENT_NETWORK_TIMEOUT_MS 30000
#define MQTT_CLIENT_OUTBOX_LIMIT_BYTES (16 * 1024)
#define MQTT_CLIENT_TASK_STACK_SIZE 4096
#define MQTT_TEST_FORCE_KEEPALIVE_SECONDS 0
#define MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS 1
#define MQTT_LOCAL_TEST_BOOTSTRAP_MIN_INTERNAL_FREE 15000
#define MQTT_LOCAL_TEST_BOOTSTRAP_MIN_LARGEST_BLOCK 9000
#define MQTT_RX_BUF_SIZE_DEFAULT        KLM_MQTT_RX_BUF_SIZE
#define MQTT_RX_BUF_SIZE_TEST           4096
#define MQTT_RX_BUF_SIZE                MQTT_RX_BUF_SIZE_TEST
#define MQTT_STATUS_REPORT_QUEUE_LEN    8

static const char *TAG = "mqtt";

mqtt_config_t mqtt_config;
static esp_mqtt_client_handle_t client;
static EXT_RAM_BSS_ATTR char mqtt_rx_buf[MQTT_RX_BUF_SIZE] = {0};
static int mqtt_socka_on_cnt = 0;
static mqtt_config_t mqtt_pending_switch_config;
#if !MQTT_TEST_USE_POLL_SWITCH_CONFIG
static TaskHandle_t mqtt_switch_task_handle = NULL;
#endif
static bool mqtt_status_publish_pending = false;
static TickType_t mqtt_status_publish_deadline = 0;
static bool mqtt_subscription_ready = false;
static bool mqtt_bootstrap_publish_pending = false;
static bool mqtt_bootstrap_publish_waiting_for_ctr_version = false;
static bool mqtt_persisted_ota_result_uploaded_this_connection = false;
static uint32_t mqtt_bootstrap_round = 0;
static bool mqtt_local_test_skip_logged = false;
static bool mqtt_auth_wait_logged = false;
static EXT_RAM_BSS_ATTR char mqtt_last_params_load_log_signature[512] = {0};
static EXT_RAM_BSS_ATTR char mqtt_last_startup_summary_log_signature[192] = {0};
typedef struct {
    bool pending;
    uint32_t section_mask;
    TickType_t deadline;
    char reason[48];
} mqtt_status_report_request_t;
typedef struct {
    bool pending;
    uint32_t section_mask;
    char reason[48];
} mqtt_immediate_status_report_request_t;
static mqtt_status_report_request_t mqtt_status_report_queue[MQTT_STATUS_REPORT_QUEUE_LEN] = {0};
static mqtt_immediate_status_report_request_t mqtt_immediate_status_report = {0};
#if MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS
typedef enum {
    MQTT_BOOTSTRAP_STATUS_PHASE_IDLE = 0,
    MQTT_BOOTSTRAP_STATUS_PHASE_SENSORS,
    MQTT_BOOTSTRAP_STATUS_PHASE_FORMULA,
    MQTT_BOOTSTRAP_STATUS_PHASE_META,
} mqtt_bootstrap_status_phase_t;
static mqtt_bootstrap_status_phase_t mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_IDLE;
#endif
#if MQTT_TEST_USE_POLL_SWITCH_CONFIG
static bool mqtt_switch_pending = false;
static TickType_t mqtt_switch_deadline = 0;
#endif
static int mqtt_get_outbox_size_safe(void);
static void mqtt_reset_status_report_queue(void);
static void mqtt_reset_immediate_status_report(void);
static bool mqtt_process_immediate_status_report(void);
static void mqtt_process_queued_status_reports(void);
static void mqtt_upload_persisted_ota_result_once(const char *reason);
static bool mqtt_try_publish_bootstrap_version_info(const char *reason);
static void mqtt_build_topics_for_client_id(mqtt_config_t *config);
static bool mqtt_resolve_preferred_client_id(char *client_id, size_t size, bool *from_factory_cfg, const char **source);
static bool mqtt_apply_identity_policy(mqtt_config_t *config, bool clear_credentials_on_factory_override, bool *credentials_cleared);
static bool mqtt_apply_credential_mode_policy(mqtt_config_t *config, bool *credentials_changed);
static void mqtt_destroy_client(void);
static uint32_t mqtt_hash_bytes(uint32_t seed, const char *value);
static uint32_t mqtt_build_params_log_fingerprint(const mqtt_config_t *config);
static uint32_t mqtt_build_startup_summary_fingerprint(const mqtt_config_t *config);
static void mqtt_log_params_load_once(const mqtt_config_t *config);
static void mqtt_log_startup_summary_once(const mqtt_config_t *config);
void mqtt_notify_power_on_reupload_needed(void);

bool mqtt_is_ui_connected(void)
{
    return sys_pra.mqtt_state == MQTT_CONNECTED && mqtt_subscription_ready;
}

static void mqtt_upload_persisted_ota_result_once(const char *reason)
{
    if (mqtt_persisted_ota_result_uploaded_this_connection) {
        ESP_LOGI(TAG,
                 "Skip persisted OTA result upload because it was already uploaded in this MQTT connection, reason=%s",
                 reason ? reason : "");
        return;
    }

    ESP_LOGI(TAG,
             "Try upload persisted softwareUpdateResult after MQTT connect, reason=%s",
             reason ? reason : "");
    if (upload_ctr_ota_info()) {
        mqtt_persisted_ota_result_uploaded_this_connection = true;
    }
}

void mqtt_notify_power_on_reupload_needed(void)
{
    mqtt_persisted_ota_result_uploaded_this_connection = false;
    mqtt_bootstrap_publish_pending = true;
    mqtt_bootstrap_publish_waiting_for_ctr_version = false;
    ESP_LOGI(TAG, "Mark persisted OTA result upload eligible again because device powered on from ST_OFF");

    if (client == NULL || sys_pra.mqtt_state != MQTT_CONNECTED || !mqtt_subscription_ready) {
        ESP_LOGI(TAG,
                 "Skip immediate MQTT bootstrap after power on because MQTT is not ready (state=%d subscribed=%d)",
                 (int)sys_pra.mqtt_state,
                 mqtt_subscription_ready ? 1 : 0);
        return;
    }

    if (!mqtt_try_publish_bootstrap_version_info("power_on_from_st_off")) {
        mqtt_bootstrap_publish_waiting_for_ctr_version = true;
        ESP_LOGI(TAG, "Defer checklatestversionInfo after power on until CTR status/version is synced");
    }

    mqtt_upload_persisted_ota_result_once("power_on_from_st_off");
}

static bool mqtt_should_skip_local_test_bootstrap_for_memory(void)
{
    size_t internal_free;
    size_t largest_block;

    if (!ctr_ota_is_waiting_for_mqtt_bootstrap()) {
        return false;
    }

    internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (internal_free >= MQTT_LOCAL_TEST_BOOTSTRAP_MIN_INTERNAL_FREE &&
        largest_block >= MQTT_LOCAL_TEST_BOOTSTRAP_MIN_LARGEST_BLOCK) {
        return false;
    }

    ESP_LOGW(TAG,
             "Skip LOCAL_TEST MQTT bootstrap due to low internal RAM: free=%u largest=%u need_free>=%u need_largest>=%u",
             (unsigned)internal_free,
             (unsigned)largest_block,
             (unsigned)MQTT_LOCAL_TEST_BOOTSTRAP_MIN_INTERNAL_FREE,
             (unsigned)MQTT_LOCAL_TEST_BOOTSTRAP_MIN_LARGEST_BLOCK);
    return true;
}

static bool mqtt_credentials_ready(const mqtt_config_t *config)
{
#if MQTT_CREDENTIAL_MODE == MQTT_CREDENTIAL_MODE_STATIC_DEFAULTS
    return config != NULL &&
           config->username[0] != '\0' &&
           config->password[0] != '\0';
#else
    return config != NULL &&
           config->username[0] != '\0' &&
           config->password[0] != '\0' &&
           mqtt_is_auth_generated();
#endif
}

static const char *mqtt_credential_mode_name(void)
{
#if MQTT_CREDENTIAL_MODE == MQTT_CREDENTIAL_MODE_STATIC_DEFAULTS
    return "STATIC_DEFAULTS";
#else
    return "DYNAMIC_AUTH";
#endif
}

static void mqtt_invalidate_auth_credentials(void)
{
    mqtt_config_t new_config = mqtt_config;

    new_config.username[0] = '\0';
    new_config.password[0] = '\0';

    if (mqtt_params_update(&new_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear MQTT credentials for re-auth");
    }

    if (mqtt_mark_auth_generated(false) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear MQTT auth_generated flag");
    }
}

bool mqtt_is_auth_generated(void)
{
    nvs_handle_t nvs_handle;
    uint8_t generated = 0;

    if (nvs_open(NVS_MQTT_CONFIG, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }

    if (nvs_get_u8(nvs_handle, NVS_KEY_AUTH_GENERATED, &generated) != ESP_OK) {
        generated = 0;
    }

    nvs_close(nvs_handle);
    return generated != 0U;
}

static void mqtt_build_topics_for_client_id(mqtt_config_t *config)
{
    if (!config || config->client_id[0] == '\0') {
        return;
    }

    snprintf(config->subscribe_topic,
             sizeof(config->subscribe_topic),
             "%s/kalerm/iot/command/controlserver/home",
             config->client_id);
    snprintf(config->publish_topic,
             sizeof(config->publish_topic),
             "stateserver/kalerm/iot/home/%s",
             config->client_id);
}

static bool mqtt_resolve_preferred_client_id(char *client_id,
                                             size_t size,
                                             bool *from_factory_cfg,
                                             const char **source)
{
    if (!client_id || size == 0U) {
        return false;
    }

    client_id[0] = '\0';
    if (from_factory_cfg) {
        *from_factory_cfg = false;
    }
    if (source) {
        *source = "none";
    }

#if MQTT_USE_FACTORY_CFG_SN
    if (factory_cfg_get_sn(client_id, size)) {
        if (from_factory_cfg) {
            *from_factory_cfg = true;
        }
        if (source) {
            *source = "factory_cfg";
        }
        return true;
    }
#endif

#if MQTT_ALLOW_COMPILE_TIME_IDENTITY_FALLBACK
    snprintf(client_id, size, "%s", MQTT_DEFAULT_CLIENT_ID);
    if (source) {
        *source = "compile_default";
    }
    return client_id[0] != '\0';
#else
    return false;
#endif
}

static bool mqtt_apply_identity_policy(mqtt_config_t *config,
                                       bool clear_credentials_on_factory_override,
                                       bool *credentials_cleared)
{
    char preferred_client_id[sizeof(config->client_id)] = {0};
    const char *source = "none";
    bool from_factory_cfg = false;
    bool client_id_changed;
    bool topics_changed;

    if (credentials_cleared) {
        *credentials_cleared = false;
    }

    if (!config) {
        return false;
    }

    if (!mqtt_resolve_preferred_client_id(preferred_client_id,
                                          sizeof(preferred_client_id),
                                          &from_factory_cfg,
                                          &source)) {
        return false;
    }

    client_id_changed =
        (config->client_id[0] == '\0') ||
        (strcmp(config->client_id, preferred_client_id) != 0);
    topics_changed =
        client_id_changed ||
        config->subscribe_topic[0] == '\0' ||
        config->publish_topic[0] == '\0';

    if (!client_id_changed && !topics_changed) {
        return false;
    }

    if (client_id_changed) {
        ESP_LOGI(TAG,
                 "Apply MQTT client identity from %s, old_client_id=%s new_client_id=%s",
                 source,
                 config->client_id[0] ? config->client_id : "<empty>",
                 preferred_client_id);
        snprintf(config->client_id, sizeof(config->client_id), "%s", preferred_client_id);
    } else {
        ESP_LOGI(TAG,
                 "Rebuild MQTT topics for client_id=%s from identity source=%s",
                 config->client_id,
                 source);
    }

    if (topics_changed) {
        mqtt_build_topics_for_client_id(config);
    }

    if (client_id_changed && from_factory_cfg && clear_credentials_on_factory_override) {
        config->username[0] = '\0';
        config->password[0] = '\0';
        if (credentials_cleared) {
            *credentials_cleared = true;
        }
        ESP_LOGI(TAG,
                 "Cleared MQTT credentials because factory_cfg SN overrides the previous client_id");
    }

    return true;
}

static bool mqtt_apply_credential_mode_policy(mqtt_config_t *config, bool *credentials_changed)
{
    bool changed = false;

    if (credentials_changed) {
        *credentials_changed = false;
    }

    if (!config) {
        return false;
    }

#if MQTT_CREDENTIAL_MODE == MQTT_CREDENTIAL_MODE_STATIC_DEFAULTS
    if (strcmp(config->username, MQTT_DEFAULT_USERNAME) != 0) {
        snprintf(config->username, sizeof(config->username), "%s", MQTT_DEFAULT_USERNAME);
        changed = true;
    }

    if (strcmp(config->password, MQTT_DEFAULT_PASSWORD) != 0) {
        snprintf(config->password, sizeof(config->password), "%s", MQTT_DEFAULT_PASSWORD);
        changed = true;
    }

    if (changed) {
        ESP_LOGI(TAG,
                 "Apply MQTT credential mode: STATIC_DEFAULTS, client_id=%s username=%s",
                 config->client_id,
                 config->username);
        if (credentials_changed) {
            *credentials_changed = true;
        }
    }
#else
    ESP_LOGI(TAG, "Apply MQTT credential mode: DYNAMIC_AUTH, client_id=%s", config->client_id);
#endif

    return changed;
}

esp_err_t mqtt_mark_auth_generated(bool generated)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_MQTT_CONFIG, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_AUTH_GENERATED, generated ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        static const char *const keys[] = {
            NVS_KEY_AUTH_GENERATED,
        };
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "mqtt auth generated flag",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   mqtt_mark_auth_generated_write_to_nvs,
                                                   &generated);
    }

    nvs_close(nvs_handle);
    return err;
}

static void mqtt_handle_ctr_version_update(const char *version)
{
    if (sys_pra.mqtt_state == MQTT_CONNECTED) {
        if (mqtt_try_publish_bootstrap_version_info("ctr_status_synced")) {
            return;
        }

        ESP_LOGI(TAG, "Publish checklatestversionInfo for CTR version update: %s", version ? version : "");
        publish_version_info_to_mqtt(&g_device_version);
    }
}

static bool mqtt_try_publish_bootstrap_version_info(const char *reason)
{
    if (!mqtt_bootstrap_publish_pending && !mqtt_bootstrap_publish_waiting_for_ctr_version) {
        return false;
    }

    if (client == NULL || sys_pra.mqtt_state != MQTT_CONNECTED || !mqtt_subscription_ready) {
        return false;
    }

    if (ctr_uart_get_last_status_tick() == 0U) {
        return false;
    }

    mqtt_bootstrap_publish_pending = false;
    mqtt_bootstrap_publish_waiting_for_ctr_version = false;
    ESP_LOGI(TAG,
             "Publish bootstrap checklatestversionInfo after %s, ctr_version=%s",
             reason ? reason : "unknown",
             g_device_version.current_ctr_fw_version);
    publish_version_info_to_mqtt(&g_device_version);
    return true;
}

static void mqtt_log_memory_snapshot(const char *phase)
{
    const char *label = phase ? phase : "snapshot";
    ESP_LOGI(TAG,
             "MQTT memory [%s]: internal_free=%d internal_min=%d largest=%d psram_free=%d outbox=%d",
             label,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             mqtt_get_outbox_size_safe());
}

void mqtt_log_memory_probe(const char *phase)
{
    mqtt_log_memory_snapshot(phase);
}

static int mqtt_get_outbox_size_safe(void)
{
    if (!client) {
        return -1;
    }

    return esp_mqtt_client_get_outbox_size(client);
}

static uint32_t mqtt_hash_bytes(uint32_t seed, const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    uint32_t hash = seed;

    if (!p) {
        return (hash ^ 0x9e3779b9U) * 16777619U;
    }

    while (*p != '\0') {
        hash ^= (uint32_t)(*p++);
        hash *= 16777619U;
    }

    hash ^= 0xffU;
    hash *= 16777619U;
    return hash;
}

static uint32_t mqtt_build_params_log_fingerprint(const mqtt_config_t *config)
{
    uint32_t hash;

    if (!config) {
        return 0U;
    }

    hash = 2166136261U;
    hash = mqtt_hash_bytes(hash, config->broker_uri);
    hash = mqtt_hash_bytes(hash, config->client_id);
    hash = mqtt_hash_bytes(hash, config->username);
    hash = mqtt_hash_bytes(hash, config->password);
    hash = mqtt_hash_bytes(hash, config->subscribe_topic);
    hash = mqtt_hash_bytes(hash, config->publish_topic);
    hash = mqtt_hash_bytes(hash, config->qos);
    hash = mqtt_hash_bytes(hash, config->keepalive);
    hash = mqtt_hash_bytes(hash, mqtt_credential_mode_name());
    hash ^= mqtt_is_auth_generated() ? 0x13579bdfU : 0U;
    hash ^= mqtt_credentials_ready(config) ? 0x2468ace0U : 0U;
    return hash;
}

static uint32_t mqtt_build_startup_summary_fingerprint(const mqtt_config_t *config)
{
    uint32_t hash;

    if (!config) {
        return 0U;
    }

    hash = 2166136261U;
    hash = mqtt_hash_bytes(hash, mqtt_credential_mode_name());
    hash = mqtt_hash_bytes(hash, config->client_id);
    hash = mqtt_hash_bytes(hash, config->username);
    hash ^= mqtt_is_auth_generated() ? 0x13579bdfU : 0U;
    hash ^= mqtt_credentials_ready(config) ? 0x2468ace0U : 0U;
    return hash;
}

static void mqtt_log_params_load_once(const mqtt_config_t *config)
{
    char signature[sizeof(mqtt_last_params_load_log_signature)] = {0};
    bool auth_generated;
    bool credentials_ready;
    uint32_t fingerprint;

    if (!config) {
        return;
    }

    auth_generated = mqtt_is_auth_generated();
    credentials_ready = mqtt_credentials_ready(config);
    fingerprint = mqtt_build_params_log_fingerprint(config);
    snprintf(signature,
             sizeof(signature),
             "%08" PRIx32 "|%d|%d",
             fingerprint,
             auth_generated ? 1 : 0,
             credentials_ready ? 1 : 0);

    if (strcmp(signature, mqtt_last_params_load_log_signature) == 0) {
        return;
    }

    snprintf(mqtt_last_params_load_log_signature,
             sizeof(mqtt_last_params_load_log_signature),
             "%s",
             signature);

    ESP_LOGI(TAG,
             "Read MQTT uri:%s, client_id:%s, username:%s, password:%s, subscribe_topic:%s, publish_topic:%s, qos:%s, keepalive:%s, mode=%s authGenerated=%d credentialsReady=%d",
             config->broker_uri,
             config->client_id,
             config->username,
             config->password,
             config->subscribe_topic,
             config->publish_topic,
             config->qos,
             config->keepalive,
             mqtt_credential_mode_name(),
             auth_generated ? 1 : 0,
             credentials_ready ? 1 : 0);
}

static void mqtt_log_startup_summary_once(const mqtt_config_t *config)
{
    char signature[sizeof(mqtt_last_startup_summary_log_signature)] = {0};
    bool auth_generated;
    bool credentials_ready;
    uint32_t fingerprint;

    if (!config) {
        return;
    }

    auth_generated = mqtt_is_auth_generated();
    credentials_ready = mqtt_credentials_ready(config);
    fingerprint = mqtt_build_startup_summary_fingerprint(config);
    snprintf(signature,
             sizeof(signature),
             "%08" PRIx32 "|%d|%d",
             fingerprint,
             auth_generated ? 1 : 0,
             credentials_ready ? 1 : 0);

    if (strcmp(signature, mqtt_last_startup_summary_log_signature) == 0) {
        return;
    }

    snprintf(mqtt_last_startup_summary_log_signature,
             sizeof(mqtt_last_startup_summary_log_signature),
             "%s",
             signature);

    ESP_LOGI(TAG,
             "MQTT startup identity summary: mode=%s client_id=%s username=%s authGenerated=%d credentialsReady=%d",
             mqtt_credential_mode_name(),
             config->client_id,
             config->username,
             auth_generated ? 1 : 0,
             credentials_ready ? 1 : 0);
}

static void mqtt_enter_runtime_mode(void)
{
    if (sys_pra.app_mode != APP_MODE_YMODEM) {
        sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
    }
}

static bool mqtt_should_dispatch_packets(void)
{
    return sys_pra.app_mode == APP_MODE_RUNTIME_BRIDGE;
}

static bool mqtt_accumulate_packet(esp_mqtt_event_handle_t event)
{
    if (!event) {
        return false;
    }

    if (event->current_data_offset == 0) {
        memset(mqtt_rx_buf, 0, sizeof(mqtt_rx_buf));
    }

    if (event->total_data_len > MQTT_RX_BUF_SIZE) {
        ESP_LOGE(TAG,
                 "MQTT packet too large: total=%d limit=%u",
                 event->total_data_len,
                 (unsigned)MQTT_RX_BUF_SIZE);
        return false;
    }

    memcpy(mqtt_rx_buf + event->current_data_offset, event->data, event->data_len);
    return event->current_data_offset + event->data_len == event->total_data_len;
}

static const char *mqtt_describe_publish_message(const char *message)
{
    if (!message) {
        return "null";
    }

    if (strstr(message, "\"checklatestversionInfo\"")) {
        return "checklatestversion";
    }

    if (strstr(message, "\"deviceInfo\"")) {
        return "device_status";
    }

    if (strstr(message, "\"controllerAck\"")) {
        return "controller_ack";
    }

    if (strstr(message, "\"cleaningAck\"")) {
        return "cleaning_ack";
    }

    if (strstr(message, "\"formulaOverallAck\"")) {
        return "formula_overall_ack";
    }

    if (strstr(message, "\"softwareUpdateInfoAck\"")) {
        return "ota_info_ack";
    }

    if (strstr(message, "\"softwareUpdateResult\"")) {
        return "ota_result";
    }

    if (strstr(message, "\"softwareUpdateStatus\"")) {
        return "ota_status";
    }

    if (strstr(message, "\"checkSoftwareStatus\"")) {
        return "ota_check_status";
    }

    return "unknown";
}

static bool mqtt_should_publish_direct(const char *kind, size_t msg_len, int qos)
{
    if (qos > 0) {
        return false;
    }

    if (kind == NULL) {
        return false;
    }

    return strcmp(kind, "checklatestversion") == 0 && msg_len <= 512U;
}

static void mqtt_schedule_delayed_status_publish(void)
{
    uint32_t delay_ms = MQTT_STATUS_REPORT_DELAY_MS;

    if (mqtt_status_publish_pending) {
        ESP_LOGW(TAG, "Delayed MQTT status publish is already pending");
        return;
    }

    mqtt_status_publish_pending = true;
#if MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS
    if (mqtt_bootstrap_status_phase == MQTT_BOOTSTRAP_STATUS_PHASE_IDLE) {
        ++mqtt_bootstrap_round;
        mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_SENSORS;
        ESP_LOGW(TAG,
                 "MQTT bootstrap round=%lu started after subscribe/reconnect",
                 (unsigned long)mqtt_bootstrap_round);
    }
    if (mqtt_bootstrap_status_phase == MQTT_BOOTSTRAP_STATUS_PHASE_SENSORS) {
        delay_ms = MQTT_BOOTSTRAP_SENSORS_DELAY_MS;
    }
    mqtt_status_publish_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    ESP_LOGI(TAG,
             "Scheduled delayed MQTT bootstrap round=%lu phase=%d in %u ms without extra task",
             (unsigned long)mqtt_bootstrap_round,
             (int)mqtt_bootstrap_status_phase,
             (unsigned int)delay_ms);
#else
    mqtt_status_publish_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    ESP_LOGI(TAG,
             "Scheduled delayed MQTT status publish in %u ms without extra task",
             (unsigned int)delay_ms);
#endif
}

bool mqtt_schedule_device_status_sections_report(uint32_t section_mask, const char *reason, uint32_t delay_ms)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t deadline = now + pdMS_TO_TICKS(delay_ms);

    for (int i = 0; i < MQTT_STATUS_REPORT_QUEUE_LEN; ++i) {
        if (!mqtt_status_report_queue[i].pending) {
            mqtt_status_report_queue[i].pending = true;
            mqtt_status_report_queue[i].section_mask = section_mask;
            mqtt_status_report_queue[i].deadline = deadline;
            snprintf(mqtt_status_report_queue[i].reason,
                     sizeof(mqtt_status_report_queue[i].reason),
                     "%s",
                     reason ? reason : "queued");
            ESP_LOGI(TAG,
                     "Queued MQTT status report idx=%d sections=0x%02x delay_ms=%u reason=%s",
                     i,
                     (unsigned int)section_mask,
                     (unsigned int)delay_ms,
                     mqtt_status_report_queue[i].reason);
            return true;
        }
    }

    ESP_LOGW(TAG,
             "Drop MQTT status report because queue is full sections=0x%02x delay_ms=%u reason=%s",
             (unsigned int)section_mask,
             (unsigned int)delay_ms,
             reason ? reason : "queued");
    return false;
}

bool mqtt_request_immediate_device_status_sections_report(uint32_t section_mask, const char *reason)
{
    mqtt_immediate_status_report.pending = true;
    mqtt_immediate_status_report.section_mask = section_mask;
    snprintf(mqtt_immediate_status_report.reason,
             sizeof(mqtt_immediate_status_report.reason),
             "%s",
             reason ? reason : "immediate");
    ESP_LOGI(TAG,
             "Requested immediate MQTT status report sections=0x%02x reason=%s",
             (unsigned int)section_mask,
             mqtt_immediate_status_report.reason);
    return true;
}

static void mqtt_reset_status_report_queue(void)
{
    memset(mqtt_status_report_queue, 0, sizeof(mqtt_status_report_queue));
}

static void mqtt_reset_immediate_status_report(void)
{
    memset(&mqtt_immediate_status_report, 0, sizeof(mqtt_immediate_status_report));
}

static bool mqtt_process_immediate_status_report(void)
{
    if (!mqtt_immediate_status_report.pending) {
        return false;
    }

    if (client == NULL || sys_pra.mqtt_state != MQTT_CONNECTED || !mqtt_subscription_ready) {
        return false;
    }

    ESP_LOGI(TAG,
             "Process immediate MQTT status report sections=0x%02x reason=%s",
             (unsigned int)mqtt_immediate_status_report.section_mask,
             mqtt_immediate_status_report.reason);
    mqtt_log_memory_snapshot("immediate_status_before");
    mqtt_report_device_status_sections(mqtt_immediate_status_report.section_mask,
                                       mqtt_immediate_status_report.reason);
    mqtt_log_memory_snapshot("immediate_status_after");
    mqtt_reset_immediate_status_report();
    return true;
}

static void mqtt_process_queued_status_reports(void)
{
    TickType_t now;

    if (client == NULL || sys_pra.mqtt_state != MQTT_CONNECTED || !mqtt_subscription_ready) {
        return;
    }

    now = xTaskGetTickCount();
    for (int i = 0; i < MQTT_STATUS_REPORT_QUEUE_LEN; ++i) {
        if (!mqtt_status_report_queue[i].pending) {
            continue;
        }

        if ((int32_t)(now - mqtt_status_report_queue[i].deadline) < 0) {
            continue;
        }

        ESP_LOGI(TAG,
                 "Process queued MQTT status report idx=%d sections=0x%02x reason=%s",
                 i,
                 (unsigned int)mqtt_status_report_queue[i].section_mask,
                 mqtt_status_report_queue[i].reason);
        mqtt_report_device_status_sections(mqtt_status_report_queue[i].section_mask,
                                           mqtt_status_report_queue[i].reason);
        mqtt_status_report_queue[i].pending = false;
        mqtt_status_report_queue[i].section_mask = 0U;
        mqtt_status_report_queue[i].deadline = 0;
        mqtt_status_report_queue[i].reason[0] = '\0';
        break;
    }
}

static void mqtt_handle_connected(esp_mqtt_client_handle_t mqtt_client)
{
    int subscribe_qos;
    int subscribe_msg_id;

    sys_pra.mqtt_state = MQTT_CONNECTED;
    mqtt_enter_runtime_mode();
    mqtt_config.auth_err = 0;
    mqtt_socka_on_cnt++;
    mqtt_subscription_ready = false;
    mqtt_bootstrap_publish_pending = true;
    mqtt_bootstrap_publish_waiting_for_ctr_version = false;
    mqtt_persisted_ota_result_uploaded_this_connection = false;

    ESP_LOGI(TAG, "Connected to broker");
    mqtt_log_memory_snapshot("connected");

    subscribe_qos = atoi(mqtt_config.qos);
    subscribe_msg_id = esp_mqtt_client_subscribe(mqtt_client, mqtt_config.subscribe_topic, subscribe_qos);
    ESP_LOGI(TAG,
             "MQTT subscribe request, topic=%s qos=%d msg_id=%d outbox=%d",
             mqtt_config.subscribe_topic,
             subscribe_qos,
             subscribe_msg_id,
             mqtt_get_outbox_size_safe());
}

static void mqtt_handle_disconnected(void)
{
    sys_pra.mqtt_state = MQTT_DISCONNECTED;
    mqtt_status_publish_pending = false;
    mqtt_status_publish_deadline = 0;
    mqtt_subscription_ready = false;
    mqtt_bootstrap_publish_pending = false;
    mqtt_bootstrap_publish_waiting_for_ctr_version = false;
    mqtt_persisted_ota_result_uploaded_this_connection = false;
    mqtt_reset_status_report_queue();
    mqtt_reset_immediate_status_report();
#if MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS
    mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_IDLE;
#endif

    if (mqtt_socka_on_cnt > 0) {
        mqtt_socka_on_cnt--;

        if (sys_pra.app_mode == APP_MODE_RUNTIME_BRIDGE &&
            sys_pra.power_off_flag == 0 &&
            wifi_scan_flag != 1) {
            /* Placeholder for a future MQTT LED update. */
        }
    }

    ESP_LOGW(TAG,
             "Disconnected from broker, client=%p auth_err=%d app_mode=%d wifi_state=%d outbox=%d",
             (void *)client,
             mqtt_config.auth_err,
             sys_pra.app_mode,
             sys_pra.wifi_state,
             mqtt_get_outbox_size_safe());

    if (mqtt_config.auth_err) {
        ESP_LOGW(TAG,
                 "MQTT broker rejected credentials, clear local MQTT credentials and request re-auth. SN=%s",
                 mqtt_config.client_id);
        mqtt_invalidate_auth_credentials();
    }
}

static void mqtt_handle_data_event(esp_mqtt_event_handle_t event)
{
    ESP_LOGI(TAG, "TOPIC=%.*s\r\n", event->topic_len, event->topic);
    ESP_LOGI(TAG, "DATA=%.*s\r\n", event->data_len, event->data);

    if (!mqtt_should_dispatch_packets()) {
        ESP_LOGW(TAG, "Ignore MQTT data while app_mode=%d", sys_pra.app_mode);
        return;
    }

    if (sys_pra.mqtt_state == MQTT_CONNECTING) {
        sys_pra.mqtt_state = MQTT_CONNECTED;
    }

    if (mqtt_accumulate_packet(event)) {
        ESP_LOGI(TAG, "MQTT full packet received, parsing...");
        handle_mqtt_message_from_mqtt(mqtt_rx_buf, event->total_data_len);
    }
}

static void mqtt_handle_before_connect(esp_mqtt_event_handle_t event)
{
    if (event && event->error_handle &&
        event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
        mqtt_config.auth_err = 1;
        ESP_LOGI(TAG, "MQTT auth err");
    }

    if (event && event->error_handle) {
        ESP_LOGI(TAG,
                 "MQTT_EVENT BEFORE_CONNECT, error_type=%d connect_return_code=%d",
                 event->error_handle->error_type,
                 event->error_handle->connect_return_code);
    } else {
        ESP_LOGI(TAG, "MQTT_EVENT BEFORE_CONNECT");
    }
}

static void mqtt_handle_error_event(esp_mqtt_event_handle_t event)
{
    if (event && event->error_handle) {
        ESP_LOGW(TAG,
                 "MQTT error occurred, type=%d connect_return_code=%d esp_tls_last_esp_err=0x%x esp_tls_stack_err=0x%x transport_sock_errno=%d",
                 event->error_handle->error_type,
                 event->error_handle->connect_return_code,
                 event->error_handle->esp_tls_last_esp_err,
                 event->error_handle->esp_tls_stack_err,
                 event->error_handle->esp_transport_sock_errno);
    } else {
        ESP_LOGI(TAG, "MQTT error occurred");
    }
}

static void mqtt_destroy_client(void)
{
    if (!client) {
        return;
    }

    if (sys_pra.mqtt_state == MQTT_CONNECTING ||
        sys_pra.mqtt_state == MQTT_CONNECTED ||
        sys_pra.mqtt_state == MQTT_DISCONNECTING) {
        esp_mqtt_client_stop(client);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_mqtt_client_destroy(client);
    client = NULL;
    sys_pra.mqtt_state = MQTT_UNINITIALIZED;
}

void mqtt_handle_auth_ready(void)
{
    mqtt_auth_wait_logged = false;
    ESP_LOGI(TAG,
             "MQTT auth ready, reset MQTT client state and continue without reboot");
    mqtt_last_params_load_log_signature[0] = '\0';
    mqtt_last_startup_summary_log_signature[0] = '\0';
    mqtt_destroy_client();
}

#if MQTT_TEST_USE_POLL_SWITCH_CONFIG
static void mqtt_process_pending_switch(void)
{
    TickType_t now;
    mqtt_config_t next_config = {0};

    if (!mqtt_switch_pending) {
        return;
    }

    now = xTaskGetTickCount();
    if ((int32_t)(now - mqtt_switch_deadline) < 0) {
        return;
    }

    mqtt_switch_pending = false;
    mqtt_switch_deadline = 0;
    memcpy(&next_config, &mqtt_pending_switch_config, sizeof(next_config));

    ESP_LOGI(TAG,
             "Process deferred MQTT env switch from main loop to broker=%s clientId=%s",
             next_config.broker_uri,
             next_config.client_id);

    mqtt_destroy_client();

    if (mqtt_params_update(&next_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist pending MQTT config");
        return;
    }

    (void)mqtt_app_start();
}
#else
static void mqtt_switch_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    mqtt_config_t next_config = {0};

    if (delay_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    memcpy(&next_config, &mqtt_pending_switch_config, sizeof(next_config));

    ESP_LOGI(TAG, "Switch MQTT env to broker=%s clientId=%s", next_config.broker_uri, next_config.client_id);

    mqtt_destroy_client();

    if (mqtt_params_update(&next_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist pending MQTT config");
        mqtt_switch_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    (void)mqtt_app_start();
    mqtt_switch_task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t mqtt_client = event->client;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_handle_connected(mqtt_client);
        break;

    case MQTT_EVENT_DISCONNECTED:
        mqtt_handle_disconnected();
        break;

    case MQTT_EVENT_SUBSCRIBED:
        mqtt_subscription_ready = true;
        ESP_LOGI(TAG, "Subscribed, msg_id=%d, bootstrap_round_next=%lu", event->msg_id, (unsigned long)(mqtt_bootstrap_round + 1U));
        if (mqtt_bootstrap_publish_pending) {
            if (!mqtt_try_publish_bootstrap_version_info("mqtt_subscribed")) {
                mqtt_bootstrap_publish_waiting_for_ctr_version = true;
                ESP_LOGI(TAG,
                         "Defer initial checklatestversion publish until CTR status/version is synced");
            }
        }
        mqtt_upload_persisted_ota_result_once("mqtt_subscribed");
        mqtt_schedule_delayed_status_publish();
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "Unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Published, msg_id=%d outbox=%d", event->msg_id, mqtt_get_outbox_size_safe());
        break;

    case MQTT_EVENT_DATA:
        mqtt_handle_data_event(event);
        break;

    case MQTT_EVENT_ERROR:
        mqtt_handle_error_event(event);
        break;

    case MQTT_EVENT_BEFORE_CONNECT:
        mqtt_handle_before_connect(event);
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static esp_err_t mqtt_params_save(const mqtt_config_t *config)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_MQTT_CONFIG, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = mqtt_params_write_to_nvs(nvs_handle, (void *)config);

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "mqtt config",
                                                   s_mqtt_config_keys,
                                                   sizeof(s_mqtt_config_keys) / sizeof(s_mqtt_config_keys[0]),
                                                   mqtt_params_write_to_nvs,
                                                   (void *)config);
    }
    nvs_close(nvs_handle);
    ESP_LOGI(TAG,
             "Write MQTT uri:%s, client_id:%s, username:%s, password:%s, subscribe_topic:%s, publish_topic:%s, qos:%s, keepalive:%s, deviceType:%s",
             config->broker_uri,
             config->client_id,
             config->username,
             config->password,
             config->subscribe_topic,
             config->publish_topic,
             config->qos,
             config->keepalive,
             config->deviceType);
    return err;
}

esp_err_t mqtt_params_load(mqtt_config_t *config)
{
    nvs_handle_t nvs_handle;
    bool config_changed = false;
    bool credentials_cleared = false;
    bool credentials_changed = false;
    esp_err_t err = nvs_open(NVS_MQTT_CONFIG, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(config->broker_uri);
    err = nvs_get_str(nvs_handle, "broker_uri", config->broker_uri, &len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    len = sizeof(config->client_id);
    err = nvs_get_str(nvs_handle, "client_id", config->client_id, &len);
    if (err != ESP_OK) {
        config->client_id[0] = '\0';
    }

    len = sizeof(config->username);
    err = nvs_get_str(nvs_handle, "username", config->username, &len);
    if (err != ESP_OK) {
        config->username[0] = '\0';
    }

    len = sizeof(config->password);
    err = nvs_get_str(nvs_handle, "password", config->password, &len);
    if (err != ESP_OK) {
        config->password[0] = '\0';
    }

    len = sizeof(config->subscribe_topic);
    err = nvs_get_str(nvs_handle, "subscribe_topic", config->subscribe_topic, &len);
    if (err != ESP_OK) {
        config->subscribe_topic[0] = '\0';
    }

    len = sizeof(config->publish_topic);
    err = nvs_get_str(nvs_handle, "publish_topic", config->publish_topic, &len);
    if (err != ESP_OK) {
        config->publish_topic[0] = '\0';
    }

    len = sizeof(config->qos);
    err = nvs_get_str(nvs_handle, "qos", config->qos, &len);
    if (err != ESP_OK) {
        strcpy(config->qos, "0");
    }

    len = sizeof(config->keepalive);
    err = nvs_get_str(nvs_handle, "keepalive", config->keepalive, &len);
    if (err != ESP_OK) {
        strcpy(config->keepalive, "60");
    }

    len = sizeof(config->deviceType);
    err = nvs_get_str(nvs_handle, "deviceType", config->deviceType, &len);
    if (err != ESP_OK) {
        memset(config->deviceType, 0, sizeof(config->deviceType));
    }

    nvs_close(nvs_handle);
    config_changed = mqtt_apply_identity_policy(config, true, &credentials_cleared);
    if (mqtt_apply_credential_mode_policy(config, &credentials_changed)) {
        config_changed = true;
    }
    if (config_changed) {
        if (mqtt_params_update(config) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist MQTT identity policy changes");
            return ESP_FAIL;
        }
        if (credentials_cleared) {
            if (mqtt_mark_auth_generated(false) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to clear MQTT auth_generated after factory_cfg override");
                return ESP_FAIL;
            }
        }
        if (credentials_changed) {
            if (mqtt_mark_auth_generated(MQTT_CREDENTIAL_MODE == MQTT_CREDENTIAL_MODE_STATIC_DEFAULTS) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to persist MQTT auth_generated after credential mode change");
                return ESP_FAIL;
            }
        }
    }
    ctr_register_version_update_handler(mqtt_handle_ctr_version_update);
    mqtt_log_params_load_once(config);
    return ESP_OK;
}

esp_err_t mqtt_params_update(const mqtt_config_t *new_config)
{
    esp_err_t err = mqtt_params_save(new_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT params to NVS");
        return err;
    }

    memcpy(&mqtt_config, new_config, sizeof(mqtt_config_t));
    return ESP_OK;
}

void mqtt_preload_identity(void)
{
    mqtt_config_t preload = mqtt_config;
    bool changed = false;

    if (mqtt_apply_identity_policy(&preload, false, NULL)) {
        changed = true;
    }

    if (mqtt_apply_credential_mode_policy(&preload, NULL)) {
        changed = true;
    }

    if (changed) {
        memcpy(&mqtt_config, &preload, sizeof(mqtt_config));
        ESP_LOGI(TAG,
                 "Preloaded MQTT identity for early consumers, client_id=%s username=%s mode=%d",
                 mqtt_config.client_id,
                 mqtt_config.username,
                 MQTT_CREDENTIAL_MODE);
    }
}

mqtt_start_result_t mqtt_app_start(void)
{
    int keepalive_seconds;

    mqtt_local_test_skip_logged = false;

    if (sys_pra.wifi_state != WIFI_CONNECTED) {
        ESP_LOGI(TAG,
                 "Skip MQTT start because Wi-Fi is not ready yet, wifi_state=%d",
                 sys_pra.wifi_state);
        return MQTT_START_SKIPPED;
    }

    if (mqtt_auth_is_in_progress()) {
        if (!mqtt_auth_wait_logged) {
            ESP_LOGI(TAG,
                     "MQTT auth is still in progress, defer MQTT start. SN=%s",
                     mqtt_config.client_id);
            mqtt_auth_wait_logged = true;
        }
        return MQTT_START_AUTH_IN_PROGRESS;
    }
    mqtt_auth_wait_logged = false;

    if (ctr_ota_should_block_mqtt_autostart()) {
        return MQTT_START_SKIPPED;
    }

    if (mqtt_should_skip_local_test_bootstrap_for_memory()) {
        ctr_ota_notify_mqtt_bootstrap_ready();
        return MQTT_START_SKIPPED;
    }

    if (sys_pra.mqtt_state == MQTT_UNINITIALIZED) {
        if (mqtt_params_load(&mqtt_config) != ESP_OK) {
#ifdef KLM_MQTT_TEST
            mqtt_config_t mqtt_config_klm = {
                .broker_uri = "mqtt://kftest1.coffee-iot.com:1987",
                .client_id = MQTT_DEFAULT_CLIENT_ID,
                .username = MQTT_DEFAULT_USERNAME,
                .password = MQTT_DEFAULT_PASSWORD,
                .subscribe_topic = MQTT_DEFAULT_SUBSCRIBE_TOPIC,
                .publish_topic = MQTT_DEFAULT_PUBLISH_TOPIC
            };
            bool credentials_cleared = false;
            bool credentials_changed = false;
            (void)mqtt_apply_identity_policy(&mqtt_config_klm, true, &credentials_cleared);
            (void)mqtt_apply_credential_mode_policy(&mqtt_config_klm, &credentials_changed);
            mqtt_params_update(&mqtt_config_klm);
            mqtt_mark_auth_generated((credentials_cleared || MQTT_CREDENTIAL_MODE == MQTT_CREDENTIAL_MODE_DYNAMIC_AUTH) ? false : true);
            if (mqtt_params_load(&mqtt_config) == ESP_OK) {
                ESP_LOGI(TAG, "Seed MQTT config from KLM_MQTT_TEST bootstrap");
            } else
#endif
            {
            ESP_LOGE(TAG, "Failed to load MQTT params from NVS");
            return MQTT_START_SKIPPED;
            }
        }

        if (strlen(mqtt_config.broker_uri) == 0) {
            ESP_LOGI(TAG, "No MQTT params found, not starting MQTT");
            if (ctr_ota_is_local_http_test_active()) {
                ctr_ota_notify_mqtt_bootstrap_ready();
            }
            return MQTT_START_SKIPPED;
        }

        if (strlen(mqtt_config.client_id) == 0) {
            ESP_LOGW(TAG, "MQTT client_id(SN) is empty, skip MQTT start");
            if (ctr_ota_is_local_http_test_active()) {
                ctr_ota_notify_mqtt_bootstrap_ready();
            }
            return MQTT_START_SKIPPED;
        }

        mqtt_log_startup_summary_once(&mqtt_config);

        if (!mqtt_credentials_ready(&mqtt_config)) {
            if (ctr_ota_is_local_http_test_active()) {
                ESP_LOGW(TAG,
                         "Skip MQTT auth during LOCAL_TEST because credentials are not ready; start local OTA directly");
                ctr_ota_notify_mqtt_bootstrap_ready();
                return MQTT_START_SKIPPED;
            }

            ESP_LOGI(TAG,
                     "MQTT credentials not ready, start one-device-one-secret auth first. SN=%s, username=%s, password=%s, authGenerated=%d",
                     mqtt_config.client_id,
                     mqtt_config.username,
                     mqtt_config.password,
                     mqtt_is_auth_generated() ? 1 : 0);
            mqtt_auth_init();
            return MQTT_START_AUTH_IN_PROGRESS;
        }

        /* Cloud subscribe topic format:
         * <client_id>/kalerm/iot/command/controlserver/home */
        keepalive_seconds = atoi(mqtt_config.keepalive);
        if (keepalive_seconds <= 0) {
            keepalive_seconds = 60;
        }
#if MQTT_TEST_FORCE_KEEPALIVE_SECONDS > 0
        keepalive_seconds = MQTT_TEST_FORCE_KEEPALIVE_SECONDS;
        ESP_LOGI(TAG, "MQTT keepalive test override: %d s", keepalive_seconds);
#endif

        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = mqtt_config.broker_uri,
            .credentials.client_id = mqtt_config.client_id,
            .credentials.username = mqtt_config.username,
            .credentials.authentication.password = mqtt_config.password,
            .session.keepalive = keepalive_seconds,
            .network.timeout_ms = MQTT_CLIENT_NETWORK_TIMEOUT_MS,
            .buffer.size = MQTT_CLIENT_IN_BUFFER_SIZE,
            .buffer.out_size = MQTT_CLIENT_OUT_BUFFER_SIZE,
            .outbox.limit = MQTT_CLIENT_OUTBOX_LIMIT_BYTES,
            .task.stack_size = MQTT_CLIENT_TASK_STACK_SIZE,
            .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
            // .broker.verification.ciphersuites_list = klm_ETSI_EN_303645_ciphersuites_list,
        };

        ESP_LOGI(TAG,
                 "MQTT client config: in_buffer=%d out_buffer=%d rx_buf=%d net_timeout=%d outbox_limit=%d task_stack=%d",
                 MQTT_CLIENT_IN_BUFFER_SIZE,
                 MQTT_CLIENT_OUT_BUFFER_SIZE,
                 MQTT_RX_BUF_SIZE,
                 MQTT_CLIENT_NETWORK_TIMEOUT_MS,
                 MQTT_CLIENT_OUTBOX_LIMIT_BYTES,
                 MQTT_CLIENT_TASK_STACK_SIZE);
        ESP_LOGI(TAG, "MQTT memory trace begin");
        mqtt_log_memory_snapshot("before_init");

        client = esp_mqtt_client_init(&mqtt_cfg);
        if (client == NULL) {
            ESP_LOGE(TAG, "esp_mqtt_client_init returned NULL");
            return MQTT_START_SKIPPED;
        }
        mqtt_log_memory_snapshot("after_init");
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        mqtt_log_memory_snapshot("before_start");
        sys_pra.mqtt_state = MQTT_CONNECTING;
        esp_err_t start_err = esp_mqtt_client_start(client);
        if (start_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(start_err));
            esp_mqtt_client_destroy(client);
            client = NULL;
            sys_pra.mqtt_state = MQTT_UNINITIALIZED;
            return MQTT_START_SKIPPED;
        }
        mqtt_log_memory_snapshot("after_start");
        return MQTT_START_STARTED;
    } else if (sys_pra.mqtt_state == MQTT_DISCONNECTING ||
               sys_pra.mqtt_state == MQTT_DISCONNECTED) {
        if (ctr_ota_is_waiting_for_mqtt_bootstrap()) {
            ESP_LOGW(TAG,
                     "LOCAL_TEST MQTT bootstrap failed or disconnected, stop retry and continue local OTA");
            ctr_ota_notify_mqtt_bootstrap_ready();
            return MQTT_START_SKIPPED;
        }

        if (!mqtt_credentials_ready(&mqtt_config)) {
            if (ctr_ota_is_local_http_test_active()) {
                ESP_LOGW(TAG,
                         "Skip MQTT re-auth during LOCAL_TEST because credentials are not ready; start local OTA directly");
                ctr_ota_notify_mqtt_bootstrap_ready();
                return MQTT_START_SKIPPED;
            }

            ESP_LOGI(TAG,
                     "MQTT disconnected and credentials are not ready, start one-device-one-secret auth. mode=%s SN=%s, username=%s, password=%s, authGenerated=%d",
                     mqtt_credential_mode_name(),
                     mqtt_config.client_id,
                     mqtt_config.username,
                     mqtt_config.password,
                     mqtt_is_auth_generated() ? 1 : 0);
            mqtt_auth_init();
            return MQTT_START_AUTH_IN_PROGRESS;
        }

        mqtt_log_memory_snapshot("restart_before_start");
        sys_pra.mqtt_state = MQTT_CONNECTING;
        esp_mqtt_client_start(client);
        mqtt_log_memory_snapshot("restart_after_start");
        return MQTT_START_STARTED;
    }

    return MQTT_START_SKIPPED;
}

void mqtt_poll(void)
{
    TickType_t now;

#if MQTT_TEST_USE_POLL_SWITCH_CONFIG
    mqtt_process_pending_switch();
#endif

    if (mqtt_process_immediate_status_report()) {
        return;
    }

    mqtt_process_queued_status_reports();

    if (!mqtt_status_publish_pending) {
        return;
    }

    if (client == NULL || sys_pra.mqtt_state != MQTT_CONNECTED) {
        ESP_LOGW(TAG,
                 "Cancel delayed MQTT status publish, client=%p mqtt_state=%d",
                 (void *)client,
                 sys_pra.mqtt_state);
        mqtt_status_publish_pending = false;
        mqtt_status_publish_deadline = 0;
#if MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS
        mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_IDLE;
#endif
        return;
    }

    if (!mqtt_subscription_ready) {
        return;
    }

    now = xTaskGetTickCount();
    if ((int32_t)(now - mqtt_status_publish_deadline) < 0) {
        return;
    }

#if MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS
    switch (mqtt_bootstrap_status_phase) {
    case MQTT_BOOTSTRAP_STATUS_PHASE_SENSORS:
        ESP_LOGI(TAG,
                 "Delayed MQTT bootstrap round=%lu phase=sensors triggered after %u ms from main loop",
                 (unsigned long)mqtt_bootstrap_round,
                 (unsigned int)MQTT_BOOTSTRAP_SENSORS_DELAY_MS);
        mqtt_report_device_status_sections(MQTT_DEVICE_STATUS_SECTION_SENSORS, "bootstrap_sensors");
        mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_FORMULA;
        mqtt_status_publish_deadline = now + pdMS_TO_TICKS(MQTT_BOOTSTRAP_FORMULA_DELAY_MS);
        ESP_LOGI(TAG,
                 "Scheduled delayed MQTT bootstrap status phase=formula in %u ms without extra task",
                 (unsigned int)MQTT_BOOTSTRAP_FORMULA_DELAY_MS);
        return;

    case MQTT_BOOTSTRAP_STATUS_PHASE_FORMULA:
        ESP_LOGI(TAG,
                 "Delayed MQTT bootstrap round=%lu phase=formula triggered after %u ms from main loop",
                 (unsigned long)mqtt_bootstrap_round,
                 (unsigned int)MQTT_BOOTSTRAP_FORMULA_DELAY_MS);
        mqtt_report_device_status_sections(MQTT_DEVICE_STATUS_SECTION_FORMULA_OVERALL, "bootstrap_formula");
        mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_META;
        mqtt_status_publish_deadline = now + pdMS_TO_TICKS(MQTT_BOOTSTRAP_META_DELAY_MS);
        ESP_LOGI(TAG,
                 "Scheduled delayed MQTT bootstrap round=%lu phase=meta in %u ms without extra task",
                 (unsigned long)mqtt_bootstrap_round,
                 (unsigned int)MQTT_BOOTSTRAP_META_DELAY_MS);
        return;

    case MQTT_BOOTSTRAP_STATUS_PHASE_META:
        ESP_LOGI(TAG,
                 "Delayed MQTT bootstrap round=%lu phase=meta triggered after %u ms from main loop",
                 (unsigned long)mqtt_bootstrap_round,
                 (unsigned int)MQTT_BOOTSTRAP_META_DELAY_MS);
        mqtt_report_device_status_sections(MQTT_DEVICE_STATUS_SECTION_CALIBRATION |
                                               MQTT_DEVICE_STATUS_SECTION_STATISTICS |
                                               MQTT_DEVICE_STATUS_SECTION_SETTING |
                                               MQTT_DEVICE_STATUS_SECTION_WIFI,
                                           "bootstrap_meta");
        ESP_LOGI(TAG, "MQTT bootstrap round=%lu deviceInfo finished, check persisted softwareUpdateResult upload state",
                 (unsigned long)mqtt_bootstrap_round);
        mqtt_upload_persisted_ota_result_once("mqtt_bootstrap_finished");
        mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_IDLE;
        mqtt_status_publish_pending = false;
        mqtt_status_publish_deadline = 0;
        if (ctr_ota_is_local_http_test_active()) {
            ctr_ota_notify_mqtt_bootstrap_ready();
        }
        return;

    case MQTT_BOOTSTRAP_STATUS_PHASE_IDLE:
    default:
        mqtt_status_publish_pending = false;
        mqtt_status_publish_deadline = 0;
        return;
    }
#else
    mqtt_status_publish_pending = false;
    mqtt_status_publish_deadline = 0;

    ESP_LOGI(TAG,
             "Delayed MQTT status publish triggered after %d ms from main loop",
             MQTT_STATUS_REPORT_DELAY_MS);
    mqtt_report_device_status();
    ESP_LOGI(TAG, "MQTT bootstrap deviceInfo finished, check persisted softwareUpdateResult upload state");
    mqtt_upload_persisted_ota_result_once("mqtt_bootstrap_finished");
#endif
}

void mqtt_app_stop(void)
{
    if (!client) {
        return;
    }

    if (sys_pra.mqtt_state == MQTT_CONNECTING || sys_pra.mqtt_state == MQTT_CONNECTED) {
        sys_pra.mqtt_state = MQTT_DISCONNECTING;
        esp_mqtt_client_stop(client);
        sys_pra.mqtt_state = MQTT_DISCONNECTED;
        mqtt_status_publish_pending = false;
        mqtt_status_publish_deadline = 0;
        mqtt_subscription_ready = false;
        mqtt_bootstrap_publish_pending = false;
        mqtt_bootstrap_publish_waiting_for_ctr_version = false;
        mqtt_persisted_ota_result_uploaded_this_connection = false;
        mqtt_reset_status_report_queue();
        mqtt_reset_immediate_status_report();
#if MQTT_TEST_SPLIT_BOOTSTRAP_DEVICE_STATUS
        mqtt_bootstrap_status_phase = MQTT_BOOTSTRAP_STATUS_PHASE_IDLE;
#endif
    }
}

void mqtt_app_shutdown(void)
{
    mqtt_destroy_client();
}

void mqtt_publish_message(const char *message)
{
    const char *kind = mqtt_describe_publish_message(message);
    size_t msg_len = message ? strlen(message) : 0U;
    int outbox_before;
    int outbox_after;
    int qos;
    int msg_id;
    bool direct_publish;

    if (!client) {
        ESP_LOGW(TAG, "Skip MQTT publish because client is null");
        return;
    }

    outbox_before = mqtt_get_outbox_size_safe();
    qos = atoi(mqtt_config.qos);
    direct_publish = mqtt_should_publish_direct(kind, msg_len, qos);

    ESP_LOGI(TAG,
             "MQTT publish request, kind=%s len=%u qos=%d direct=%d topic=%s outbox_before=%d",
             kind,
             (unsigned int)msg_len,
             qos,
             direct_publish ? 1 : 0,
             mqtt_config.publish_topic,
             outbox_before);

    if (direct_publish) {
        msg_id = esp_mqtt_client_publish(client,
                                         mqtt_config.publish_topic,
                                         message,
                                         0,
                                         0,
                                         0);
    } else {
        msg_id = esp_mqtt_client_enqueue(client,
                                         mqtt_config.publish_topic,
                                         message,
                                         0,
                                         qos,
                                         0,
                                         1);
    }
    outbox_after = mqtt_get_outbox_size_safe();
    if (msg_id < 0) {
        ESP_LOGE(TAG,
                 "Failed to publish MQTT message, kind=%s qos=%d direct=%d msg_id=%d outbox_before=%d outbox_after=%d %s %s",
                 kind,
                 qos,
                 direct_publish ? 1 : 0,
                 msg_id,
                 outbox_before,
                 outbox_after,
                 mqtt_config.publish_topic,
                 message);
    } else {
        ESP_LOGI(TAG,
                 "MQTT publish accepted, kind=%s qos=%d direct=%d msg_id=%d outbox_before=%d outbox_after=%d topic=%s %s",
                 kind,
                 qos,
                 direct_publish ? 1 : 0,
                 msg_id,
                 outbox_before,
                 outbox_after,
                 mqtt_config.publish_topic,
                 message);
    }
}

esp_err_t mqtt_schedule_switch_config(const mqtt_config_t *new_config, uint32_t delay_ms)
{
    if (!new_config) {
        return ESP_ERR_INVALID_ARG;
    }

#if MQTT_TEST_USE_POLL_SWITCH_CONFIG
    if (mqtt_switch_pending) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&mqtt_pending_switch_config, new_config, sizeof(mqtt_pending_switch_config));
    mqtt_switch_pending = true;
    mqtt_switch_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    ESP_LOGI(TAG,
             "Scheduled MQTT env switch via main loop in %u ms to broker=%s clientId=%s",
             (unsigned)delay_ms,
             mqtt_pending_switch_config.broker_uri,
             mqtt_pending_switch_config.client_id);
    return ESP_OK;
#else
    BaseType_t ok;

    if (mqtt_switch_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&mqtt_pending_switch_config, new_config, sizeof(mqtt_pending_switch_config));
    ok = xTaskCreate(mqtt_switch_task,
                     "mqtt_switch_task",
                     MQTT_SWITCH_TASK_STACK_SIZE,
                     (void *)(uintptr_t)delay_ms,
                     MQTT_SWITCH_TASK_PRIORITY,
                     &mqtt_switch_task_handle);
    if (ok != pdPASS) {
        mqtt_switch_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
#endif
}

