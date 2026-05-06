#include "ota_ctr.h"
#include "sp_pro_app_ctrl.h"
#include "nvs_flash.h"
#include "board_config.h"
#include "system_runtime.h"
#include "driver/uart.h"
#include "uart_ctr.h"
#include "esp_log.h"
#include "string.h"
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_attr.h"
#include "mbedtls/md5.h"
#include "Ymodem_esp.h"
#include "mqtt_protocol.h"
#include "ota_bundle.h"
#include "ota_esp32.h"
#include "esp_heap_caps.h"
#include "mqtt.h"
#include "wifi.h"
#include "mqtt_protocol_core.h"
#include "sp_pro_app.h"
#include "nvs_retry.h"

#define CTR_OTA_PACKAGE_HEADER_LEN 0x800U

typedef struct {
    uint32_t crc16;
    uint32_t app_size;
    uint8_t reserved[CTR_OTA_PACKAGE_HEADER_LEN - 8U];
} app_head_t;
EXT_RAM_BSS_ATTR app_head_t app_head = {0};

EXT_RAM_BSS_ATTR ota_info_t g_ota_info={0};
uint8_t g_ota_new_url_pending=0;

#define NVS_CTR_OTA "ota_info"
#define CTR_OTA_TASK_STACK_SIZE 8192
#define CTR_OTA_TASK_STACK_WORDS ((CTR_OTA_TASK_STACK_SIZE + sizeof(StackType_t) - 1) / sizeof(StackType_t))
static const char *TAG = "CTR_OTA";
uint8_t ctr_ota_run_flag=0;
static volatile uint8_t ctr_ota_result_ack_received = 0;
static portMUX_TYPE ctr_ota_task_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR ota_package_probe_result_t g_ota_package_probe = {0};
static StaticTask_t s_ctr_ota_task_tcb;
static StackType_t s_ctr_ota_task_stack[CTR_OTA_TASK_STACK_WORDS];
static volatile bool s_local_http_test_waiting_for_wifi = false;
static volatile bool s_local_http_test_waiting_for_mqtt_bootstrap = false;
#define CTR_OTA_WAIT_NEW_URL_TIMEOUT_MS 30000
#define CTR_OTA_RESULT_ACK_RETRY_MS 5000
#define CTR_OTA_APPLY_TIMEOUT_MS 30000
#define CTR_OTA_HANDSHAKE_TIMEOUT_MS 30000
#define CTR_OTA_BOOT_WAIT_LOG_MS 2000
#define CTR_OTA_SUCCESS_VOICE_WAIT_MS 10000
#define OTA_LOCAL_SIM_TEST 0
#define OTA_LOCAL_HTTP_TEST_ENABLE 0
#define OTA_LOCAL_HTTP_TEST_URL "http://192.168.39.148:8000/ota_ctr_esp32_20260410_165510.bin"
#define OTA_LOCAL_HTTP_TEST_MD5 "5F47166BD5D104FE1C7B7D23413DD4CA"
#define OTA_LOCAL_HTTP_TEST_FILE_NAME "ota_ctr_esp32_20260410_165510.bin"
#define OTA_LOCAL_HTTP_TEST_TASK_ID "local-http-task-001"
#define OTA_LOCAL_HTTP_TEST_MSG_ID "local-http-msg-001"
#define OTA_TEST_ALLOW_CTR_UPGRADE_WHEN_VERSION_DIFF 1
#define CTR_OTA_SIM_CONFIRM_DELAY_MS 3000
#define CTR_OTA_SIM_BOOT_C_DELAY_MS 2000
#define CTR_OTA_SIM_APPLY_DELAY_MS 1500
static const char *ota_nvs_name_arr[] = {
    "ota_url",
    "ota_md5",
    "ota_file_name",
    "ota_dtype",
    "ota_stype",
    "ota_tkid",
    "ota_file_path",
    "ota_autoUp",
    "ota_msgid",
    "ota_sta",
    "ota_error_info",
};

typedef struct {
    OTA_INFOIDX_TYP idx;
    ota_info_t *info;
    const char *str_value;
} ota_nvs_write_ctx_t;

static esp_err_t ota_write_single_param_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    const ota_nvs_write_ctx_t *write_ctx = (const ota_nvs_write_ctx_t *)ctx;

    if (!write_ctx || !write_ctx->info || write_ctx->idx > OTA_INFO_ERROR_INFO) {
        return ESP_ERR_INVALID_ARG;
    }

    if (write_ctx->idx == OTA_INFO_STA) {
        return nvs_set_u8(nvs_handle, ota_nvs_name_arr[write_ctx->idx], write_ctx->info->ota_sta);
    }

    if (write_ctx->idx == OTA_INFO_ERROR_INFO) {
        return nvs_set_u8(nvs_handle, ota_nvs_name_arr[write_ctx->idx], write_ctx->info->ota_error_info);
    }

    return nvs_set_str(nvs_handle,
                       ota_nvs_name_arr[write_ctx->idx],
                       write_ctx->str_value ? write_ctx->str_value : "");
}

static esp_err_t ota_write_reset_context_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    (void)ctx;

    for (int idx = 0; idx <= OTA_INFO_ERROR_INFO; ++idx) {
        esp_err_t err;

        if (idx == OTA_INFO_STA || idx == OTA_INFO_ERROR_INFO) {
            err = nvs_set_u8(nvs_handle, ota_nvs_name_arr[idx], 0);
        } else {
            err = nvs_set_str(nvs_handle, ota_nvs_name_arr[idx], "");
        }

        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static const char *ctr_ota_state_name(IOT_SIMPLE_OTA_STA_TYP state)
{
    switch (state) {
    case IOT_SIMPEL_OTA_NULL:
        return "NULL";
    case IOT_SIMPLE_OTA_URL_GOT:
        return "URL_GOT";
    case IOT_SIMPLE_OTA_URL_AUTH_FAIL:
        return "URL_AUTH_FAIL";
    case IOT_SIMPLE_OTA_CRC_OK:
        return "CRC_OK";
    case IOT_SIMPLE_OTA_PACK_CHECK_END:
        return "PACK_CHECK_END";
    case IOT_SIMPLE_OTA_WAIT_CONFIRM:
        return "WAIT_CONFIRM";
    case IOT_SIMPLE_OTA_YMODEM:
        return "YMODEM";
    case IOT_SIMPLE_OTA_SUCCESS:
        return "SUCCESS";
    case IOT_SIMPLE_OTA_FAIL:
        return "FAIL";
    default:
        return "UNKNOWN";
    }
}

static bool ctr_ota_should_delay_auto_update(void)
{
    int task_status = mqtt_get_task_status();

    if (task_status == 257 || task_status == 1793) {
        ESP_LOGW(TAG,
                 "Defer auto OTA because machine is busy, taskStatus=%d appState=%d",
                 task_status,
                 (int)sp_pro_app_get_state());
        return true;
    }

    return false;
}

static const char *ctr_ota_error_name(OTA_ERROR_TYPE_TYP error)
{
    switch (error) {
    case OTA_ERROR_NONE:
        return "NONE";
    case OTA_ERROR_DOWNLOAD_FAIL:
        return "DOWNLOAD_FAIL";
    case OTA_ERROR_CRC32_ERR:
        return "CRC32_ERR";
    case OTA_ERROR_SERVER_CHECK_FAIL:
        return "SERVER_CHECK_FAIL";
    case OTA_ERROR_MD5_ERR:
        return "MD5_ERR";
    case OTA_ERROR_APPLY_FAIL:
        return "APPLY_FAIL";
    default:
        return "UNKNOWN";
    }
}

static const char *ctr_ota_package_kind_name(ota_package_kind_t kind)
{
    switch (kind) {
    case OTA_PACKAGE_KIND_BUNDLE:
        return "BUNDLE";
    case OTA_PACKAGE_KIND_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static void ctr_ota_log_context(const char *reason)
{
    ESP_LOGI(TAG,
             "%s | state=%s(%d) error=%s(%d) auto=%s taskId=%s msgId=%s file=%s path=%s pendingUrl=%u run=%u ctrVer=%s",
             reason ? reason : "OTA context",
             ctr_ota_state_name((IOT_SIMPLE_OTA_STA_TYP)g_ota_info.ota_sta),
             g_ota_info.ota_sta,
             ctr_ota_error_name((OTA_ERROR_TYPE_TYP)g_ota_info.ota_error_info),
             g_ota_info.ota_error_info,
             g_ota_info.ota_auto_up,
             g_ota_info.ota_tkid,
             g_ota_info.ota_msgid,
             g_ota_info.ota_file_name,
             g_ota_info.ota_file_path,
             g_ota_new_url_pending,
             ctr_ota_run_flag,
             g_device_version.current_ctr_fw_version);
}

static void ctr_ota_reset_package_probe(void)
{
    memset(&g_ota_package_probe, 0, sizeof(g_ota_package_probe));
    g_ota_package_probe.kind = OTA_PACKAGE_KIND_UNKNOWN;
}

static esp_err_t ctr_ota_stage_esp32_from_bundle(void)
{
    char package_md5[33] = "";
    char payload_md5[33] = "";
    esp_err_t err;

    if (g_ota_package_probe.kind != OTA_PACKAGE_KIND_BUNDLE || !g_ota_package_probe.has_esp32) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!g_ota_package_probe.esp32_upgrade_required) {
        ESP_LOGW(TAG,
                 "Skip bundled HMI OTA stage because upgrade plan does not require HMI update");
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "CTR OTA complete, start staging bundled HMI image name=%s size=%lu packageMd5=%s; this may take a while before reboot",
             g_ota_package_probe.esp32_entry.name,
             (unsigned long)g_ota_package_probe.esp32_entry.size,
             g_ota_info.ota_md5);

    err = perform_ota_from_bundle_url(g_ota_info.ota_url,
                                      &g_ota_package_probe.esp32_entry,
                                      g_ota_info.ota_md5,
                                      package_md5,
                                      payload_md5);
    if (err == ESP_OK) {
        g_ota_package_probe.esp32_staged = true;
        g_ota_package_probe.reboot_required = true;
        ESP_LOGI(TAG,
                 "Merged bundle ESP32 payload staged name=%s version=%s payloadMd5=%s",
                 g_ota_package_probe.esp32_entry.name,
                 g_ota_package_probe.esp32_entry.version,
                 payload_md5);
    } else {
        ESP_LOGE(TAG,
                 "Merged bundle ESP32 payload stage failed: %s",
                 esp_err_to_name(err));
    }

    return err;
}

static bool ctr_ota_is_hmi_only_bundle(void)
{
    return g_ota_package_probe.kind == OTA_PACKAGE_KIND_BUNDLE &&
           g_ota_package_probe.esp32_upgrade_required &&
           !g_ota_package_probe.ctr_upgrade_required;
}

const ota_package_probe_result_t *ctr_ota_get_package_probe_result(void)
{
    return &g_ota_package_probe;
}

bool ctr_ota_is_local_http_test_active(void)
{
#if OTA_LOCAL_HTTP_TEST_ENABLE
    return strcmp(g_ota_info.ota_dtype, "LOCAL_TEST") == 0;
#else
    return false;
#endif
}

#if OTA_LOCAL_HTTP_TEST_ENABLE
static void ctr_ota_start_local_http_test(void)
{
    uint8_t state = IOT_SIMPLE_OTA_URL_GOT;
    uint8_t error_none = OTA_ERROR_NONE;

    ESP_LOGW(TAG, "Start local HTTP OTA test: %s", OTA_LOCAL_HTTP_TEST_URL);

    ctr_ota_reset_package_probe();

    ota_params_edit(OTA_INFO_MSGID, OTA_LOCAL_HTTP_TEST_MSG_ID);
    ota_params_edit(OTA_INFO_URL, OTA_LOCAL_HTTP_TEST_URL);
    ota_params_edit(OTA_INFO_MD5, OTA_LOCAL_HTTP_TEST_MD5);
    ota_params_edit(OTA_INFO_FILE_NAME, OTA_LOCAL_HTTP_TEST_FILE_NAME);
    ota_params_edit(OTA_INFO_TKID, OTA_LOCAL_HTTP_TEST_TASK_ID);
    ota_params_edit(OTA_INFO_DTYPE, "LOCAL_TEST");
    ota_params_edit(OTA_INFO_STYPE, "0");
    ota_params_edit(OTA_INFO_FILE_PATH, "");
    ota_params_edit(OTA_INFO_AUTOUP, "1");
    ota_params_edit(OTA_INFO_ERROR_INFO, &error_none);
    ota_params_edit(OTA_INFO_STA, &state);
    ctr_ota_clear_result_ack();
    s_local_http_test_waiting_for_mqtt_bootstrap = false;

    if (wifi_is_runtime_online()) {
        s_local_http_test_waiting_for_wifi = false;
        s_local_http_test_waiting_for_mqtt_bootstrap = true;
        ESP_LOGW(TAG, "Local HTTP OTA test waits for MQTT bootstrap because WIFI is already online");
    } else {
        s_local_http_test_waiting_for_wifi = true;
        ESP_LOGW(TAG, "Local HTTP OTA test is armed and will start after WIFI ALL DONE");
    }
}
#endif

static void ctr_ota_log_controller_gate(const char *reason)
{
    extern MACHINE_STATUS machine_status;

    ESP_LOGI(TAG,
             "%s | ctr_status=%u drink=%u error=0x%08lX waterShort=%u beanBoxAbnormal=%u brewHandleAbnormal=%u grindHandleAbnormal=%u appMode=%d ctrVer=%s",
             reason ? reason : "OTA controller gate",
             (unsigned)machine_status.ctr_status,
             (unsigned)machine_status.drink_making_flg,
             (unsigned long)machine_status.error_code,
             (unsigned)machine_status.water_box_shortage_flag,
             (unsigned)machine_status.beanbox_in_place,
             (unsigned)machine_status.brew_handle_postion_flag,
             (unsigned)machine_status.grind_handle_postion_flag,
             (int)sys_pra.app_mode,
             g_device_version.current_ctr_fw_version);
}

static bool ctr_ota_has_resume_context(void)
{
    return g_ota_info.ota_tkid[0] != '\0' &&
           g_ota_info.ota_msgid[0] != '\0' &&
           g_ota_info.ota_file_name[0] != '\0';
}

static void ctr_ota_set_state(IOT_SIMPLE_OTA_STA_TYP state)
{
    IOT_SIMPLE_OTA_STA_TYP old_state = (IOT_SIMPLE_OTA_STA_TYP)g_ota_info.ota_sta;
    uint8_t state_u8 = (uint8_t)state;
    if (old_state != state) {
        ESP_LOGI(TAG,
                 "OTA state change: %s(%d) -> %s(%d)",
                 ctr_ota_state_name(old_state),
                 old_state,
                 ctr_ota_state_name(state),
                 state);
    }
    ota_params_edit(OTA_INFO_STA, (char *)&state_u8);
}

static void ctr_ota_set_error(OTA_ERROR_TYPE_TYP error)
{
    OTA_ERROR_TYPE_TYP old_error = (OTA_ERROR_TYPE_TYP)g_ota_info.ota_error_info;
    uint8_t error_u8 = (uint8_t)error;
    if (old_error != error) {
        ESP_LOGW(TAG,
                 "OTA error change: %s(%d) -> %s(%d)",
                 ctr_ota_error_name(old_error),
                 old_error,
                 ctr_ota_error_name(error),
                 error);
    }
    ota_params_edit(OTA_INFO_ERROR_INFO, (char *)&error_u8);
}

void ctr_ota_clear_result_ack(void)
{
    ctr_ota_result_ack_received = 0;
}

void ctr_ota_mark_result_ack_received(void)
{
    ctr_ota_result_ack_received = 1;
}

uint8_t ctr_ota_is_result_ack_received(void)
{
    return ctr_ota_result_ack_received;
}

static bool ctr_ota_enter_ymodem_mode(void)
{
    ctr_ota_log_controller_gate("OTA bootloader request");
    ESP_LOGI(TAG, "Send controller OTA boot command: 123@OTA@NULL#123");
    if (!ctr_cmd_action(CTRL_ACT_OTA_BOOT, NULL)) {
        ESP_LOGE(TAG, "Failed to send OTA boot command to controller");
        return false;
    }

    ESP_LOGI(TAG, "Controller OTA boot command sent, wait for bootloader YMODEM handshake");
    ctr_ota_set_state(IOT_SIMPLE_OTA_YMODEM);
    return true;
}
static void ctr_ota_publish_failure_result(void)
{
    if (g_ota_info.ota_error_info == OTA_ERROR_DOWNLOAD_FAIL) {
        publish_ota_result_to_mqtt(OTA_RESULT_DOWNLOAD_ERR, "Download_Fail");
    } else if (g_ota_info.ota_error_info == OTA_ERROR_CRC32_ERR) {
        publish_ota_result_to_mqtt(OTA_RESULT_VERIFY_ERR, "CRC32_ERR");
    } else if (g_ota_info.ota_error_info == OTA_ERROR_SERVER_CHECK_FAIL) {
        publish_ota_result_to_mqtt(OTA_RESULT_FAILED, "Server_Check_Fail");
    } else if (g_ota_info.ota_error_info == OTA_ERROR_MD5_ERR) {
        publish_ota_result_to_mqtt(OTA_RESULT_VERIFY_ERR, "MD5_ERR");
    } else if (g_ota_info.ota_error_info == OTA_ERROR_APPLY_FAIL) {
        publish_ota_result_to_mqtt(OTA_RESULT_BURN_ERR, "Apply_Fail");
    } else {
        publish_ota_result_to_mqtt(OTA_RESULT_FAILED, "Update_Fail");
    }
}

esp_err_t ota_params_reset(void)
{
    nvs_handle_t nvs_handle;
    static const char *const keys[] = {
        "ota_url",
        "ota_md5",
        "ota_file_name",
        "ota_dtype",
        "ota_stype",
        "ota_tkid",
        "ota_file_path",
        "ota_autoUp",
        "ota_msgid",
        "ota_sta",
        "ota_error_info",
    };
    esp_err_t err = nvs_open(NVS_CTR_OTA, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_write_reset_context_to_nvs(nvs_handle, NULL);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "ota reset context",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   ota_write_reset_context_to_nvs,
                                                   NULL);
    }
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    memset(&g_ota_info,0,sizeof(g_ota_info));
    g_ota_new_url_pending = 0;
    ctr_ota_clear_result_ack();
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Reset Ota Pra ok!");
    ctr_ota_log_context("OTA reset context");

    return err;
}
esp_err_t ota_params_edit(OTA_INFOIDX_TYP idx, const void *value)
{
    ota_info_t *info = &g_ota_info;
    const char *str_value = (const char *)value;
    ota_nvs_write_ctx_t write_ctx = {
        .idx = idx,
        .info = info,
        .str_value = str_value,
    };
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_CTR_OTA, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    switch (idx) {
        case OTA_INFO_URL:
            strncpy(info->ota_url, str_value, sizeof(info->ota_url) - 1);
            info->ota_url[sizeof(info->ota_url) - 1] = '\0';
            break;
        case OTA_INFO_MD5:
            strncpy(info->ota_md5, str_value, sizeof(info->ota_md5) - 1);
            info->ota_md5[sizeof(info->ota_md5) - 1] = '\0';
            break;
        case OTA_INFO_FILE_NAME:
            strncpy(info->ota_file_name, str_value, sizeof(info->ota_file_name) - 1);
            info->ota_file_name[sizeof(info->ota_file_name) - 1] = '\0';
            break;
        case OTA_INFO_DTYPE:
            strncpy(info->ota_dtype, str_value, sizeof(info->ota_dtype) - 1);
            info->ota_dtype[sizeof(info->ota_dtype) - 1] = '\0';
            break;
        case OTA_INFO_STYPE:
            strncpy(info->ota_stype, str_value, sizeof(info->ota_stype) - 1);
            info->ota_stype[sizeof(info->ota_stype) - 1] = '\0';
            break;
        case OTA_INFO_TKID:
            strncpy(info->ota_tkid, str_value, sizeof(info->ota_tkid) - 1);
            info->ota_tkid[sizeof(info->ota_tkid) - 1] = '\0';
            break;
        case OTA_INFO_FILE_PATH:
            strncpy(info->ota_file_path, str_value, sizeof(info->ota_file_path) - 1);
            info->ota_file_path[sizeof(info->ota_file_path) - 1] = '\0';
            break;
        case OTA_INFO_AUTOUP:
            strncpy(info->ota_auto_up, str_value, sizeof(info->ota_auto_up) - 1);
            info->ota_auto_up[sizeof(info->ota_auto_up) - 1] = '\0';
            break;
        case OTA_INFO_MSGID:
            strncpy(info->ota_msgid, str_value, sizeof(info->ota_msgid) - 1);
            info->ota_msgid[sizeof(info->ota_msgid) - 1] = '\0';
            break;
        case OTA_INFO_STA:
            info->ota_sta = *(const uint8_t *)value;
            break;
        case OTA_INFO_ERROR_INFO:
            info->ota_error_info = *(const uint8_t *)value;
            break;
    }

    err = ota_write_single_param_to_nvs(nvs_handle, &write_ctx);

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        const char *const keys[] = {
            ota_nvs_name_arr[idx],
        };
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "ota context field",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   ota_write_single_param_to_nvs,
                                                   &write_ctx);
    }

    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (idx == OTA_INFO_STA)
    {
        ESP_LOGI(TAG, "Write Ota Pra:%s:%d", ota_nvs_name_arr[idx], info->ota_sta);
    }
    else if (idx == OTA_INFO_ERROR_INFO)
    {
        ESP_LOGI(TAG, "Write Ota Pra:%s:%d", ota_nvs_name_arr[idx], info->ota_error_info);
    }
    else
    {
        ESP_LOGI(TAG, "Write Ota Pra:%s:%s", ota_nvs_name_arr[idx], str_value);
    }

    return err;
}

static esp_err_t ota_params_load(ota_info_t *config)
{
    nvs_handle_t nvs_handle;
    size_t len=0;

    esp_err_t err = nvs_open(NVS_CTR_OTA, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(nvs_handle, "ota_sta",(uint8_t*)&config->ota_sta);
    if (err != ESP_OK) {
        config->ota_sta = 0;
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%d","ota_sta",config->ota_sta);

    len = sizeof(config->ota_url);
    err = nvs_get_str(nvs_handle, "ota_url", config->ota_url, &len);
    if (err != ESP_OK) {
        config->ota_url[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_url",config->ota_url);

    len = sizeof(config->ota_md5);
    err = nvs_get_str(nvs_handle, "ota_md5", config->ota_md5, &len);
    if (err != ESP_OK) {
        config->ota_md5[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_md5",config->ota_md5);

    len = sizeof(config->ota_file_name);
    err = nvs_get_str(nvs_handle, "ota_file_name", config->ota_file_name, &len);
    if (err != ESP_OK) {
        config->ota_file_name[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_file_name",config->ota_file_name);

    len = sizeof(config->ota_dtype);
    err = nvs_get_str(nvs_handle, "ota_dtype", config->ota_dtype, &len);
    if (err != ESP_OK) {
        config->ota_dtype[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_dtype",config->ota_dtype);

    len = sizeof(config->ota_stype);
    err = nvs_get_str(nvs_handle, "ota_stype", config->ota_stype, &len);
    if (err != ESP_OK) {
        config->ota_stype[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_stype",config->ota_stype);

    len = sizeof(config->ota_tkid);
    err = nvs_get_str(nvs_handle, "ota_tkid", config->ota_tkid, &len);
    if (err != ESP_OK) {
        config->ota_tkid[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_tkid",config->ota_tkid);

    len = sizeof(config->ota_file_path);
    err = nvs_get_str(nvs_handle, "ota_file_path", config->ota_file_path, &len);
    if (err != ESP_OK) {
        config->ota_file_path[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_file_path",config->ota_file_path);

    len = sizeof(config->ota_auto_up);
    err = nvs_get_str(nvs_handle, "ota_autoUp", config->ota_auto_up, &len);
    if (err != ESP_OK) {
        config->ota_auto_up[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_autoUp",config->ota_auto_up);

    len = sizeof(config->ota_msgid);
    err = nvs_get_str(nvs_handle, "ota_msgid", config->ota_msgid, &len);
    if (err != ESP_OK) {
        config->ota_msgid[0] = '\0';
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%s","ota_msgid",config->ota_msgid);

    err = nvs_get_u8(nvs_handle, "ota_error_info", (uint8_t*)&config->ota_error_info);
    if (err != ESP_OK) {
        config->ota_error_info = OTA_ERROR_NONE;
    }
    ESP_LOGI(TAG, "Read OTA Pra:%s:%d","ota_error_info",config->ota_error_info);

    nvs_close(nvs_handle);
    return ESP_OK;
}

const esp_partition_t *get_unused_ota_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *configured = esp_ota_get_boot_partition();

    if (configured != running) {
        return configured;
    }

    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);

    if (ota0 != running) {
        ESP_LOGI(TAG, "get_unused_ota_partition %p",ota0);
        return ota0;
    } else {
        ESP_LOGI(TAG, "get_unused_ota_partition %p",ota1);
        return ota1;
    }
}

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "mbedtls/md5.h"
#include "esp_crt_bundle.h"

#define TAG "HTTP_DOWNLOAD"
#define CHUNK_SIZE 1024  // Download buffer size in bytes.

static void ctr_ota_log_download_preview(const char *buffer, size_t len)
{
    char text_preview[129] = {0};
    char hex_preview[3 * 32 + 1] = {0};
    size_t text_len = len < (sizeof(text_preview) - 1U) ? len : (sizeof(text_preview) - 1U);
    size_t hex_len = len < 32U ? len : 32U;
    size_t hex_offset = 0;

    for (size_t idx = 0; idx < text_len; ++idx) {
        unsigned char ch = (unsigned char)buffer[idx];
        text_preview[idx] = (ch >= 32U && ch <= 126U) ? (char)ch : '.';
    }

    for (size_t idx = 0; idx < hex_len && hex_offset < (sizeof(hex_preview) - 4U); ++idx) {
        hex_offset += snprintf(&hex_preview[hex_offset],
                               sizeof(hex_preview) - hex_offset,
                               "%02X ",
                               (unsigned char)buffer[idx]);
    }

    ESP_LOGW(TAG, "Download preview text: %s", text_preview);
    ESP_LOGW(TAG, "Download preview hex: %s", hex_preview);
}

static bool ctr_ota_payload_looks_like_error_page(const char *buffer, size_t len)
{
    char preview[129] = {0};
    size_t preview_len = len < (sizeof(preview) - 1U) ? len : (sizeof(preview) - 1U);

    if (buffer == NULL || len == 0U) {
        return false;
    }

    memcpy(preview, buffer, preview_len);
    for (size_t idx = 0; idx < preview_len; ++idx) {
        if (preview[idx] == '\r' || preview[idx] == '\n' || preview[idx] == '\t') {
            preview[idx] = ' ';
        }
    }

    return strstr(preview, "<?xml") != NULL ||
           strstr(preview, "<Error>") != NULL ||
           strstr(preview, "AccessDenied") != NULL;
}
static void ctr_ota_format_md5(char *md5_str, size_t md5_str_size, const unsigned char digest[16])
{
    static const char hex[] = "0123456789abcdef";

    if (md5_str == NULL || md5_str_size < 33) {
        return;
    }

    for (size_t idx = 0; idx < 16; ++idx) {
        md5_str[idx * 2] = hex[(digest[idx] >> 4) & 0x0F];
        md5_str[idx * 2 + 1] = hex[digest[idx] & 0x0F];
    }
    md5_str[32] = '\0';
}

static bool ctr_ota_md5_matches(const char *actual_md5, const char *expected_md5)
{
    if (expected_md5 == NULL || expected_md5[0] == '\0') {
        return true;
    }

    return strcasecmp(actual_md5, expected_md5) == 0;
}

static bool ctr_ota_ready_to_reboot_after_success(TickType_t now, TickType_t *wait_tick)
{
    if (!g_ota_package_probe.reboot_required) {
        return true;
    }

    if (!voice_play_is_busy()) {
        return true;
    }

    if (wait_tick && *wait_tick == 0) {
        *wait_tick = now;
        ESP_LOGI(TAG, "Delay OTA reboot until success voice playback finishes");
        return false;
    }

    if (wait_tick &&
        (now - *wait_tick) >= pdMS_TO_TICKS(CTR_OTA_SUCCESS_VOICE_WAIT_MS)) {
        ESP_LOGW(TAG,
                 "OTA success voice wait timeout after %u ms, continue reboot",
                 (unsigned)CTR_OTA_SUCCESS_VOICE_WAIT_MS);
        return true;
    }

    return false;
}

static esp_err_t download_bundle_ctr_entry_to_partition_and_get_md5(const char *url,
                                                                    const ota_bundle_entry_t *entry,
                                                                    char *package_md5_str,
                                                                    char *payload_md5_str)
{
    esp_err_t err = ESP_OK;
    mbedtls_md5_context package_md5_ctx;
    mbedtls_md5_context payload_md5_ctx;
    unsigned char package_md5_digest[16] = {0};
    unsigned char payload_md5_digest[16] = {0};
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET
    };
    esp_http_client_handle_t client = NULL;
    const esp_partition_t *partition = NULL;
    char buffer[CHUNK_SIZE];
    size_t absolute_offset = 0;
    size_t written = 0;
    uint8_t temp_u8 = 0;

    if (url == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (package_md5_str != NULL) {
        package_md5_str[0] = '\0';
    }
    if (payload_md5_str != NULL) {
        payload_md5_str[0] = '\0';
    }

    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for bundle payload download");
        return ESP_FAIL;
    }

    mbedtls_md5_init(&package_md5_ctx);
    mbedtls_md5_init(&payload_md5_ctx);
    mbedtls_md5_starts(&package_md5_ctx);
    mbedtls_md5_starts(&payload_md5_ctx);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for bundle payload: %s", esp_err_to_name(err));
        goto cleanup;
    }

    {
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);

        if (status_code != 200) {
            ESP_LOGE(TAG, "Unexpected HTTP status for bundle payload: %d", status_code);
            err = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }

        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed for bundle payload: %d", content_length);
            err = ESP_FAIL;
            goto cleanup;
        }

        if ((uint32_t)content_length != g_ota_package_probe.content_length) {
            ESP_LOGE(TAG,
                     "Bundle payload download length mismatch: probe=%lu actual=%d",
                     (unsigned long)g_ota_package_probe.content_length,
                     content_length);
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

    partition = get_unused_ota_partition();
    if (partition == NULL) {
        ESP_LOGE(TAG, "No unused OTA partition found for bundle payload");
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (entry->size >= partition->size) {
        ESP_LOGE(TAG,
                 "Bundle CTR payload too large payload=%lu partition=%lu",
                 (unsigned long)entry->size,
                 (unsigned long)partition->size);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase partition for bundle payload: %s", esp_err_to_name(err));
        goto cleanup;
    }

    while (1) {
        int data_read = esp_http_client_read(client, buffer, CHUNK_SIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Bundle payload read failed");
            err = ESP_FAIL;
            goto cleanup;
        }

        if (data_read == 0) {
            break;
        }

        mbedtls_md5_update(&package_md5_ctx,
                           (const unsigned char *)buffer,
                           (size_t)data_read);

        {
            size_t chunk_begin = absolute_offset;
            size_t chunk_end = absolute_offset + (size_t)data_read;
            size_t payload_begin = entry->offset;
            size_t payload_end = entry->offset + entry->size;

            if (chunk_begin < payload_end && chunk_end > payload_begin) {
                size_t slice_begin = chunk_begin > payload_begin ? chunk_begin : payload_begin;
                size_t slice_end = chunk_end < payload_end ? chunk_end : payload_end;
                size_t slice_offset = slice_begin - chunk_begin;
                size_t slice_len = slice_end - slice_begin;

                err = esp_partition_write(partition, written, buffer + slice_offset, slice_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to write bundle CTR payload: %s", esp_err_to_name(err));
                    goto cleanup;
                }

                mbedtls_md5_update(&payload_md5_ctx, (const unsigned char *)(buffer + slice_offset), slice_len);
                written += slice_len;

                if (entry->size != 0U) {
                    uint8_t progress = (uint8_t)((written * 100U) / entry->size);
                    if (progress != temp_u8 && (progress % 5U) == 0U) {
                        temp_u8 = progress;
                        ESP_LOGI(TAG, "Down CTR payload %u %%", temp_u8);
                    }
                }
            }
        }

        absolute_offset += (size_t)data_read;
    }

    if (written != entry->size) {
        ESP_LOGE(TAG,
                 "Bundle CTR payload size mismatch: written=%u expected=%lu",
                 (unsigned)written,
                 (unsigned long)entry->size);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    mbedtls_md5_finish(&package_md5_ctx, package_md5_digest);
    mbedtls_md5_finish(&payload_md5_ctx, payload_md5_digest);
    ctr_ota_format_md5(package_md5_str, 33, package_md5_digest);
    ctr_ota_format_md5(payload_md5_str, 33, payload_md5_digest);

    {
        char temp[200] = "";
        sprintf(temp, "%08X", (unsigned int)written);
        ota_params_edit(OTA_INFO_FILE_PATH, temp);
    }

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    mbedtls_md5_free(&package_md5_ctx);
    mbedtls_md5_free(&payload_md5_ctx);
    return err;
}

static void ctr_ota_log_bundle_probe_result(const ota_bundle_header_t *header)
{
    const ota_bundle_entry_t *ctr_entry = ota_bundle_find_entry(header, OTA_BUNDLE_PAYLOAD_CTR);
    const ota_bundle_entry_t *esp_entry = ota_bundle_find_entry(header, OTA_BUNDLE_PAYLOAD_ESP32);

    ESP_LOGI(TAG,
             "Merged OTA bundle detected: packageSize=%lu hasCtr=%d hasEsp32=%d packageVersion=%s",
             (unsigned long)header->package_size,
             ctr_entry != NULL,
             esp_entry != NULL,
             header->package_version);

    if (ctr_entry != NULL) {
        ESP_LOGI(TAG,
                 "Bundle CTR entry: name=%s version=%s offset=%lu size=%lu",
                 ctr_entry->name,
                 ctr_entry->version,
                 (unsigned long)ctr_entry->offset,
                 (unsigned long)ctr_entry->size);
    }

    if (esp_entry != NULL) {
        ESP_LOGI(TAG,
                 "Bundle ESP32 entry: name=%s version=%s offset=%lu size=%lu",
                 esp_entry->name,
                 esp_entry->version,
                 (unsigned long)esp_entry->offset,
                 (unsigned long)esp_entry->size);
    }
}

static bool ctr_ota_has_persisted_report_context(void)
{
    return g_ota_info.ota_file_name[0] != '\0' ||
           g_ota_info.ota_tkid[0] != '\0' ||
           g_ota_info.ota_msgid[0] != '\0' ||
           g_ota_info.ota_md5[0] != '\0';
}

static int ctr_ota_parse_next_version_token(const char **cursor, unsigned long *value)
{
    const char *p;
    char *endptr = NULL;

    if (cursor == NULL || *cursor == NULL || value == NULL) {
        return -1;
    }

    p = *cursor;
    while (*p != '\0' && !isdigit((unsigned char)*p)) {
        ++p;
    }

    if (*p == '\0') {
        *cursor = p;
        return 0;
    }

    *value = strtoul(p, &endptr, 10);
    if (endptr == p) {
        *cursor = p + 1;
        return -1;
    }

    *cursor = endptr;
    return 1;
}

static int ctr_ota_compare_version_text(const char *incoming, const char *current)
{
    const char *left = incoming;
    const char *right = current;

    while (1) {
        unsigned long left_value = 0;
        unsigned long right_value = 0;
        int left_status = ctr_ota_parse_next_version_token(&left, &left_value);
        int right_status = ctr_ota_parse_next_version_token(&right, &right_value);

        if (left_status <= 0 && right_status <= 0) {
            return 0;
        }

        if (left_status <= 0) {
            left_value = 0;
        }
        if (right_status <= 0) {
            right_value = 0;
        }

        if (left_value > right_value) {
            return 1;
        }
        if (left_value < right_value) {
            return -1;
        }
    }
}

static bool ctr_ota_is_version_not_newer(const char *component,
                                         const char *incoming_version,
                                         const char *current_version)
{
    int cmp;

    if (incoming_version == NULL || incoming_version[0] == '\0') {
        ESP_LOGI(TAG,
                 "Skip version gate for %s OTA because incoming version is empty",
                 component ? component : "unknown");
        return false;
    }

    if (current_version == NULL || current_version[0] == '\0') {
        ESP_LOGI(TAG,
                 "Skip version gate for %s OTA because current version is empty",
                 component ? component : "unknown");
        return false;
    }

    cmp = ctr_ota_compare_version_text(incoming_version, current_version);
    ESP_LOGI(TAG,
             "OTA version compare component=%s incoming=%s current=%s cmp=%d",
             component ? component : "unknown",
             incoming_version,
             current_version,
             cmp);

    return cmp <= 0;
}

static bool ctr_ota_should_skip_ctr_upgrade(const char *incoming_version,
                                            const char *current_version)
{
    int cmp;

    if (incoming_version == NULL || incoming_version[0] == '\0') {
        ESP_LOGI(TAG, "Skip CTR version gate because incoming version is empty");
        return false;
    }

    if (current_version == NULL || current_version[0] == '\0') {
        ESP_LOGI(TAG, "Skip CTR version gate because current version is empty");
        return false;
    }

    cmp = ctr_ota_compare_version_text(incoming_version, current_version);
    ESP_LOGI(TAG,
             "OTA version compare component=CTR incoming=%s current=%s cmp=%d mode=%s",
             incoming_version,
             current_version,
             cmp,
#if OTA_TEST_ALLOW_CTR_UPGRADE_WHEN_VERSION_DIFF
             "diff-allows-upgrade");
#else
             "strict-newer-only");
#endif

#if OTA_TEST_ALLOW_CTR_UPGRADE_WHEN_VERSION_DIFF
    return cmp == 0;
#else
    return cmp <= 0;
#endif
}

static esp_err_t ctr_ota_refresh_bundle_upgrade_plan(void)
{
    char current_hmi_version[16] = "";

    g_ota_package_probe.ctr_upgrade_required = false;
    g_ota_package_probe.esp32_upgrade_required = false;

    if (g_ota_package_probe.kind != OTA_PACKAGE_KIND_BUNDLE) {
        return ESP_OK;
    }

    if (!g_ota_package_probe.has_esp32) {
        ESP_LOGE(TAG, "Reject OTA bundle because HMI payload is missing and HMI upgrade is mandatory");
        return ESP_ERR_NOT_SUPPORTED;
    }

    mqtt_get_hmi_version_from_app(current_hmi_version, sizeof(current_hmi_version));
    if (ctr_ota_is_version_not_newer("HMI",
                                     g_ota_package_probe.esp32_entry.version,
                                     current_hmi_version)) {
        ESP_LOGW(TAG,
                 "Bundle HMI version is not newer, stop OTA directly: incoming=%s current=%s",
                 g_ota_package_probe.esp32_entry.version,
                 current_hmi_version);
        return ESP_OK;
    }

    g_ota_package_probe.esp32_upgrade_required = true;

    if (!g_ota_package_probe.has_ctr) {
        ESP_LOGE(TAG, "Reject OTA bundle because CTR payload is missing and bundle must contain CTR + HMI");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (ctr_ota_should_skip_ctr_upgrade(g_ota_package_probe.ctr_entry.version,
                                        g_device_version.current_ctr_fw_version)) {
        ESP_LOGI(TAG,
                 "Bundle upgrade plan: HMI only because CTR version does not require upgrade incoming=%s current=%s",
                 g_ota_package_probe.ctr_entry.version,
                 g_device_version.current_ctr_fw_version);
        return ESP_OK;
    }

    g_ota_package_probe.ctr_upgrade_required = true;
    ESP_LOGI(TAG,
             "Bundle upgrade plan: CTR + HMI incomingCtr=%s currentCtr=%s incomingHmi=%s currentHmi=%s",
             g_ota_package_probe.ctr_entry.version,
             g_device_version.current_ctr_fw_version,
             g_ota_package_probe.esp32_entry.version,
             current_hmi_version);
    return ESP_OK;
}

static esp_err_t ctr_ota_probe_package_from_url(const char *url)
{
    esp_err_t err = ESP_OK;
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET
    };
    esp_http_client_handle_t client = NULL;
    ota_bundle_header_t *header = NULL;
    size_t total_read = 0;
    uint32_t magic = 0;

    ctr_ota_reset_package_probe();

    header = calloc(1, sizeof(*header));
    if (header == NULL) {
        ESP_LOGE(TAG, "Allocate merged OTA bundle header buffer failed");
        return ESP_ERR_NO_MEM;
    }

    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for package probe");
        err = ESP_FAIL;
        goto cleanup;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP probe connection: %s", esp_err_to_name(err));
        goto cleanup;
    }

    g_ota_package_probe.content_length = (uint32_t)esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status during package probe: %d",
                 esp_http_client_get_status_code(client));
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    while (total_read < sizeof(*header)) {
        int chunk = esp_http_client_read(client,
                                         ((char *)header) + total_read,
                                         sizeof(*header) - total_read);
        if (chunk < 0) {
            ESP_LOGE(TAG, "HTTP probe read failed");
            err = ESP_FAIL;
            goto cleanup;
        }
        if (chunk == 0) {
            break;
        }
        total_read += (size_t)chunk;
    }

    if (total_read < sizeof(magic)) {
        ESP_LOGE(TAG, "Reject legacy OTA package probe because bundle with HMI payload is required");
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    memcpy(&magic, header, sizeof(magic));
    if (magic != OTA_BUNDLE_MAGIC) {
        ESP_LOGE(TAG, "Reject legacy OTA package because bundle with HMI payload is required");
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    if (total_read < sizeof(*header)) {
        ESP_LOGE(TAG,
                 "Merged OTA bundle header incomplete: got=%u need=%u",
                 (unsigned)total_read,
                 (unsigned)sizeof(*header));
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = ota_bundle_validate_header(header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Merged OTA bundle header parse failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    if (g_ota_package_probe.content_length != 0U &&
        header->package_size != g_ota_package_probe.content_length) {
        ESP_LOGE(TAG,
                 "Merged OTA bundle size mismatch: header=%lu http=%lu",
                 (unsigned long)header->package_size,
                 (unsigned long)g_ota_package_probe.content_length);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    g_ota_package_probe.kind = OTA_PACKAGE_KIND_BUNDLE;
    g_ota_package_probe.has_ctr = false;
    g_ota_package_probe.has_esp32 = false;

    {
        const ota_bundle_entry_t *entry = ota_bundle_find_entry(header, OTA_BUNDLE_PAYLOAD_CTR);
        if (entry != NULL) {
            g_ota_package_probe.has_ctr = true;
            memcpy(&g_ota_package_probe.ctr_entry, entry, sizeof(*entry));
        }
    }

    {
        const ota_bundle_entry_t *entry = ota_bundle_find_entry(header, OTA_BUNDLE_PAYLOAD_ESP32);
        if (entry != NULL) {
            g_ota_package_probe.has_esp32 = true;
            memcpy(&g_ota_package_probe.esp32_entry, entry, sizeof(*entry));
        }
    }

    if (!g_ota_package_probe.has_ctr && !g_ota_package_probe.has_esp32) {
        ESP_LOGE(TAG, "Merged OTA bundle contains no supported payload entries");
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    ctr_ota_log_bundle_probe_result(header);
    err = ctr_ota_refresh_bundle_upgrade_plan();
    if (err != ESP_OK) {
        goto cleanup;
    }

cleanup:
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "OTA package probe failed, kind=%s err=%s",
                 ctr_ota_package_kind_name(g_ota_package_probe.kind),
                 esp_err_to_name(err));
        ctr_ota_reset_package_probe();
    }

    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (header != NULL) {
        free(header);
    }

    return err;
}

int app_file_check(const esp_partition_t *partition)
{
    uint16_t crc = 0xFFFF;
    uint8_t data_arr[1024] = {0};

    esp_err_t err = esp_partition_read(partition, 0, &app_head, sizeof(app_head));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read APP header! Error: %d", err);
        return 0;
    }

    uint32_t length = app_head.app_size;
    uint32_t package_size = CTR_OTA_PACKAGE_HEADER_LEN + length;
    uint16_t expected_crc = (uint16_t)(app_head.crc16 & 0xFFFFU);

    if (package_size > partition->size) {
        ESP_LOGE(TAG, "Package size %lu exceeds partition size %lu", package_size, partition->size);
        return 0;
    }

    ESP_LOGI(TAG,
             "Starting CRC16 check, payload=%lu bytes, header=%u bytes, expected=0x%04X",
             length,
             CTR_OTA_PACKAGE_HEADER_LEN,
             expected_crc);

    uint32_t offset = 0;
    while (offset < length) {
        uint32_t pack_len = sizeof(data_arr);
        if (offset + pack_len > length) {
            pack_len = length - offset;
        }

        err = esp_partition_read(partition, CTR_OTA_PACKAGE_HEADER_LEN + offset, &data_arr, pack_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read APP data for CRC calculation at offset %lu! Error: %d", offset, err);
            return 0;
        }

        for (uint32_t idx = 0; idx < pack_len; ++idx) {
            crc ^= data_arr[idx];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                if (crc & 1U) {
                    crc = (uint16_t)((crc >> 1) ^ 0xA001U);
                } else {
                    crc >>= 1;
                }
            }
        }

        offset += pack_len;
        if (offset < length) {
            vTaskDelay(1);
        }
    }

    if (crc == expected_crc) {
        ESP_LOGI(TAG, "CRC16 check passed!");
        return 1;
    }

    ESP_LOGE(TAG, "CRC16 check failed! Calculated: 0x%04X, Expected: 0x%04X", crc, expected_crc);
    return 0;
}

static bool ctr_ota_switch_to_pending_url(void)
{
    if (!g_ota_new_url_pending) {
        return false;
    }

    g_ota_new_url_pending = 0;
    ctr_ota_reset_package_probe();
    ctr_ota_set_error(OTA_ERROR_NONE);
    ctr_ota_set_state(IOT_SIMPLE_OTA_URL_GOT);
    ESP_LOGW(TAG, "Switch OTA flow to pending new URL");
    ctr_ota_log_context("OTA pending URL switch");
    return true;
}

static void ctr_ota_finish_task(void)
{
    ctr_ota_log_context("OTA finish task before reset");
    ctr_ota_reset_package_probe();
    ota_params_reset();

    taskENTER_CRITICAL(&ctr_ota_task_lock);
    ctr_ota_run_flag = 0;
    taskEXIT_CRITICAL(&ctr_ota_task_lock);

    ctr_ota_log_context("OTA finish task after reset");
    ESP_LOGW(TAG, "Ctr_OTA thread exit");
    vTaskDelete(NULL);
}

void ctr_ota_task(void *pvParameters)
{
    uint8_t temp = 0;
    char bundle_md5[33] = "";
    char payload_md5[33] = "";
    char expected_payload_md5[33] = "";
    uint8_t error_info = OTA_ERROR_NONE;
    IOT_SIMPLE_OTA_STA_TYP last_state = IOT_SIMPEL_OTA_NULL;
    TickType_t state_enter_tick = 0;
        TickType_t result_last_publish_tick = 0;
    bool ymodem_transfer_done = false;
    TickType_t ymodem_complete_tick = 0;
    TickType_t ymodem_wait_log_tick = 0;
    TickType_t success_voice_wait_tick = 0;
    char ctr_version_before_ota[sizeof(g_device_version.current_ctr_fw_version)] = {0};

    (void)pvParameters;

    while (1)
    {
        TickType_t now = xTaskGetTickCount();

        if (g_ota_info.ota_sta != last_state) {
            last_state = g_ota_info.ota_sta;
            state_enter_tick = now;
            result_last_publish_tick = 0;
            success_voice_wait_tick = 0;

                        if (g_ota_info.ota_sta != IOT_SIMPLE_OTA_YMODEM) {
                ymodem_transfer_done = false;
                ymodem_complete_tick = 0;
                ymodem_wait_log_tick = 0;
                ctr_version_before_ota[0] = '\0';
            }

            ctr_ota_log_context("OTA state loop entered");
        }

        if (sys_pra.wifi_state == WIFI_DISCONNECTED &&
            (g_ota_info.ota_sta < IOT_SIMPLE_OTA_YMODEM))
        {
            vTaskDelay(100);
            continue;
        }

        switch (g_ota_info.ota_sta)
        {
            case IOT_SIMPLE_OTA_URL_GOT:
            {
                if (ctr_ota_switch_to_pending_url()) {
                    break;
                }

                ESP_LOGI(TAG, "OTA start download taskId=%s auto=%s msgId=%s", g_ota_info.ota_tkid, g_ota_info.ota_auto_up, g_ota_info.ota_msgid);
                {
                    esp_err_t probe_err = ctr_ota_probe_package_from_url(g_ota_info.ota_url);
                    if (probe_err != ESP_OK) {
                        temp = IOT_SIMPLE_OTA_FAIL;
                        error_info = OTA_ERROR_DOWNLOAD_FAIL;
                        break;
                    }

                    if (g_ota_package_probe.kind == OTA_PACKAGE_KIND_BUNDLE) {
                        if (!g_ota_package_probe.esp32_upgrade_required) {
                            char current_hmi_version[16] = "";
                            mqtt_get_hmi_version_from_app(current_hmi_version, sizeof(current_hmi_version));
                            ESP_LOGW(TAG,
                                     "Skip OTA silently because HMI version is not newer: incoming=%s current=%s",
                                     g_ota_package_probe.esp32_entry.version,
                                     current_hmi_version);
                            ESP_LOGI(TAG, "OTA no-op: keep softwareUpdateInfoAck only, do not publish softwareUpdateResult");
                            ctr_ota_finish_task();
                            break;
                        }

                        if (!g_ota_package_probe.ctr_upgrade_required) {
                            esp_err_t err = ctr_ota_stage_esp32_from_bundle();

                            if (err == ESP_OK) {
                                publish_ota_result_to_mqtt(OTA_RESULT_DOWNLOAD_OK, "Download Success");
                                publish_ctr_ota_check_status_to_mqtt();
                                if (g_ota_info.ota_auto_up[0] == '1') {
                                    temp = IOT_SIMPLE_OTA_SUCCESS;
                                    error_info = OTA_ERROR_NONE;
                                } else {
                                    ESP_LOGI(TAG,
                                             "Merged HMI-only OTA package downloaded and staged, wait local/UI confirm taskId=%s",
                                             g_ota_info.ota_tkid);
                                    temp = IOT_SIMPLE_OTA_PACK_CHECK_END;
                                    error_info = OTA_ERROR_NONE;
                                }
                            } else {
                                temp = IOT_SIMPLE_OTA_FAIL;
                                error_info = OTA_ERROR_APPLY_FAIL;
                            }
                            ctr_ota_set_state((IOT_SIMPLE_OTA_STA_TYP)temp);
                            ctr_ota_set_error((OTA_ERROR_TYPE_TYP)error_info);
                            break;
                        }

                        ctr_ota_format_md5(expected_payload_md5,
                                           sizeof(expected_payload_md5),
                                           g_ota_package_probe.ctr_entry.md5);

                        {
                            esp_err_t err = download_bundle_ctr_entry_to_partition_and_get_md5(
                                g_ota_info.ota_url,
                                &g_ota_package_probe.ctr_entry,
                                bundle_md5,
                                payload_md5);

                            ESP_LOGI(TAG,
                                     "download_bundle_ctr_payload %d packageMd5=%s payloadMd5=%s expectedPackageMd5=%s expectedPayloadMd5=%s",
                                     err,
                                     bundle_md5,
                                     payload_md5,
                                     g_ota_info.ota_md5,
                                     expected_payload_md5);

                            if (err == ESP_OK) {
                                if (!ctr_ota_md5_matches(bundle_md5, g_ota_info.ota_md5)) {
                                    ESP_LOGE(TAG,
                                             "Merged bundle package md5 mismatch actual=%s expected=%s",
                                             bundle_md5,
                                             g_ota_info.ota_md5);
                                    temp = IOT_SIMPLE_OTA_FAIL;
                                    error_info = OTA_ERROR_MD5_ERR;
                                } else if (!ctr_ota_md5_matches(payload_md5, expected_payload_md5)) {
                                    ESP_LOGE(TAG,
                                             "Merged bundle CTR payload md5 mismatch actual=%s expected=%s",
                                             payload_md5,
                                             expected_payload_md5);
                                    temp = IOT_SIMPLE_OTA_FAIL;
                                    error_info = OTA_ERROR_MD5_ERR;
                                } else {
                                    publish_ota_result_to_mqtt(OTA_RESULT_DOWNLOAD_OK, "Download Success");
                                    if (app_file_check(get_unused_ota_partition())) {
                                        temp = IOT_SIMPLE_OTA_CRC_OK;
                                        error_info = OTA_ERROR_NONE;
                                    } else {
                                        temp = IOT_SIMPLE_OTA_FAIL;
                                        error_info = OTA_ERROR_CRC32_ERR;
                                    }
                                }
                            } else {
                                temp = IOT_SIMPLE_OTA_URL_AUTH_FAIL;
                                error_info = OTA_ERROR_DOWNLOAD_FAIL;
                            }
                        }

                        if (g_ota_new_url_pending) {
                            g_ota_new_url_pending = 0;
                            break;
                        }

                        ctr_ota_set_state((IOT_SIMPLE_OTA_STA_TYP)temp);
                        ctr_ota_set_error((OTA_ERROR_TYPE_TYP)error_info);
                        break;
                    }
                    ESP_LOGE(TAG,
                             "Reject non-bundle OTA flow in URL_GOT, kind=%s",
                             ctr_ota_package_kind_name(g_ota_package_probe.kind));
                    temp = IOT_SIMPLE_OTA_FAIL;
                    error_info = OTA_ERROR_DOWNLOAD_FAIL;
                    ctr_ota_set_state((IOT_SIMPLE_OTA_STA_TYP)temp);
                    ctr_ota_set_error((OTA_ERROR_TYPE_TYP)error_info);
                }
            }
            break;

            case IOT_SIMPLE_OTA_URL_AUTH_FAIL:
            {
                if (ctr_ota_switch_to_pending_url()) {
                    break;
                }

                if ((now - state_enter_tick) >= pdMS_TO_TICKS(CTR_OTA_WAIT_NEW_URL_TIMEOUT_MS)) {
                    ESP_LOGE(TAG, "Wait OTA new URL timeout");
                    ESP_LOGE(TAG, "OTA wait new URL timeout taskId=%s msgId=%s", g_ota_info.ota_tkid, g_ota_info.ota_msgid);
                    ctr_ota_set_error(OTA_ERROR_DOWNLOAD_FAIL);
                    ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                }
            }
            break;

            case IOT_SIMPLE_OTA_CRC_OK:
            {
                if (ctr_ota_switch_to_pending_url()) {
                    break;
                }

#if OTA_LOCAL_SIM_TEST
                ESP_LOGW(TAG, "Simulate OTA server check success for local OTA testing taskId=%s", g_ota_info.ota_tkid);
                ctr_ota_set_state(IOT_SIMPLE_OTA_PACK_CHECK_END);
#else
                if (ctr_ota_is_local_http_test_active()) {
                    ESP_LOGW(TAG, "LOCAL_TEST: skip MQTT server check and advance to PACK_CHECK_END taskId=%s", g_ota_info.ota_tkid);
                    ctr_ota_set_state(IOT_SIMPLE_OTA_PACK_CHECK_END);
                } else {
                    publish_ctr_ota_check_status_to_mqtt();
                    vTaskDelay(2000);
                }
#endif
            }
            break;

                        case IOT_SIMPLE_OTA_PACK_CHECK_END:
            {
                if (ctr_ota_switch_to_pending_url()) {
                    break;
                }

                if (g_ota_info.ota_auto_up[0] == '1')
                {
                    if (ctr_ota_should_delay_auto_update()) {
                        ESP_LOGI(TAG, "Auto OTA is pending, wait for machine task to finish before continue");
                        ctr_ota_set_state(IOT_SIMPLE_OTA_WAIT_CONFIRM);
                    } else {
                        if (ctr_ota_is_hmi_only_bundle()) {
                            ESP_LOGI(TAG, "HMI OTA auto confirmed by autoUpdateFlag=1, stage ESP32 image now");
                            if (ctr_ota_stage_esp32_from_bundle() != ESP_OK) {
                                ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                                ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                            } else {
                                ctr_ota_set_error(OTA_ERROR_NONE);
                                ctr_ota_set_state(IOT_SIMPLE_OTA_SUCCESS);
                            }
                        } else {
                            ESP_LOGI(TAG, "OTA auto update confirmed by autoUpdateFlag=1, request controller reboot to bootloader");
                            publish_ota_status_to_mqtt(OTA_YES_CONFIRM);
                            if (!ctr_ota_enter_ymodem_mode())
                            {
                                ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                                ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                            }
                        }
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "OTA package approved by server, waiting external confirmUpdate");
                    if (!ctr_ota_is_local_http_test_active()) {
                        publish_ota_status_to_mqtt(OTA_WAIT_CONFIRM);
                    } else {
                        ESP_LOGW(TAG, "LOCAL_TEST: skip MQTT wait-confirm publish and enter WAIT_CONFIRM directly");
                    }
                    ctr_ota_set_state(IOT_SIMPLE_OTA_WAIT_CONFIRM);
                }
            }
            break;

                                    case IOT_SIMPLE_OTA_WAIT_CONFIRM:
            {
                if (ctr_ota_switch_to_pending_url()) {
                    break;
                }

                if (g_ota_info.ota_auto_up[0] == '1')
                {
                    if (ctr_ota_should_delay_auto_update()) {
                        break;
                    }

                    ESP_LOGI(TAG, "Auto OTA resume now because machine task finished");
                    publish_ota_status_to_mqtt(OTA_YES_CONFIRM);
                    ota_params_edit(OTA_INFO_AUTOUP, "9");
                }

                if (g_ota_info.ota_auto_up[0] == '9')
                {
                    if (ctr_ota_is_hmi_only_bundle()) {
                        ESP_LOGI(TAG, "HMI OTA manual confirm received from local UI, staged image is ready, finish OTA");
                        if (!g_ota_package_probe.esp32_staged) {
                            ESP_LOGE(TAG, "HMI OTA confirm reached WAIT_CONFIRM without staged ESP32 image");
                            ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                            ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                        } else {
                            ctr_ota_set_error(OTA_ERROR_NONE);
                            ctr_ota_set_state(IOT_SIMPLE_OTA_SUCCESS);
                        }
                    } else {
                        ESP_LOGI(TAG, "OTA manual confirm already received, request controller reboot to bootloader");
                        if (!ctr_ota_enter_ymodem_mode())
                        {
                            ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                            ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                        }
                    }
                }
                else if (ctr_ota_is_local_http_test_active())
                {
                    ESP_LOGW(TAG, "LOCAL_TEST: auto confirm OTA and continue");
                    ota_params_edit(OTA_INFO_AUTOUP, "9");
                }
#if OTA_LOCAL_SIM_TEST
                else if (!ctr_ota_is_hmi_only_bundle() &&
                         (now - state_enter_tick) >= pdMS_TO_TICKS(CTR_OTA_SIM_CONFIRM_DELAY_MS))
                {
                    ESP_LOGW(TAG, "Simulate confirmUpdate for local OTA testing taskId=%s", g_ota_info.ota_tkid);
                    publish_ota_status_to_mqtt(OTA_YES_CONFIRM);
                    ota_params_edit(OTA_INFO_AUTOUP, "9");
                }
#endif
            }
            break;

                                    case IOT_SIMPLE_OTA_YMODEM:
            {
                if (ctr_ota_switch_to_pending_url()) {
                    sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
                    break;
                }

                if (!ymodem_transfer_done)
                {
                    if (ctr_version_before_ota[0] == '\0') {
                        snprintf(ctr_version_before_ota,
                                 sizeof(ctr_version_before_ota),
                                 "%s",
                                 g_device_version.current_ctr_fw_version);
                    }

#if OTA_LOCAL_SIM_TEST
                    if (sys_pra.app_mode != APP_MODE_YMODEM &&
                        (now - state_enter_tick) >= pdMS_TO_TICKS(CTR_OTA_SIM_BOOT_C_DELAY_MS))
                    {
                        sys_pra.app_mode = APP_MODE_YMODEM;
                        ESP_LOGW(TAG, "Simulate controller bootloader handshake(C) for local OTA testing");
                    }
#endif

                    if (sys_pra.app_mode == APP_MODE_YMODEM)
                    {
                        unsigned int file_size = 0;
                        vTaskDelay(200);
                        sscanf(g_ota_info.ota_file_path, "%08X", &file_size);
                        uart_flush_input(CTR_UART_NUM);

#if OTA_LOCAL_SIM_TEST
                        ESP_LOGW(TAG, "Simulate YMODEM transfer success for local OTA testing file=%s size=%u", g_ota_info.ota_file_name, file_size);
                        ymodem_transfer_done = true;
                        ymodem_complete_tick = xTaskGetTickCount();
                        ctr_ota_set_error(OTA_ERROR_NONE);
                        sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
#else
                        ESP_LOGW(TAG,
                                 "Controller bootloader is already in YMODEM mode, skip #SOCKA pre-command and send raw YMODEM directly");
                        ESP_LOGI(TAG, "OTA YMODEM start file=%s size=%u oldCtrVer=%s", g_ota_info.ota_file_name, file_size, ctr_version_before_ota);
                        if (ymodem_send_file(g_ota_info.ota_file_name, file_size) == 0)
                        {
                            ESP_LOGI(TAG, "OTA YMODEM transfer finished, wait controller version change");
                            ymodem_transfer_done = true;
                            ymodem_complete_tick = xTaskGetTickCount();
                            ctr_ota_set_error(OTA_ERROR_NONE);
                        }
                        else
                        {
                            ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                            ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                        }

                        sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
#endif
                    }
                    else if (ymodem_wait_log_tick == 0 ||
                             (now - ymodem_wait_log_tick) >= pdMS_TO_TICKS(CTR_OTA_BOOT_WAIT_LOG_MS))
                    {
                        ymodem_wait_log_tick = now;
                        ESP_LOGI(TAG,
                                 "Waiting controller bootloader handshake(C) taskId=%s state=%d appMode=%d ctrVer=%s",
                                 g_ota_info.ota_tkid,
                                 g_ota_info.ota_sta,
                                 (int)sys_pra.app_mode,
                                 g_device_version.current_ctr_fw_version);
                        ctr_ota_log_controller_gate("OTA wait handshake");
                    }

                    if ((now - state_enter_tick) >= pdMS_TO_TICKS(CTR_OTA_HANDSHAKE_TIMEOUT_MS))
                    {
                        ESP_LOGE(TAG,
                                 "Controller bootloader handshake timeout taskId=%s appMode=%d ctrVer=%s",
                                 g_ota_info.ota_tkid,
                                 (int)sys_pra.app_mode,
                                 g_device_version.current_ctr_fw_version);
                        ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                        ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                    }
                }
#if OTA_LOCAL_SIM_TEST
                else if ((now - ymodem_complete_tick) >= pdMS_TO_TICKS(CTR_OTA_SIM_APPLY_DELAY_MS))
                {
                    ESP_LOGW(TAG, "Simulate controller apply success for local OTA testing");
                    if (g_ota_package_probe.kind == OTA_PACKAGE_KIND_BUNDLE &&
                        g_ota_package_probe.has_esp32 &&
                        !g_ota_package_probe.esp32_staged) {
                        esp_err_t err = ctr_ota_stage_esp32_from_bundle();
                        if (err != ESP_OK) {
                            ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                            ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                            break;
                        }
                    }
                    ESP_LOGW(TAG,
                             "CTR OTA SUCCESS!!!!!! simulated apply complete oldCtrVer=%s currentCtrVer=%s",
                             ctr_version_before_ota,
                             g_device_version.current_ctr_fw_version);
                    ctr_ota_set_error(OTA_ERROR_NONE);
                    ctr_ota_set_state(IOT_SIMPLE_OTA_SUCCESS);
                }
#else
                else if (ctr_version_before_ota[0] == '\0' ||
                         strcmp(ctr_version_before_ota, g_device_version.current_ctr_fw_version) != 0 ||
                         (sys_pra.app_mode == APP_MODE_RUNTIME_BRIDGE &&
                          (TickType_t)ctr_uart_get_last_status_tick() > ymodem_complete_tick))
                {
                    if (ctr_version_before_ota[0] == '\0' ||
                        strcmp(ctr_version_before_ota, g_device_version.current_ctr_fw_version) != 0) {
                        ESP_LOGI(TAG,
                                 "OTA apply success detected by controller version change old=%s new=%s",
                                 ctr_version_before_ota,
                                 g_device_version.current_ctr_fw_version);
                    } else {
                        ESP_LOGI(TAG,
                                 "OTA apply success detected by controller status recovery after YMODEM, ctrVer=%s",
                                 g_device_version.current_ctr_fw_version);
                    }

                    if (g_ota_package_probe.kind == OTA_PACKAGE_KIND_BUNDLE &&
                        g_ota_package_probe.has_esp32 &&
                        !g_ota_package_probe.esp32_staged) {
                        esp_err_t err = ctr_ota_stage_esp32_from_bundle();
                        if (err != ESP_OK) {
                            ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                            ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                            break;
                        }
                    }

                    ESP_LOGW(TAG,
                             "CTR OTA SUCCESS!!!!!! controller apply complete oldCtrVer=%s newCtrVer=%s",
                             ctr_version_before_ota,
                             g_device_version.current_ctr_fw_version);
                    ctr_ota_set_error(OTA_ERROR_NONE);
                    ctr_ota_set_state(IOT_SIMPLE_OTA_SUCCESS);
                }
                else if ((now - ymodem_complete_tick) >= pdMS_TO_TICKS(CTR_OTA_APPLY_TIMEOUT_MS))
                {
                    ESP_LOGE(TAG, "Controller upgrade result timeout, version unchanged: %s",
                             g_device_version.current_ctr_fw_version);
                    ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
                    ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
                }
#endif
            }
            break;

            case IOT_SIMPLE_OTA_FAIL:
            {
                if (ctr_ota_is_result_ack_received()) {
                    ctr_ota_finish_task();
                    break;
                }

                if (result_last_publish_tick == 0 ||
                    (now - result_last_publish_tick) >= pdMS_TO_TICKS(CTR_OTA_RESULT_ACK_RETRY_MS))
                {
                    ESP_LOGI(TAG, "OTA publish failure result and wait final result ACK");
                    ctr_ota_publish_failure_result();
                    result_last_publish_tick = now;
                }
            }
            break;

            case IOT_SIMPLE_OTA_SUCCESS:
            {
                if (ctr_ota_is_result_ack_received()) {
                    if (g_ota_package_probe.reboot_required) {
                        if (!ctr_ota_ready_to_reboot_after_success(now, &success_voice_wait_tick)) {
                            break;
                        }
                        ctr_ota_log_context("OTA success acked, reboot to apply staged ESP32 image");
                        ota_params_reset();
                        taskENTER_CRITICAL(&ctr_ota_task_lock);
                        ctr_ota_run_flag = 0;
                        taskEXIT_CRITICAL(&ctr_ota_task_lock);
                        ctr_ota_reset_package_probe();
                        esp_restart();
                    }
                    ctr_ota_finish_task();
                    break;
                }

                if (ctr_ota_is_local_http_test_active()) {
                    if (g_ota_package_probe.reboot_required) {
                        if (!ctr_ota_ready_to_reboot_after_success(now, &success_voice_wait_tick)) {
                            break;
                        }
                        ctr_ota_log_context("LOCAL_TEST OTA success, reboot directly to apply staged ESP32 image without MQTT ACK");
                        ota_params_reset();
                        taskENTER_CRITICAL(&ctr_ota_task_lock);
                        ctr_ota_run_flag = 0;
                        taskEXIT_CRITICAL(&ctr_ota_task_lock);
                        ctr_ota_reset_package_probe();
                        esp_restart();
                    }

                    ESP_LOGI(TAG, "LOCAL_TEST OTA success, finish task without waiting MQTT result ACK");
                    ctr_ota_finish_task();
                    break;
                }

                if (result_last_publish_tick == 0 ||
                    (now - result_last_publish_tick) >= pdMS_TO_TICKS(CTR_OTA_RESULT_ACK_RETRY_MS))
                {
                    ESP_LOGI(TAG, "OTA publish success result and wait final result ACK");
                    publish_ota_result_to_mqtt(OTA_RESULT_SUCCESS, "Update_Success");
                    result_last_publish_tick = now;
                }
            }
            break;

            default:
                vTaskDelay(100);
                break;
        }
        vTaskDelay(10);
    }
}

void ctr_ota_task_run(void)
{
    taskENTER_CRITICAL(&ctr_ota_task_lock);
    if (ctr_ota_run_flag)
    {
        taskEXIT_CRITICAL(&ctr_ota_task_lock);
        ctr_ota_log_context("OTA task already running");
        ESP_LOGW(TAG, "Ctr_OTA thread already run");
        return;
    }
    ctr_ota_run_flag = 1;
    taskEXIT_CRITICAL(&ctr_ota_task_lock);

    if (xTaskCreateStatic(&ctr_ota_task,
                          "ctr_ota_task",
                          CTR_OTA_TASK_STACK_SIZE,
                          NULL,
                          5,
                          s_ctr_ota_task_stack,
                          &s_ctr_ota_task_tcb) == NULL)
    {
        taskENTER_CRITICAL(&ctr_ota_task_lock);
        ctr_ota_run_flag = 0;
        taskEXIT_CRITICAL(&ctr_ota_task_lock);
        ctr_ota_log_context("OTA task create failed");
        ESP_LOGE(TAG, "Ctr_OTA thread create failed");
        return;
    }

    ctr_ota_log_context("OTA task started");
    ESP_LOGW(TAG, "Ctr_OTA thread run");
}

void ctr_ota_notify_wifi_ready(void)
{
#if OTA_LOCAL_HTTP_TEST_ENABLE
    if (!s_local_http_test_waiting_for_wifi) {
        return;
    }

    s_local_http_test_waiting_for_wifi = false;
    s_local_http_test_waiting_for_mqtt_bootstrap = true;
    ESP_LOGW(TAG, "WIFI ALL DONE -> wait MQTT bootstrap before local HTTP OTA test");
#endif
}

void ctr_ota_notify_mqtt_bootstrap_ready(void)
{
#if OTA_LOCAL_HTTP_TEST_ENABLE
    if (!s_local_http_test_waiting_for_mqtt_bootstrap) {
        return;
    }

    s_local_http_test_waiting_for_mqtt_bootstrap = false;
    ESP_LOGW(TAG, "MQTT bootstrap complete -> stop MQTT and kick local HTTP OTA test");
    mqtt_app_shutdown();
    vTaskDelay(pdMS_TO_TICKS(300));
    ctr_ota_task_run();
#endif
}

bool ctr_ota_is_waiting_for_mqtt_bootstrap(void)
{
#if OTA_LOCAL_HTTP_TEST_ENABLE
    return s_local_http_test_waiting_for_mqtt_bootstrap;
#else
    return false;
#endif
}

bool ctr_ota_should_block_mqtt_autostart(void)
{
#if OTA_LOCAL_HTTP_TEST_ENABLE
    return ctr_ota_is_local_http_test_active() && !s_local_http_test_waiting_for_mqtt_bootstrap;
#else
    return false;
#endif
}

void ctr_ota_init(void)
{
    ota_params_load(&g_ota_info);
    ctr_ota_log_context("OTA init loaded persisted context");

#if OTA_LOCAL_HTTP_TEST_ENABLE
    if (ctr_ota_has_resume_context() ||
        g_ota_info.ota_sta != IOT_SIMPEL_OTA_NULL ||
        g_ota_info.ota_error_info != OTA_ERROR_NONE) {
        ESP_LOGW(TAG, "Local HTTP OTA test overrides persisted OTA context on boot");
        ctr_ota_log_context("OTA init override persisted context for local HTTP test");
        ota_params_reset();
    }
    ctr_ota_start_local_http_test();
    return;
#endif

    if (strcmp(g_ota_info.ota_dtype, "LOCAL_TEST") == 0) {
        ESP_LOGW(TAG, "Drop persisted LOCAL_TEST OTA context on boot because local HTTP OTA is disabled");
        ctr_ota_log_context("OTA init discard persisted LOCAL_TEST context");
        ota_params_reset();
        return;
    }

    if (!ctr_ota_has_resume_context())
    {
        if (g_ota_info.ota_sta != IOT_SIMPEL_OTA_NULL || g_ota_info.ota_error_info != OTA_ERROR_NONE) {
            ESP_LOGW(TAG, "Drop stale OTA context on boot because resume fields are incomplete");
            ctr_ota_log_context("OTA init discard incomplete context");
            ota_params_reset();
        }
        return;
    }

    switch (g_ota_info.ota_sta)
    {
        case IOT_SIMPLE_OTA_URL_GOT:
        case IOT_SIMPLE_OTA_URL_AUTH_FAIL:
        case IOT_SIMPLE_OTA_CRC_OK:
        case IOT_SIMPLE_OTA_PACK_CHECK_END:
            ESP_LOGW(TAG,
                     "Clear persisted non-terminal OTA state %s on boot because auto resume is disabled",
                     ctr_ota_state_name((IOT_SIMPLE_OTA_STA_TYP)g_ota_info.ota_sta));
            ctr_ota_reset_package_probe();
            ota_params_reset();
            break;

        case IOT_SIMPLE_OTA_WAIT_CONFIRM:
            if (g_ota_info.ota_auto_up[0] == '9') {
                ESP_LOGW(TAG, "Boot with WAIT_CONFIRM but confirm flag already consumed, roll back to wait confirm status");
                ota_params_edit(OTA_INFO_AUTOUP, "0");
            }
            ESP_LOGW(TAG,
                     "Clear persisted OTA WAIT_CONFIRM on boot because auto resume is disabled");
            ctr_ota_reset_package_probe();
            ota_params_reset();
            break;

        case IOT_SIMPLE_OTA_YMODEM:
            ESP_LOGW(TAG, "Boot during YMODEM/apply stage, convert to FAIL and keep result for later MQTT upload");
            ctr_ota_set_error(OTA_ERROR_APPLY_FAIL);
            ctr_ota_set_state(IOT_SIMPLE_OTA_FAIL);
            break;

        case IOT_SIMPLE_OTA_SUCCESS:
        case IOT_SIMPLE_OTA_FAIL:
            ESP_LOGW(TAG,
                     "Persisted terminal OTA state %s loaded on boot; wait for MQTT reconnect to upload softwareUpdateResult",
                     ctr_ota_state_name((IOT_SIMPLE_OTA_STA_TYP)g_ota_info.ota_sta));
            break;

        default:
            ESP_LOGI(TAG, "No OTA recovery needed on boot");
            break;
    }
}

bool upload_ctr_ota_info(void)
{
    if (!ctr_ota_has_persisted_report_context()) {
        ESP_LOGI(TAG, "OTA context is empty, try publish last persisted OTA result instead");
        return publish_last_ota_result_to_mqtt();
    }

    ESP_LOGI(TAG,
             "Upload persisted OTA result after MQTT bootstrap, state=%s(%d) error=%s(%d) taskId=%s file=%s",
             ctr_ota_state_name((IOT_SIMPLE_OTA_STA_TYP)g_ota_info.ota_sta),
             g_ota_info.ota_sta,
             ctr_ota_error_name((OTA_ERROR_TYPE_TYP)g_ota_info.ota_error_info),
             g_ota_info.ota_error_info,
             g_ota_info.ota_tkid,
             g_ota_info.ota_file_name);

    if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_SUCCESS) {
        publish_ota_result_to_mqtt(OTA_RESULT_SUCCESS, "Update_Success");
        return true;
    } else if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_FAIL) {
        ctr_ota_publish_failure_result();
        return true;
    } else if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_YMODEM) {
        publish_ota_result_to_mqtt(OTA_RESULT_BURN_ERR, "Apply_Fail");
        return true;
    } else if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_URL_AUTH_FAIL) {
        if (g_ota_info.ota_error_info != OTA_ERROR_NONE) {
            ctr_ota_publish_failure_result();
        } else {
            publish_ota_result_to_mqtt(OTA_RESULT_DOWNLOAD_ERR, "Download_Fail");
        }
        return true;
    } else if (g_ota_info.ota_error_info != OTA_ERROR_NONE) {
        ctr_ota_publish_failure_result();
        return true;
    } else {
        ESP_LOGI(TAG,
                 "Skip persisted OTA result upload because state=%s(%d) is non-terminal and has no reportable persisted result",
                 ctr_ota_state_name((IOT_SIMPLE_OTA_STA_TYP)g_ota_info.ota_sta),
                 g_ota_info.ota_sta);
        return false;
    }
}

