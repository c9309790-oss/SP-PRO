#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/semphr.h>
#include "board_config.h"
#include "ctr_scheduler.h"
#include "ota_ctr.h"
#include "system_runtime.h"
#include "uart_ctr.h"
#include "sp_pro_app.h"
#include "mqtt_protocol.h"
#include "nvs.h"
#include "esp_err.h"
#include "ram_diag.h"

#define TAG "uart_ctr"

#define CTR_PKT_MAX_LEN       512
#define UART_RX_BUF_SIZE      128
#define UART_RX_RING_BUF_SIZE (1024 * 10)
#define CTR_POLL_PERIOD_MS    200
#define CTR_PARSE_BUF_LEN     CTR_PKT_MAX_LEN
#define CTR_ECHO_WINDOW_MS    500
#define CTR_UART_RX_TASK_STACK_SIZE_DEFAULT 4096
#define CTR_UART_RX_TASK_STACK_SIZE_TEST    4096
#define CTR_UART_RX_TASK_STACK_SIZE         CTR_UART_RX_TASK_STACK_SIZE_TEST
#define CTR_FACTORY_NVS_NAMESPACE "ctr_factory"
#define CTR_FACTORY_NVS_KEY       "factory_v1"
#define CTR_FACTORY_BLOB_MAGIC    0x43544631UL
#define CTR_FACTORY_BLOB_VERSION  1U
#define CTR_FACTORY_FIELD_COUNT   14U
#define CTR_FACTORY_MAX_ABS_FLOAT 1000000.0f
#define CTR_FACTORY_MAX_INT_VALUE 1000000
#define CTR_FACTORY_LIQUID_SCALE_ENABLED 0
#define CTR_FACTORY_FLOWMETER_DEFAULT 0.45f

static QueueHandle_t ctr_uart_event_queue;
static SemaphoreHandle_t ctr_tx_mutex;
static StaticSemaphore_t ctr_tx_mutex_buffer;
static TimerHandle_t ctr_poll_timer = NULL;
static char ctr_last_tx_payload[CTR_PKT_MAX_LEN];
static TickType_t ctr_last_tx_tick = 0;
static volatile TickType_t ctr_last_status_tick = 0;
static ctr_version_update_handler_t ctr_version_update_handler = NULL;
static int ctr_uart_baud_rate = 115200;

MACHINE_STATUS machine_status;
FLASH_FACTORY_DATA factory_data;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    FLASH_FACTORY_DATA data;
} ctr_factory_blob_t;

static bool ctr_factory_float_is_valid(float value);

static const csv_field_desc_t machine_status_fields[] = {
    { CSV_UINT,  &machine_status.error_code, 0 },
    { CSV_UINT,  &machine_status.ctr_status, 0 },

    /* Grinder-related status. */
    { CSV_UINT,  &machine_status.grind_run_flg, 0 },
    { CSV_UINT,  &machine_status.grind_level, 0 },
    { CSV_UINT,  &machine_status.bean_detect_flag, 0 },
    { CSV_UINT,  &machine_status.beanbox_in_place, 0 },
    { CSV_UINT,  &machine_status.grind_handle_postion_flag, 0 },

    /* Brewing path status. */
    { CSV_FLOAT, &machine_status.flow_rate, 0 },
    { CSV_FLOAT, &machine_status.pressure, 0 },
    { CSV_FLOAT, &machine_status.brew_current_temp, 0 },
    { CSV_UINT,  &machine_status.brew_handle_postion_flag, 0 },
    { CSV_UINT,  &machine_status.water_box_shortage_flag, 0 },
    { CSV_UINT,  &machine_status.bin_ready_state, 0 },
    { CSV_UINT,  &machine_status.weight_fluid_flag, 0 },

    { CSV_UINT,  &machine_status.current_stage, 0 },
    { CSV_UINT,  &machine_status.total_stage, 0 },
    { CSV_UINT,  &machine_status.drink_making_flg, 0 },

    /* Hot water status. */
    { CSV_UINT,  &machine_status.hot_water_flg, 0 },
    { CSV_FLOAT, &machine_status.hot_current_temp, 0 },

    /* Steam status. */
    { CSV_UINT,  &machine_status.steam_flag, 0 },
    { CSV_UINT,  &machine_status.steam_level, 0 },
    { CSV_FLOAT, &machine_status.steam_current_temp, 0 },
    { CSV_FLOAT, &machine_status.steam_target_temp, 0 },
    { CSV_FLOAT, &machine_status.milk_target_temp, 0 },
    { CSV_FLOAT, &machine_status.milk_current_temp, 0 },

    /* Weight and ADC readings. */
    { CSV_INT,   &machine_status.powder_adc, 0 },
    { CSV_INT,   &machine_status.liquid_adc, 0 },
    { CSV_FLOAT, &machine_status.powder_weight, 0 },
    { CSV_FLOAT, &machine_status.liquid_weight, 0 },

    /* Controller firmware version. */
    { CSV_STRING, machine_status.ucFwVersion, sizeof(machine_status.ucFwVersion) },

    /* Encoder status. */
    { CSV_UINT,  &machine_status.encoder.active, 0 },
    { CSV_UINT,  &machine_status.encoder.param_id, 0 },
    { CSV_FLOAT, &machine_status.encoder.cur_value, 0 },
    { CSV_INT,   &machine_status.encoder.rotate, 0 },
    { CSV_UINT,  &machine_status.encoder.evt_type, 0 },
    { CSV_UINT,  &machine_status.encoder.evt_seq, 0 },
};

static bool ctr_factory_data_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    static ctr_factory_blob_t blob;
    size_t size = sizeof(blob);

    memset(&blob, 0, sizeof(blob));

    err = nvs_open(CTR_FACTORY_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory nvs_open(read) failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_blob(nvs_handle, CTR_FACTORY_NVS_KEY, &blob, &size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "factory load failed: %s", esp_err_to_name(err));
        }
        return false;
    }

    if (size != sizeof(blob) ||
        blob.magic != CTR_FACTORY_BLOB_MAGIC ||
        blob.version != CTR_FACTORY_BLOB_VERSION) {
        ESP_LOGW(TAG,
                 "factory blob invalid, ignore (size=%u magic=0x%08lX version=%u)",
                 (unsigned)size,
                 (unsigned long)blob.magic,
                 (unsigned)blob.version);
        return false;
    }

    factory_data = blob.data;
    if (!ctr_factory_float_is_valid(factory_data.powder_k_value) ||
        !ctr_factory_float_is_valid(factory_data.powder_b_value)) {
        ESP_LOGW(TAG,
                 "factory NVS powder calibration invalid, default to 0 k=%.8g b=%.8g",
                 (double)factory_data.powder_k_value,
                 (double)factory_data.powder_b_value);
        factory_data.powder_k_value = 0.0f;
        factory_data.powder_b_value = 0.0f;
    }
#if !CTR_FACTORY_LIQUID_SCALE_ENABLED
    factory_data.liquid_k_value = 0.0f;
    factory_data.liquid_b_value = 0.0f;
#endif
    if (!ctr_factory_float_is_valid(factory_data.flowmeter_coff) ||
        factory_data.flowmeter_coff <= 0.0f) {
        factory_data.flowmeter_coff = CTR_FACTORY_FLOWMETER_DEFAULT;
    }
    ESP_LOGI(TAG,
             "factory restored from NVS sn=%s waterMode=%d powderK=%.8g powderB=%.8g",
             factory_data.sn_num,
             factory_data.water_supply_mode,
             (double)factory_data.powder_k_value,
             (double)factory_data.powder_b_value);
    return true;
}

static bool ctr_factory_data_save_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    static ctr_factory_blob_t blob;

    memset(&blob, 0, sizeof(blob));
    blob.magic = CTR_FACTORY_BLOB_MAGIC;
    blob.version = CTR_FACTORY_BLOB_VERSION;
    blob.data = factory_data;

    err = nvs_open(CTR_FACTORY_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory nvs_open(write) failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(nvs_handle, CTR_FACTORY_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory save failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool ctr_copy_csv_payload(const char *src, char *dst, size_t dst_size)
{
    size_t len;

    if (!src || !dst || dst_size == 0U) {
        return false;
    }

    len = strlen(src);
    if (len >= dst_size) {
        ESP_LOGW(TAG, "CSV payload too long: %u", (unsigned)len);
        return false;
    }

    memcpy(dst, src, len + 1U);
    return true;
}

static void ctr_factory_set_reason(char *reason, size_t reason_size, const char *msg)
{
    if (!reason || reason_size == 0U) {
        return;
    }

    snprintf(reason, reason_size, "%s", msg ? msg : "invalid");
}

static bool ctr_factory_string_is_valid(const char *value,
                                        size_t max_size,
                                        bool required,
                                        char *reason,
                                        size_t reason_size,
                                        const char *field_name)
{
    size_t len;

    if (!value || max_size == 0U) {
        ctr_factory_set_reason(reason, reason_size, "string null");
        return false;
    }

    len = strnlen(value, max_size);
    if (len >= max_size) {
        ctr_factory_set_reason(reason, reason_size, "string unterminated");
        return false;
    }
    if (required && len == 0U) {
        ctr_factory_set_reason(reason, reason_size, field_name);
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20U || ch > 0x7eU || ch == ',' || ch == '#' || ch == '@') {
            ctr_factory_set_reason(reason, reason_size, field_name);
            return false;
        }
    }

    return true;
}

static bool ctr_factory_float_is_valid(float value)
{
    return isfinite(value) && fabsf(value) <= CTR_FACTORY_MAX_ABS_FLOAT;
}

bool ctr_factory_data_is_valid(const FLASH_FACTORY_DATA *data, char *reason, size_t reason_size)
{
    if (!data) {
        ctr_factory_set_reason(reason, reason_size, "factory null");
        return false;
    }

    if (!ctr_factory_string_is_valid(data->sn_num,
                                     sizeof(data->sn_num),
                                     false,
                                     reason,
                                     reason_size,
                                     "invalid sn")) {
        return false;
    }
    if (!ctr_factory_string_is_valid(data->model_name,
                                     sizeof(data->model_name),
                                     true,
                                     reason,
                                     reason_size,
                                     "invalid model")) {
        return false;
    }
    if (data->mains_frequency != 50 && data->mains_frequency != 60) {
        ctr_factory_set_reason(reason, reason_size, "invalid mains_frequency");
        return false;
    }
    if (!ctr_factory_float_is_valid(data->powder_k_value) ||
        !ctr_factory_float_is_valid(data->powder_b_value) ||
        !ctr_factory_float_is_valid(data->liquid_k_value) ||
        !ctr_factory_float_is_valid(data->liquid_b_value) ||
        !ctr_factory_float_is_valid(data->flowmeter_coff)) {
        ctr_factory_set_reason(reason, reason_size, "invalid calibration float");
        return false;
    }
    if (data->powder_weight_coff < 0 ||
        data->powder_weight_coff > CTR_FACTORY_MAX_INT_VALUE ||
        data->first_powered_on < 0 ||
        data->first_powered_on > 1 ||
        data->water_supply_mode < 0 ||
        data->water_supply_mode > 1 ||
        data->reserved_2 < 0 ||
        data->reserved_2 > CTR_FACTORY_MAX_INT_VALUE ||
        data->reserved_3 < 0 ||
        data->reserved_3 > CTR_FACTORY_MAX_INT_VALUE ||
        data->reserved_4 < 0 ||
        data->reserved_4 > CTR_FACTORY_MAX_INT_VALUE) {
        ctr_factory_set_reason(reason, reason_size, "invalid factory integer");
        return false;
    }

    ctr_factory_set_reason(reason, reason_size, "ok");
    return true;
}

static bool ctr_parse_int_field(const char *text, int min_value, int max_value, int *out)
{
    char *end = NULL;
    long value;

    if (!text || !out || text[0] == '\0') {
        return false;
    }

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < min_value || value > max_value) {
        return false;
    }

    *out = (int)value;
    return true;
}

static bool ctr_parse_float_field(const char *text, float *out)
{
    char *end = NULL;
    float value;

    if (!text || !out || text[0] == '\0') {
        return false;
    }

    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !ctr_factory_float_is_valid(value)) {
        return false;
    }

    *out = value;
    return true;
}

static bool ctr_split_factory_fields(char *payload, char *fields[], size_t field_count)
{
    char *cursor = payload;
    size_t idx = 0;

    if (!payload || !fields || field_count == 0U) {
        return false;
    }

    while (true) {
        char *comma;

        if (idx >= field_count) {
            return false;
        }

        fields[idx++] = cursor;
        comma = strchr(cursor, ',');
        if (!comma) {
            break;
        }

        *comma = '\0';
        cursor = comma + 1;
    }

    return idx == field_count;
}

static bool parse_factory_csv_strict(char *factory_payload, FLASH_FACTORY_DATA *out)
{
    static char *fields[CTR_FACTORY_FIELD_COUNT];
    static FLASH_FACTORY_DATA parsed;
    static char reason[64];

    if (!factory_payload || !out) {
        return false;
    }

    memset(fields, 0, sizeof(fields));
    memset(&parsed, 0, sizeof(parsed));
    reason[0] = '\0';

    if (strncmp(factory_payload, "FACTORY=", strlen("FACTORY=")) == 0) {
        factory_payload += strlen("FACTORY=");
    }

    if (!ctr_split_factory_fields(factory_payload, fields, CTR_FACTORY_FIELD_COUNT)) {
        ESP_LOGW(TAG, "Reject factory payload: field count mismatch");
        return false;
    }

    for (size_t i = 0; i < CTR_FACTORY_FIELD_COUNT; i++) {
        if (i == 0U) {
            continue;
        }
        if (!fields[i] || fields[i][0] == '\0') {
            ESP_LOGW(TAG, "Reject factory payload: empty field index=%u", (unsigned)i);
            return false;
        }
    }

    if (strlen(fields[0]) >= sizeof(parsed.sn_num) ||
        strlen(fields[1]) >= sizeof(parsed.model_name)) {
        ESP_LOGW(TAG, "Reject factory payload: sn/model too long");
        return false;
    }

    snprintf(parsed.sn_num, sizeof(parsed.sn_num), "%s", fields[0]);
    snprintf(parsed.model_name, sizeof(parsed.model_name), "%s", fields[1]);

    if (!ctr_parse_int_field(fields[2], 1, 400, &parsed.mains_frequency) ||
        !ctr_parse_float_field(fields[3], &parsed.powder_k_value) ||
        !ctr_parse_float_field(fields[4], &parsed.powder_b_value) ||
        !ctr_parse_float_field(fields[5], &parsed.liquid_k_value) ||
        !ctr_parse_float_field(fields[6], &parsed.liquid_b_value) ||
        !ctr_parse_float_field(fields[7], &parsed.flowmeter_coff) ||
        !ctr_parse_int_field(fields[8], 0, CTR_FACTORY_MAX_INT_VALUE, &parsed.powder_weight_coff) ||
        !ctr_parse_int_field(fields[9], 0, 1, &parsed.first_powered_on) ||
        !ctr_parse_int_field(fields[10], 0, 1, &parsed.water_supply_mode) ||
        !ctr_parse_int_field(fields[11], 0, CTR_FACTORY_MAX_INT_VALUE, &parsed.reserved_2) ||
        !ctr_parse_int_field(fields[12], 0, CTR_FACTORY_MAX_INT_VALUE, &parsed.reserved_3) ||
        !ctr_parse_int_field(fields[13], 0, CTR_FACTORY_MAX_INT_VALUE, &parsed.reserved_4)) {
        ESP_LOGW(TAG, "Reject factory payload: field parse failed");
        return false;
    }

#if !CTR_FACTORY_LIQUID_SCALE_ENABLED
    parsed.liquid_k_value = 0.0f;
    parsed.liquid_b_value = 0.0f;
#endif
    if (parsed.flowmeter_coff <= 0.0f) {
        parsed.flowmeter_coff = CTR_FACTORY_FLOWMETER_DEFAULT;
    }

    if (!ctr_factory_data_is_valid(&parsed, reason, sizeof(reason))) {
        ESP_LOGW(TAG, "Reject factory payload: %s", reason);
        return false;
    }

    *out = parsed;
    return true;
}

static void csv_parse_fields_in_place(char *params,
                                      const char *prefix,
                                      const csv_field_desc_t *fields,
                                      uint16_t field_num)
{
    char *token;
    char *rest = params;
    uint16_t idx = 0;

    if (!params || !fields) {
        return;
    }

    if (prefix && strncmp(rest, prefix, strlen(prefix)) == 0) {
        rest += strlen(prefix);
    }

    while ((token = strtok_r(rest, ",", &rest)) && idx < field_num) {
        const csv_field_desc_t *field = &fields[idx];

        switch (field->type) {
        case CSV_INT:
            *(int *)field->dst = atoi(token);
            break;
        case CSV_UINT:
            *(uint32_t *)field->dst = strtoul(token, NULL, 10);
            break;
        case CSV_FLOAT:
            *(float *)field->dst = atof(token);
            break;
        case CSV_STRING:
            strncpy((char *)field->dst, token, field->size - 1U);
            ((char *)field->dst)[field->size - 1U] = '\0';
            break;
        default:
            break;
        }
        idx++;
    }
}

static void ctr_cache_tx_payload(const char *packet)
{
    const char *payload_start;
    const char *payload_end;
    size_t payload_len;

    if (!packet) {
        ctr_last_tx_payload[0] = '\0';
        ctr_last_tx_tick = 0;
        return;
    }

    payload_start = strchr(packet, '@');
    payload_end = strrchr(packet, '#');
    if (!payload_start || !payload_end || payload_end <= payload_start) {
        ctr_last_tx_payload[0] = '\0';
        ctr_last_tx_tick = 0;
        return;
    }

    payload_start++;
    payload_len = (size_t)(payload_end - payload_start);
    if (payload_len >= sizeof(ctr_last_tx_payload)) {
        payload_len = sizeof(ctr_last_tx_payload) - 1U;
    }

    memcpy(ctr_last_tx_payload, payload_start, payload_len);
    ctr_last_tx_payload[payload_len] = '\0';
    ctr_last_tx_tick = xTaskGetTickCount();
}

static bool ctr_is_recent_tx_echo(const char *payload)
{
    if (!payload || ctr_last_tx_payload[0] == '\0') {
        return false;
    }

    if ((xTaskGetTickCount() - ctr_last_tx_tick) > pdMS_TO_TICKS(CTR_ECHO_WINDOW_MS)) {
        return false;
    }

    return strcmp(payload, ctr_last_tx_payload) == 0;
}

static TickType_t ctr_uart_tx_timeout_ticks(int len)
{
    uint32_t baud = (ctr_uart_baud_rate > 0) ? (uint32_t)ctr_uart_baud_rate : 115200U;
    uint32_t bits = (uint32_t)len * 12U;
    uint32_t tx_ms = (bits * 1000U + baud - 1U) / baud;

    /* Leave margin for UART FIFO draining and RS485 direction switching. */
    tx_ms += 50U;
    if (tx_ms < 100U) {
        tx_ms = 100U;
    }

    return pdMS_TO_TICKS(tx_ms);
}

static bool parse_machine_status(const char *params)
{
    static char parse_buf[CTR_PARSE_BUF_LEN];

    if (!ctr_copy_csv_payload(params, parse_buf, sizeof(parse_buf))) {
        return false;
    }

    csv_parse_fields_in_place(parse_buf,
                              NULL,
                              machine_status_fields,
                              sizeof(machine_status_fields) / sizeof(machine_status_fields[0]));
    return true;
}

static bool parse_factory_data(const char *params)
{
    static char parse_buf[CTR_PARSE_BUF_LEN];
    char *factory_payload;
    static FLASH_FACTORY_DATA parsed;

    if (!ctr_copy_csv_payload(params, parse_buf, sizeof(parse_buf))) {
        return false;
    }

    memset(&parsed, 0, sizeof(parsed));
    factory_payload = strstr(parse_buf, "FACTORY=");
    if (!factory_payload) {
        return false;
    }

    if (!parse_factory_csv_strict(factory_payload, &parsed)) {
        return false;
    }

    factory_data = parsed;
    return true;
}

static void ctr_poll_timer_cb(TimerHandle_t timer_handle)
{
    (void)timer_handle;
    if (sys_pra.app_mode == APP_MODE_YMODEM) {
        return;
    }
    ctr_send_cmd(CTR_CMD_READ_ALL, "123@READ@ALL#123");
}

static bool ctr_sync_version_info_from_status(void)
{
    const char *fw_version = (const char *)machine_status.ucFwVersion;
    char version[sizeof(g_device_version.current_ctr_fw_version)] = {0};
    size_t version_len;

    if (!fw_version) {
        return false;
    }

    version_len = strnlen(fw_version, sizeof(machine_status.ucFwVersion));
    if (version_len == 0U) {
        return false;
    }

    snprintf(version, sizeof(version), "%.*s", (int)version_len, fw_version);

    if (strcmp(g_device_version.current_ctr_fw_version, version) == 0) {
        return false;
    }

    update_ctr_version(version);
    ESP_LOGI(TAG, "Synced CTR version: version=%s", g_device_version.current_ctr_fw_version);
    return true;
}

static void ctr_handle_status(const char *payload)
{
    if (!parse_machine_status(payload)) {
        ESP_LOGW(TAG, "Failed to parse machine status payload");
        return;
    }

    // ESP_LOGI(TAG,
            //  "[CTR][STATUS] ctr=%u making=%u stage=%u/%u water_lack=%u err=%u pressure=%.2f brew_temp=%.1f liquid=%.1f",
            //  (unsigned)machine_status.ctr_status,
            //  (unsigned)machine_status.drink_making_flg,
            //  (unsigned)machine_status.current_stage,
            //  (unsigned)machine_status.total_stage,
            //  (unsigned)machine_status.water_box_shortage_flag,
            //  (unsigned)machine_status.error_code,
            //  machine_status.pressure,
            //  machine_status.brew_current_temp,
            //  machine_status.liquid_weight);

    ctr_sync_version_info_from_status();
    ctr_last_status_tick = xTaskGetTickCount();

    sp_pro_update_machine_status(&machine_status);
}

void ctr_register_version_update_handler(ctr_version_update_handler_t handler)
{
    ctr_version_update_handler = handler;
}

uint32_t ctr_uart_get_last_status_tick(void)
{
    return (uint32_t)ctr_last_status_tick;
}

static bool ctr_should_log_ota_resp(void)
{
    return g_ota_info.ota_sta == IOT_SIMPLE_OTA_PACK_CHECK_END ||
           g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM ||
           g_ota_info.ota_sta == IOT_SIMPLE_OTA_YMODEM;
}

static void ctr_log_ota_resp_context(const char *reason)
{
    ESP_LOGW(TAG,
             "%s | otaState=%d appMode=%d ctr_status=%u drink=%u error=0x%08lX waterShort=%u beanBoxAbnormal=%u brewHandleAbnormal=%u grindHandleAbnormal=%u fw=%s",
             reason ? reason : "CTR OTA resp",
             g_ota_info.ota_sta,
             (int)sys_pra.app_mode,
             (unsigned)machine_status.ctr_status,
             (unsigned)machine_status.drink_making_flg,
             (unsigned long)machine_status.error_code,
             (unsigned)machine_status.water_box_shortage_flag,
             (unsigned)machine_status.beanbox_in_place,
             (unsigned)machine_status.brew_handle_postion_flag,
             (unsigned)machine_status.grind_handle_postion_flag,
             g_device_version.current_ctr_fw_version);
}

static void ctr_handle_simple_resp(const char *resp)
{
    ESP_LOGI(TAG, "CTR Resp: %s", resp);

    if (!ctr_should_log_ota_resp()) {
        return;
    }

    if (!strcmp(resp, "ERROR") || !strcmp(resp, "BUSY")) {
        ctr_log_ota_resp_context("Controller rejected OTA event");
    } else if (!strcmp(resp, "OK")) {
        ctr_log_ota_resp_context("Controller accepted OTA event");
    }
}

static void ctr_handle_factory(const char *payload)
{
    ram_diag_snapshot("ctr/factory_rx_before_parse");
    if (!parse_factory_data(payload)) {
        ESP_LOGW(TAG, "Failed to parse factory payload");
        ram_diag_snapshot("ctr/factory_rx_parse_fail");
        return;
    }

    ram_diag_snapshot("ctr/factory_rx_before_nvs_save");
    ctr_factory_data_save_to_nvs();
    ram_diag_snapshot("ctr/factory_rx_after_nvs_save");
    ESP_LOGI(TAG,
             "factory synced from CTR sn=%s waterMode=%d powderK=%.8g powderB=%.8g",
             factory_data.sn_num,
             factory_data.water_supply_mode,
             (double)factory_data.powder_k_value,
             (double)factory_data.powder_b_value);
}

static void ctr_parse_packet(char *packet)
{
    char *crc;
    char *payload;

    if (!packet) {
        return;
    }

    /* Packet layout: 123@payload#crc */
    crc = strrchr(packet, '#');
    if (!crc) {
        return;
    }
    *crc = '\0';

    payload = strchr(packet, '@');
    if (!payload) {
        return;
    }
    payload++;

    if (ctr_is_recent_tx_echo(payload)) {
        ESP_LOGI(TAG, "Ignored echoed CTR TX payload: %s", payload);
        return;
    }

    if (!strcmp(payload, "OK") ||
        !strcmp(payload, "ERROR") ||
        !strcmp(payload, "BUSY")) {
        ctr_handle_simple_resp(payload);
        return;
    }

    if (isdigit((unsigned char)payload[0])) {
        ctr_handle_status(payload);
        return;
    }

    if ((!strncmp(payload, "PCWRITE", 7) || !strncmp(payload, "PCREAD", 6)) &&
        strstr(payload, "FACTORY=") != NULL) {
        ctr_handle_factory(payload);
        return;
    }

    ESP_LOGW(TAG, "Unknown CTR packet: %s", payload);
}

static bool ctr_try_enter_ymodem_mode(char byte_value)
{
    static uint8_t ymodem_c_count = 0;

        if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_YMODEM && byte_value == 'C') {
        ymodem_c_count++;
        if (ymodem_c_count == 1U || ymodem_c_count == 5U) {
            ESP_LOGI(TAG, "Controller bootloader handshake byte 'C' count=%u", (unsigned)ymodem_c_count);
        }
        if (ymodem_c_count >= 5U) {
            ymodem_c_count = 0;
            uart_flush_input(CTR_UART_NUM);
            xQueueReset(ctr_uart_event_queue);
            sys_pra.app_mode = APP_MODE_YMODEM;
            ESP_LOGW(TAG, "Detected controller YMODEM handshake, entering APP_MODE_YMODEM");
            return true;
        }
        return false;
    }

    ymodem_c_count = 0;
    return false;
}

static void ctr_uart_rx_task(void *task_arg)
{
    uart_event_t uart_event;
    uint8_t rx_buf[UART_RX_BUF_SIZE];
    static char packet_buf[CTR_PKT_MAX_LEN];
    static int packet_len = 0;
    static bool ymodem_pause_active = false;
    UBaseType_t watermark;
    bool workload_logged = false;

    (void)task_arg;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "ctr_uart_rx_task start, stack=%u bytes, high_water=%u words",
             (unsigned)CTR_UART_RX_TASK_STACK_SIZE,
             (unsigned)watermark);

    while (1) {
        if (sys_pra.app_mode == APP_MODE_YMODEM) {
            ymodem_pause_active = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ymodem_pause_active) {
            uart_flush_input(CTR_UART_NUM);
            xQueueReset(ctr_uart_event_queue);
            packet_len = 0;
            ymodem_pause_active = false;
        }

        if (!xQueueReceive(ctr_uart_event_queue, &uart_event, portMAX_DELAY)) {
            continue;
        }

        if (uart_event.type == UART_DATA) {
            size_t read_len = uart_event.size < sizeof(rx_buf) ? uart_event.size : sizeof(rx_buf);
            int len = uart_read_bytes(
                CTR_UART_NUM,
                rx_buf,
                read_len,
                portMAX_DELAY);

            for (int i = 0; i < len; i++) {
                char byte_value = (char)rx_buf[i];

                if (ctr_try_enter_ymodem_mode(byte_value)) {
                    packet_len = 0;
                    break;
                }

                if (packet_len >= CTR_PKT_MAX_LEN - 1) {
                    packet_len = 0;
                }

                packet_buf[packet_len++] = byte_value;

                if (byte_value == '#') {
                    packet_buf[packet_len] = '\0';
                    if (!workload_logged) {
                        workload_logged = true;
                        watermark = uxTaskGetStackHighWaterMark(NULL);
                        ESP_LOGI(TAG,
                                 "ctr_uart_rx_task first packet parsed, high_water=%u words",
                                 (unsigned)watermark);
                    }
                    ctr_parse_packet(packet_buf);
                    packet_len = 0;
                }
            }
        } else if (uart_event.type == UART_FIFO_OVF ||
                   uart_event.type == UART_BUFFER_FULL) {
            watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGW(TAG,
                     "ctr_uart_rx_task fifo/buffer full, high_water=%u words",
                     (unsigned)watermark);
            uart_flush_input(CTR_UART_NUM);
            xQueueReset(ctr_uart_event_queue);
            packet_len = 0;
        }
    }
}

void ctr_uart_send_data(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }

    if (!ctr_tx_mutex) {
        ESP_LOGE(TAG, "CTR TX mutex not ready");
        return;
    }

    if (xSemaphoreTake(ctr_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "CTR TX mutex timeout");
        return;
    }

    /* Switch the RS485 transceiver to TX mode. */
    gpio_set_level(PIN_CTR_RS485_DIR, 1);
    esp_rom_delay_us(20);

    ctr_cache_tx_payload(data);
    // ESP_LOGI(TAG, "[CTR][TX] len=%d data=%.*s", len, len, data);
    (void)uart_write_bytes(CTR_UART_NUM, data, len);
    (void)uart_wait_tx_done(CTR_UART_NUM, ctr_uart_tx_timeout_ticks(len));
    // ESP_LOGI(TAG, "[CTR][TX][RESULT] written=%d wait_err=%d", written_len, (int)wait_err);

    gpio_set_level(PIN_CTR_RS485_DIR, 0);
    xSemaphoreGive(ctr_tx_mutex);
}

void ctr_factory_data_persist(void)
{
    (void)ctr_factory_data_save_to_nvs();
}

void ctr_uart_init(int baud_rate, int rx_pin, int tx_pin)
{
    ctr_uart_baud_rate = baud_rate;
    (void)ctr_factory_data_load_from_nvs();

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_CTR_RS485_DIR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);

    /* Keep RS485 in RX mode by default. */
    gpio_set_level(PIN_CTR_RS485_DIR, 0);

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_param_config(CTR_UART_NUM, &uart_config);
    uart_set_pin(CTR_UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* Install the UART driver and create the event queue. */
    uart_driver_install(CTR_UART_NUM,
                        KLM_UART_BUF_SIZE * 2,
                        KLM_UART_BUF_SIZE * 2,
                        20,
                        &ctr_uart_event_queue,
                        0);

    ctr_tx_mutex = xSemaphoreCreateMutexStatic(&ctr_tx_mutex_buffer);
    if (!ctr_tx_mutex) {
        ESP_LOGE(TAG, "Failed to create CTR TX mutex");
        return;
    }

    if (xTaskCreate(ctr_uart_rx_task, "ctr_uart_rx_task", CTR_UART_RX_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create ctr_uart_rx_task, stack=%u bytes",
                 (unsigned)CTR_UART_RX_TASK_STACK_SIZE);
        return;
    }
    ESP_LOGI(TAG,
             "ctr_uart_rx_task created, stack=%u bytes",
             (unsigned)CTR_UART_RX_TASK_STACK_SIZE);

    /* Periodically queue READ@ALL requests for the controller. */
    ctr_poll_timer = xTimerCreate("ctr_poll_timer",
                                  pdMS_TO_TICKS(CTR_POLL_PERIOD_MS),
                                  pdTRUE,
                                  NULL,
                                  ctr_poll_timer_cb);
    if (ctr_poll_timer != NULL) {
        xTimerStart(ctr_poll_timer, 0);
    } else {
        ESP_LOGE(TAG, "Failed to create ctr_poll_timer");
    }
}









