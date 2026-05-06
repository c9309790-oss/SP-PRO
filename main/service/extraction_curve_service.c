#include "extraction_curve_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt.h"
#include "record_publish_port.h"

#if EXTRACTION_CURVE_FEATURE_ENABLE

#define EXTRACTION_CURVE_FILE_PATH               "/spiffs/extraction_curve.bin"
#define EXTRACTION_CURVE_MAGIC                   0x43555256UL
#define EXTRACTION_CURVE_VERSION                 1U
#define EXTRACTION_CURVE_PENDING_MAX             8U
#define EXTRACTION_CURVE_SAMPLE_INTERVAL_MS      500LL
#define EXTRACTION_CURVE_REMOTE_IDLE_MS          1000LL
#define EXTRACTION_CURVE_ACK_TIMEOUT_MS          10000LL
#define EXTRACTION_CURVE_MAX_RETRY_COUNT         5U

static const char *TAG = "EXTRACT_CURVE";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t next_id;
    extraction_curve_record_t records[EXTRACTION_CURVE_PENDING_MAX];
} extraction_curve_blob_t;

typedef struct {
    bool initialized;
    extraction_curve_blob_t blob;
} extraction_curve_store_t;

typedef struct {
    bool active;
    bool local_session;
    bool machine_started;
    bool failed;
    app_state_t local_state;
    control_action_t remote_action;
    int64_t monotonic_start_ms;
    int64_t last_sample_ms;
    int64_t idle_since_ms;
    extraction_curve_record_t record;
} extraction_curve_session_t;

typedef struct {
    bool inflight;
    bool last_connected;
    bool wait_reconnect_after_retry_exhausted;
    uint8_t retry_count;
    int64_t deadline_ms;
    uint32_t inflight_ids[EXTRACTION_CURVE_PENDING_MAX];
    uint16_t inflight_id_count;
    extraction_curve_record_t retry_batch[EXTRACTION_CURVE_PENDING_MAX];
} extraction_curve_publish_runtime_t;

static extraction_curve_store_t s_store = {0};
static extraction_curve_session_t s_session = {0};
static extraction_curve_publish_runtime_t s_publish = {0};

static bool extraction_curve_is_local_beverage_state(app_state_t state)
{
    return state == ST_ESPRESSO ||
           state == ST_MASTER ||
           state == ST_AMERICANO ||
           state == ST_COLD_BREW;
}

static bool extraction_curve_is_remote_action(control_action_t action)
{
    return action == CTRL_ACT_ESPRESSO ||
           action == CTRL_ACT_AMERICANO_BREW ||
           action == CTRL_ACT_COLD_BREW;
}

static bool extraction_curve_is_brewing_status(const MACHINE_STATUS *status)
{
    return status != NULL && status->drink_making_flg != DRINK_MAKER_NONE;
}

static uint8_t extraction_curve_state_to_drink_id(app_state_t state)
{
    switch (state) {
    case ST_MASTER:
        return DRINK_ID_MASTER;
    case ST_ESPRESSO:
        return DRINK_ID_ESPRESSO;
    case ST_AMERICANO:
        return DRINK_ID_AMERICAN;
    case ST_COLD_BREW:
        return DRINK_ID_COLDBREW;
    default:
        return DRINK_ID_MASTER;
    }
}

static const char *extraction_curve_drink_name(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_MASTER:
        return "Player";
    case DRINK_ID_ESPRESSO:
        return "Espresso";
    case DRINK_ID_AMERICAN:
        return "Americano";
    case DRINK_ID_COLDBREW:
        return "Cold Brew";
    default:
        return "Unknown";
    }
}

static int64_t extraction_curve_now_monotonic_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static long extraction_curve_now_unix_sec(void)
{
    return (long)time(NULL);
}

static void extraction_curve_init_defaults(void)
{
    memset(&s_store.blob, 0, sizeof(s_store.blob));
    s_store.blob.magic = EXTRACTION_CURVE_MAGIC;
    s_store.blob.version = EXTRACTION_CURVE_VERSION;
    s_store.blob.next_id = 1U;
}

static void extraction_curve_reset_session(void)
{
    memset(&s_session, 0, sizeof(s_session));
}

static bool extraction_curve_load(void)
{
    FILE *fp;
    size_t len;

    fp = fopen(EXTRACTION_CURVE_FILE_PATH, "rb");
    if (fp == NULL) {
        extraction_curve_init_defaults();
        return true;
    }

    len = fread(&s_store.blob, 1U, sizeof(s_store.blob), fp);
    fclose(fp);

    if (len != sizeof(s_store.blob) ||
        s_store.blob.magic != EXTRACTION_CURVE_MAGIC ||
        s_store.blob.version != EXTRACTION_CURVE_VERSION ||
        s_store.blob.record_count > EXTRACTION_CURVE_PENDING_MAX) {
        ESP_LOGW(TAG, "curve file invalid or truncated, reset defaults readLen=%u", (unsigned int)len);
        extraction_curve_init_defaults();
        return false;
    }

    if (s_store.blob.next_id == 0U) {
        s_store.blob.next_id = 1U;
    }

    return true;
}

static bool extraction_curve_save(void)
{
    FILE *fp = fopen(EXTRACTION_CURVE_FILE_PATH, "wb");
    size_t written;

    if (fp == NULL) {
        ESP_LOGE(TAG, "curve file open(write) failed path=%s", EXTRACTION_CURVE_FILE_PATH);
        return false;
    }

    written = fwrite(&s_store.blob, 1U, sizeof(s_store.blob), fp);
    fflush(fp);
    fclose(fp);

    if (written != sizeof(s_store.blob)) {
        ESP_LOGE(TAG,
                 "curve file save failed written=%u expected=%u",
                 (unsigned int)written,
                 (unsigned int)sizeof(s_store.blob));
        return false;
    }

    return true;
}

static void extraction_curve_build_local_formula(app_state_t state,
                                                 const app_beverage_settings_t *settings,
                                                 formula_info_t *formula)
{
    uint8_t drink_id;
    const char *drink_name;

    if (!settings || !formula) {
        return;
    }

    memset(formula, 0, sizeof(*formula));
    drink_id = extraction_curve_state_to_drink_id(state);
    drink_name = extraction_curve_drink_name(drink_id);

    formula->drink_id = drink_id;
    snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", drink_name);
    snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", drink_name);
    formula->grind_weight = settings->grind_w;

    switch (state) {
    case ST_MASTER:
    case ST_ESPRESSO:
        formula->preset_temperature = (uint16_t)(settings->esp_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->esp_brew_w + 0.5f);
        break;
    case ST_AMERICANO:
        formula->preset_temperature = (uint16_t)(settings->ame_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->ame_brew_w + 0.5f);
        formula->water_temperature = (uint16_t)(settings->ame_water_t + 0.5f);
        formula->water_weight = (uint16_t)(settings->ame_water_w + 0.5f);
        break;
    case ST_COLD_BREW:
        formula->preset_liquid_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        formula->water_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        break;
    default:
        break;
    }
}

static void extraction_curve_start_session(bool local_session,
                                           app_state_t local_state,
                                           control_action_t remote_action,
                                           const formula_info_t *formula)
{
    if (!formula) {
        return;
    }

    extraction_curve_reset_session();
    s_session.active = true;
    s_session.local_session = local_session;
    s_session.local_state = local_state;
    s_session.remote_action = remote_action;
    s_session.monotonic_start_ms = extraction_curve_now_monotonic_ms();
    s_session.record.semi_formula = *formula;
    s_session.record.curve_update = false;

    ESP_LOGI(TAG,
             "curve session start local=%d state=%d action=%d formula=%s drinkId=%u",
             local_session ? 1 : 0,
             (int)local_state,
             (int)remote_action,
             formula->formula_name,
             (unsigned int)formula->drink_id);
}

static void extraction_curve_append_sample(const MACHINE_STATUS *status)
{
    extraction_curve_point_t *point;
    int64_t now_ms;

    if (!s_session.active || !status) {
        return;
    }

    now_ms = extraction_curve_now_monotonic_ms();
    if (s_session.record.point_count > 0 &&
        (now_ms - s_session.last_sample_ms) < EXTRACTION_CURVE_SAMPLE_INTERVAL_MS) {
        return;
    }

    if (s_session.record.point_count >= EXTRACTION_CURVE_MAX_POINTS) {
        return;
    }

    point = &s_session.record.points[s_session.record.point_count++];
    point->elapsed_ms = (uint32_t)(now_ms - s_session.monotonic_start_ms);
    point->pressure = status->pressure;
    point->flow_rate = status->flow_rate;
    s_session.last_sample_ms = now_ms;
}

static void extraction_curve_drop_pending_index(uint16_t index)
{
    if (index >= s_store.blob.record_count) {
        return;
    }

    for (uint16_t i = index; i + 1U < s_store.blob.record_count; i++) {
        s_store.blob.records[i] = s_store.blob.records[i + 1U];
    }
    memset(&s_store.blob.records[s_store.blob.record_count - 1U], 0, sizeof(s_store.blob.records[0]));
    s_store.blob.record_count--;
}

static void extraction_curve_enqueue_record(void)
{
    extraction_curve_record_t *slot;
    uint32_t assigned_id;

    if (!s_session.active || s_session.record.point_count == 0U) {
        extraction_curve_reset_session();
        return;
    }

    if (s_store.blob.record_count >= EXTRACTION_CURVE_PENDING_MAX) {
        ESP_LOGW(TAG, "curve pending queue full, drop oldest id=%lu",
                 (unsigned long)s_store.blob.records[0].id);
        extraction_curve_drop_pending_index(0U);
    }

    slot = &s_store.blob.records[s_store.blob.record_count++];
    *slot = s_session.record;
    assigned_id = s_store.blob.next_id++;
    slot->id = assigned_id;
    slot->produce_time = extraction_curve_now_unix_sec();
    if (s_store.blob.next_id == 0U) {
        s_store.blob.next_id = 1U;
    }

    if (!extraction_curve_save()) {
        ESP_LOGW(TAG,
                 "curve enqueue rollback because save failed id=%lu pointCount=%u formula=%s",
                 (unsigned long)assigned_id,
                 (unsigned int)slot->point_count,
                 slot->semi_formula.formula_name);
        memset(slot, 0, sizeof(*slot));
        s_store.blob.record_count--;
        s_store.blob.next_id = assigned_id;
        extraction_curve_reset_session();
        return;
    }
    ESP_LOGI(TAG,
             "curve queued id=%lu pointCount=%u formula=%s pending=%u",
             (unsigned long)slot->id,
             (unsigned int)slot->point_count,
             slot->semi_formula.formula_name,
             (unsigned int)s_store.blob.record_count);
    extraction_curve_reset_session();
}

static void extraction_curve_discard_session(const char *reason)
{
    if (!s_session.active) {
        return;
    }

    ESP_LOGI(TAG,
             "curve session discarded reason=%s local=%d formula=%s points=%u started=%d failed=%d",
             reason ? reason : "",
             s_session.local_session ? 1 : 0,
             s_session.record.semi_formula.formula_name,
             (unsigned int)s_session.record.point_count,
             s_session.machine_started ? 1 : 0,
             s_session.failed ? 1 : 0);
    extraction_curve_reset_session();
}

static bool extraction_curve_collect_records_by_ids(extraction_curve_record_t *out_records,
                                                    const uint32_t *ids,
                                                    uint16_t id_count)
{
    uint16_t found = 0U;

    if (!out_records || !ids || id_count == 0U) {
        return false;
    }

    for (uint16_t i = 0U; i < id_count; i++) {
        for (uint16_t j = 0U; j < s_store.blob.record_count; j++) {
            if (s_store.blob.records[j].id == ids[i]) {
                out_records[found++] = s_store.blob.records[j];
                break;
            }
        }
    }

    return found == id_count;
}

static bool extraction_curve_publish_batch(const extraction_curve_record_t *records,
                                           uint16_t record_count)
{
    if (!records || record_count == 0U) {
        return false;
    }

    if (!record_publish_extraction_curve_batch(records, (int)record_count, false)) {
        return false;
    }

    return true;
}

static void extraction_curve_start_inflight_from_pending(void)
{
    if (s_store.blob.record_count == 0U || s_publish.inflight || s_publish.wait_reconnect_after_retry_exhausted) {
        return;
    }

    for (uint16_t i = 0U; i < s_store.blob.record_count; i++) {
        s_publish.inflight_ids[i] = s_store.blob.records[i].id;
    }

    if (!extraction_curve_publish_batch(s_store.blob.records, s_store.blob.record_count)) {
        ESP_LOGW(TAG, "curve publish skipped because MQTT publish failed");
        return;
    }

    s_publish.inflight = true;
    s_publish.retry_count = 0U;
    s_publish.inflight_id_count = s_store.blob.record_count;
    s_publish.deadline_ms = extraction_curve_now_monotonic_ms() + EXTRACTION_CURVE_ACK_TIMEOUT_MS;
    ESP_LOGI(TAG,
             "curve publish batch started count=%u firstId=%lu",
             (unsigned int)s_publish.inflight_id_count,
             (unsigned long)s_publish.inflight_ids[0]);
}

static void extraction_curve_retry_inflight_if_needed(void)
{
    int64_t now_ms = extraction_curve_now_monotonic_ms();
    int64_t backoff_ms;

    if (!s_publish.inflight || now_ms < s_publish.deadline_ms) {
        return;
    }

    if (s_publish.retry_count >= EXTRACTION_CURVE_MAX_RETRY_COUNT) {
        ESP_LOGW(TAG,
                 "curve retry exhausted retryCount=%u pendingIds=%u, wait until reconnect",
                 (unsigned int)s_publish.retry_count,
                 (unsigned int)s_publish.inflight_id_count);
        s_publish.inflight = false;
        s_publish.wait_reconnect_after_retry_exhausted = true;
        s_publish.inflight_id_count = 0U;
        return;
    }

    if (!extraction_curve_collect_records_by_ids(s_publish.retry_batch,
                                                 s_publish.inflight_ids,
                                                 s_publish.inflight_id_count)) {
        ESP_LOGW(TAG, "curve retry lost inflight records, reset inflight state");
        s_publish.inflight = false;
        s_publish.inflight_id_count = 0U;
        return;
    }

    if (!extraction_curve_publish_batch(s_publish.retry_batch, s_publish.inflight_id_count)) {
        ESP_LOGW(TAG, "curve retry publish failed");
        return;
    }

    s_publish.retry_count++;
    backoff_ms = EXTRACTION_CURVE_ACK_TIMEOUT_MS << s_publish.retry_count;
    s_publish.deadline_ms = now_ms + backoff_ms;
    ESP_LOGI(TAG,
             "curve retry publish count=%u inflight=%u nextBackoffMs=%lld",
             (unsigned int)s_publish.retry_count,
             (unsigned int)s_publish.inflight_id_count,
             (long long)backoff_ms);
}

static void extraction_curve_process_pending_publish(void)
{
    bool connected = mqtt_is_ui_connected();

    if (connected != s_publish.last_connected) {
        s_publish.last_connected = connected;
        s_publish.inflight = false;
        s_publish.retry_count = 0U;
        s_publish.inflight_id_count = 0U;
        s_publish.deadline_ms = 0LL;
        if (connected) {
            s_publish.wait_reconnect_after_retry_exhausted = false;
            ESP_LOGI(TAG, "curve publish resume after MQTT reconnect pending=%u",
                     (unsigned int)s_store.blob.record_count);
        }
    }

    if (!connected) {
        return;
    }

    if (s_publish.inflight) {
        extraction_curve_retry_inflight_if_needed();
        return;
    }

    extraction_curve_start_inflight_from_pending();
}

bool extraction_curve_service_init(void)
{
    bool loaded;

    extraction_curve_reset_session();
    memset(&s_publish, 0, sizeof(s_publish));
    loaded = extraction_curve_load();
    s_store.initialized = true;
    ESP_LOGI(TAG,
             "curve service init pending=%u nextId=%lu loaded=%d",
             (unsigned int)s_store.blob.record_count,
             (unsigned long)s_store.blob.next_id,
             loaded ? 1 : 0);
    return true;
}

void extraction_curve_notify_local_state_start(app_state_t state, const app_beverage_settings_t *settings)
{
    formula_info_t formula;

    if (!extraction_curve_is_local_beverage_state(state) || !settings) {
        return;
    }

    if (s_session.active && s_session.local_session && s_session.local_state == state) {
        return;
    }

    extraction_curve_build_local_formula(state, settings, &formula);
    extraction_curve_start_session(true, state, CTRL_ACT_NONE, &formula);
}

void extraction_curve_notify_local_state_success(app_state_t state)
{
    if (!s_session.active || !s_session.local_session || s_session.local_state != state) {
        return;
    }

    extraction_curve_enqueue_record();
}

void extraction_curve_notify_local_state_cancel(app_state_t state)
{
    if (!s_session.active || !s_session.local_session || s_session.local_state != state) {
        return;
    }

    extraction_curve_discard_session("local_cancel");
}

void extraction_curve_notify_local_state_fail(app_state_t state)
{
    if (!s_session.active || !s_session.local_session || s_session.local_state != state) {
        return;
    }

    extraction_curve_discard_session("local_fail");
}

void extraction_curve_notify_remote_action_start(control_action_t action, const formula_info_t *formula)
{
    if (!extraction_curve_is_remote_action(action) || !formula) {
        return;
    }

    if (s_session.active && !s_session.local_session && s_session.remote_action == action) {
        return;
    }

    extraction_curve_start_session(false, ST_READY, action, formula);
}

void extraction_curve_notify_remote_cancel(void)
{
    if (!s_session.active || s_session.local_session) {
        return;
    }

    extraction_curve_discard_session("remote_cancel");
}

void extraction_curve_notify_remote_fail(void)
{
    if (!s_session.active || s_session.local_session) {
        return;
    }

    extraction_curve_discard_session("remote_fail");
}

void extraction_curve_handle_machine_status(const MACHINE_STATUS *status)
{
    int64_t now_ms;

    extraction_curve_process_pending_publish();

    if (!s_session.active || !status) {
        return;
    }

    now_ms = extraction_curve_now_monotonic_ms();

    if (extraction_curve_is_brewing_status(status)) {
        s_session.machine_started = true;
        s_session.idle_since_ms = 0LL;
        if (status->error_code != 0U) {
            s_session.failed = true;
        }
        extraction_curve_append_sample(status);
        return;
    }

    if (!s_session.machine_started) {
        return;
    }

    if (status->error_code != 0U) {
        s_session.failed = true;
    }

    if (s_session.local_session) {
        return;
    }

    if (s_session.idle_since_ms == 0LL) {
        s_session.idle_since_ms = now_ms;
        return;
    }

    if ((now_ms - s_session.idle_since_ms) >= EXTRACTION_CURVE_REMOTE_IDLE_MS) {
        if (s_session.failed) {
            extraction_curve_discard_session("remote_finish_failed");
        } else {
            extraction_curve_enqueue_record();
        }
    }
}

void extraction_curve_handle_ack(const uint32_t *curve_ids, size_t curve_id_count)
{
    bool removed = false;

    if (!curve_ids || curve_id_count == 0U) {
        return;
    }

    for (size_t i = 0U; i < curve_id_count; i++) {
        for (uint16_t j = 0U; j < s_store.blob.record_count; j++) {
            if (s_store.blob.records[j].id == curve_ids[i]) {
                ESP_LOGI(TAG, "curve ack received id=%lu", (unsigned long)curve_ids[i]);
                extraction_curve_drop_pending_index(j);
                removed = true;
                break;
            }
        }
    }

    if (!removed) {
        return;
    }

    extraction_curve_save();

    if (s_publish.inflight) {
        uint16_t remaining = 0U;

        for (uint16_t i = 0U; i < s_publish.inflight_id_count; i++) {
            bool acked = false;

            for (size_t j = 0U; j < curve_id_count; j++) {
                if (s_publish.inflight_ids[i] == curve_ids[j]) {
                    acked = true;
                    break;
                }
            }

            if (!acked) {
                s_publish.inflight_ids[remaining++] = s_publish.inflight_ids[i];
            }
        }

        s_publish.inflight_id_count = remaining;
        if (remaining == 0U) {
            s_publish.inflight = false;
            s_publish.retry_count = 0U;
            s_publish.deadline_ms = 0LL;
            ESP_LOGI(TAG, "curve inflight batch fully acked");
        } else {
            s_publish.deadline_ms = extraction_curve_now_monotonic_ms() + EXTRACTION_CURVE_ACK_TIMEOUT_MS;
            ESP_LOGI(TAG,
                     "curve inflight batch partially acked remaining=%u",
                     (unsigned int)remaining);
        }
    }
}

#else

bool extraction_curve_service_init(void)
{
    return true;
}

void extraction_curve_notify_local_state_start(app_state_t state, const app_beverage_settings_t *settings)
{
    (void)state;
    (void)settings;
}

void extraction_curve_notify_local_state_success(app_state_t state)
{
    (void)state;
}

void extraction_curve_notify_local_state_cancel(app_state_t state)
{
    (void)state;
}

void extraction_curve_notify_local_state_fail(app_state_t state)
{
    (void)state;
}

void extraction_curve_notify_remote_action_start(control_action_t action, const formula_info_t *formula)
{
    (void)action;
    (void)formula;
}

void extraction_curve_notify_remote_cancel(void)
{
}

void extraction_curve_notify_remote_fail(void)
{
}

void extraction_curve_handle_machine_status(const MACHINE_STATUS *status)
{
    (void)status;
}

void extraction_curve_handle_ack(const uint32_t *curve_ids, size_t curve_id_count)
{
    (void)curve_ids;
    (void)curve_id_count;
}

#endif
