#include "device_statistics_store.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt.h"
#include "mqtt_protocol.h"
#include "nvs.h"

#define DEVICE_STATS_NVS_NAMESPACE          "device_stats"
#define DEVICE_STATS_KEY_PAYLOAD            "payload"
#define DEVICE_STATS_MAGIC                  0x53544154UL
#define DEVICE_STATS_VERSION                4U
#define DEVICE_STATS_DRINK_KIND_COUNT       5
#define DEVICE_STATS_BUCKET_COUNT           183
#define DEVICE_STATS_BEVERAGE_PERIOD_COUNT  3
#define DEVICE_STATS_BEVERAGE_IDLE_MS       (3000LL)
#define DEVICE_STATS_WATER_IDLE_MS          (1000LL)
#define DEVICE_STATS_GRIND_IDLE_MS          (1000LL)
#define DEVICE_STATS_STEAM_IDLE_MS          (1000LL)
#define DEVICE_STATS_STEAM_MIN_MS           (10000LL)
#define DEVICE_STATS_VALID_UNIX_TIME        1700000000LL
#define DEVICE_STATS_DAY_MS                 (24LL * 60LL * 60LL * 1000LL)
#define DEVICE_STATS_BACKWASH_TOTAL_COUNT   100U
#define DEVICE_STATS_STEAM_CLEAN_TOTAL      10U
#define DEVICE_STATS_DESCALING_TOTAL_COUNT  200U
#define DEVICE_STATS_INVALID_DAY_INDEX      0xFFFFU

static const char *TAG = "DEVICE_STATS";
extern MACHINE_STATUS machine_status;

typedef enum {
    DEVICE_STATS_OP_NONE = 0,
    DEVICE_STATS_OP_BEVERAGE,
    DEVICE_STATS_OP_WATER,
    DEVICE_STATS_OP_STEAM,
    DEVICE_STATS_OP_GRIND,
} device_stats_op_t;

typedef struct {
    uint16_t day_index;
    uint16_t total;
    uint16_t drink_count[DEVICE_STATS_DRINK_KIND_COUNT];
} beverage_day_bucket_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int64_t logical_time_base_ms;
    float total_grind;
    float total_extraction;
    uint32_t steam_time_sec;
    float total_water;
    uint32_t beverage_total;
    uint32_t maintain_brew_count;
    uint32_t descaling_count;
    uint32_t maintain_steam_count;
    uint32_t backwash_count;
    uint32_t steam_clean_count;
    uint32_t descaling_water_count;
    beverage_day_bucket_t beverage_buckets[DEVICE_STATS_BUCKET_COUNT];
} device_statistics_blob_t;

typedef struct {
    bool initialized;
    bool dirty;
    device_statistics_blob_t blob;
} device_statistics_store_t;

typedef struct {
    bool active;
    bool local_state_session;
    bool machine_started;
    app_state_t local_state;
    device_stats_op_t op;
    uint8_t drink_id;
    char formula_name[64];
    float start_powder_weight;
    float start_liquid_weight;
    int64_t start_ms;
    int64_t idle_since_ms;
} device_statistics_session_t;

static device_statistics_store_t s_device_stats = {0};
static device_statistics_session_t s_session = {0};
static beverage_period_data_t s_beverage_periods[DEVICE_STATS_BEVERAGE_PERIOD_COUNT] = {0};
static beverage_data_t s_beverage_period_items[DEVICE_STATS_BEVERAGE_PERIOD_COUNT][DEVICE_STATS_DRINK_KIND_COUNT] = {0};

static const int s_beverage_period_days[DEVICE_STATS_BEVERAGE_PERIOD_COUNT] = {
    0,
    7,
    183,
};

static const char *device_statistics_drink_name(uint8_t drink_id)
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
    case DRINK_ID_WATER:
        return "Water";
    default:
        return "Unknown";
    }
}

static uint8_t device_statistics_action_drink_id(control_action_t action)
{
    switch (action) {
    case CTRL_ACT_ESPRESSO:
        return DRINK_ID_ESPRESSO;
    case CTRL_ACT_AMERICANO_BREW:
        return DRINK_ID_AMERICAN;
    case CTRL_ACT_COLD_BREW:
        return DRINK_ID_COLDBREW;
    default:
        return DRINK_ID_MASTER;
    }
}

static device_stats_op_t device_statistics_state_to_op(app_state_t state)
{
    switch (state) {
    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_AMERICANO:
    case ST_COLD_BREW:
        return DEVICE_STATS_OP_BEVERAGE;
    case ST_WATER:
        return DEVICE_STATS_OP_WATER;
    case ST_STEAM:
        return DEVICE_STATS_OP_STEAM;
    case ST_GRIND:
        return DEVICE_STATS_OP_GRIND;
    default:
        return DEVICE_STATS_OP_NONE;
    }
}

static uint8_t device_statistics_state_to_drink_id(app_state_t state)
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

static int64_t device_statistics_now_ms(void)
{
    time_t unix_now = time(NULL);

    if ((int64_t)unix_now >= DEVICE_STATS_VALID_UNIX_TIME) {
        return (int64_t)unix_now * 1000LL;
    }

    return s_device_stats.blob.logical_time_base_ms + (esp_timer_get_time() / 1000LL);
}

static int device_statistics_round_to_int(float value)
{
    return (value >= 0.0f) ? (int)(value + 0.5f) : (int)(value - 0.5f);
}

static void device_statistics_reset_session(void)
{
    memset(&s_session, 0, sizeof(s_session));
}

static void device_statistics_init_defaults(void)
{
    memset(&s_device_stats.blob, 0, sizeof(s_device_stats.blob));
    s_device_stats.blob.magic = DEVICE_STATS_MAGIC;
    s_device_stats.blob.version = DEVICE_STATS_VERSION;
    s_device_stats.blob.logical_time_base_ms = esp_timer_get_time() / 1000LL;
    for (int i = 0; i < DEVICE_STATS_BUCKET_COUNT; i++) {
        s_device_stats.blob.beverage_buckets[i].day_index = DEVICE_STATS_INVALID_DAY_INDEX;
    }
}

static void device_statistics_mark_dirty(void)
{
    s_device_stats.dirty = true;
    s_device_stats.blob.logical_time_base_ms = device_statistics_now_ms();
}

static bool device_statistics_load(void)
{
    nvs_handle_t nvs_handle;
    size_t len = sizeof(s_device_stats.blob);
    esp_err_t err = nvs_open(DEVICE_STATS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        device_statistics_init_defaults();
        return true;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stats nvs open failed: %s", esp_err_to_name(err));
        device_statistics_init_defaults();
        return false;
    }

    err = nvs_get_blob(nvs_handle, DEVICE_STATS_KEY_PAYLOAD, &s_device_stats.blob, &len);
    nvs_close(nvs_handle);

    if (err != ESP_OK || len != sizeof(s_device_stats.blob) ||
        s_device_stats.blob.magic != DEVICE_STATS_MAGIC ||
        s_device_stats.blob.version != DEVICE_STATS_VERSION) {
        device_statistics_init_defaults();
        return false;
    }

    return true;
}

static bool device_statistics_save(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    if (!s_device_stats.initialized) {
        return false;
    }

    s_device_stats.blob.logical_time_base_ms = device_statistics_now_ms();
    err = nvs_open(DEVICE_STATS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stats nvs open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(nvs_handle, DEVICE_STATS_KEY_PAYLOAD, &s_device_stats.blob, sizeof(s_device_stats.blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGW(TAG, "stats nvs full, erase old payload and retry, blobSize=%u",
                 (unsigned int)sizeof(s_device_stats.blob));
        if (nvs_erase_key(nvs_handle, DEVICE_STATS_KEY_PAYLOAD) == ESP_OK &&
            nvs_commit(nvs_handle) == ESP_OK) {
            err = nvs_set_blob(nvs_handle, DEVICE_STATS_KEY_PAYLOAD, &s_device_stats.blob, sizeof(s_device_stats.blob));
            if (err == ESP_OK) {
                err = nvs_commit(nvs_handle);
            }
        }
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stats nvs save failed: %s", esp_err_to_name(err));
        return false;
    }

    s_device_stats.dirty = false;
    return true;
}

static bool device_statistics_erase_nvs_data(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(DEVICE_STATS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stats erase open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_erase_key(nvs_handle, DEVICE_STATS_KEY_PAYLOAD);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stats erase failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool device_statistics_start_session(device_stats_op_t op,
                                            bool local_state_session,
                                            app_state_t local_state,
                                            uint8_t drink_id,
                                            const char *formula_name)
{
    if (op == DEVICE_STATS_OP_NONE) {
        return false;
    }

    device_statistics_reset_session();
    s_session.active = true;
    s_session.local_state_session = local_state_session;
    s_session.local_state = local_state;
    s_session.op = op;
    s_session.drink_id = drink_id;
    s_session.start_powder_weight = machine_status.powder_weight;
    s_session.start_liquid_weight = machine_status.liquid_weight;
    s_session.start_ms = device_statistics_now_ms();
    snprintf(s_session.formula_name,
             sizeof(s_session.formula_name),
             "%s",
             (formula_name && formula_name[0] != 0) ? formula_name : device_statistics_drink_name(drink_id));

    ESP_LOGI(TAG, "stats session start op=%d local=%d state=%d drinkId=%u formula=%s powder=%.1f liquid=%.1f",
             (int)op,
             local_state_session ? 1 : 0,
             (int)local_state,
             (unsigned int)drink_id,
             s_session.formula_name,
             (double)s_session.start_powder_weight,
             (double)s_session.start_liquid_weight);
    return true;
}

static void device_statistics_add_beverage_event(uint8_t drink_id, int64_t now_ms)
{
    uint32_t absolute_day_index;
    uint16_t day_index;
    int slot;

    if (drink_id >= DEVICE_STATS_DRINK_KIND_COUNT) {
        return;
    }

    s_device_stats.blob.beverage_total++;
    absolute_day_index = (uint32_t)(now_ms / DEVICE_STATS_DAY_MS);
    if (absolute_day_index >= DEVICE_STATS_INVALID_DAY_INDEX) {
        absolute_day_index %= DEVICE_STATS_INVALID_DAY_INDEX;
    }
    day_index = (uint16_t)absolute_day_index;
    slot = (int)(day_index % DEVICE_STATS_BUCKET_COUNT);

    if (s_device_stats.blob.beverage_buckets[slot].day_index != day_index) {
        memset(&s_device_stats.blob.beverage_buckets[slot], 0, sizeof(s_device_stats.blob.beverage_buckets[slot]));
        s_device_stats.blob.beverage_buckets[slot].day_index = day_index;
    }

    s_device_stats.blob.beverage_buckets[slot].total++;
    s_device_stats.blob.beverage_buckets[slot].drink_count[drink_id]++;
    s_device_stats.blob.backwash_count++;
    s_device_stats.blob.descaling_water_count++;
}

static void device_statistics_report_updated(const char *reason)
{
    mqtt_schedule_device_status_sections_report(
        MQTT_DEVICE_STATUS_SECTION_STATISTICS,
        reason,
        0U
    );
}

static void device_statistics_finish_beverage_success(void)
{
    float delta = machine_status.liquid_weight - s_session.start_liquid_weight;
    int64_t now_ms = device_statistics_now_ms();

    if (delta < 0.0f) {
        delta = 0.0f;
    }

    s_device_stats.blob.total_extraction += delta;
    device_statistics_add_beverage_event(s_session.drink_id, now_ms);
    device_statistics_mark_dirty();
    device_statistics_save();

    ESP_LOGI(TAG, "stats beverage success drinkId=%u formula=%s deltaLiquid=%.1f totalExtraction=%.1f beverageTotal=%u",
             (unsigned int)s_session.drink_id,
             s_session.formula_name,
             (double)delta,
             (double)s_device_stats.blob.total_extraction,
             (unsigned int)s_device_stats.blob.beverage_total);
    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_STATISTICS,
                                                "beverage_statistics_updated",
                                                0U);
    device_statistics_reset_session();
}

static void device_statistics_finish_water_success(void)
{
    float delta = machine_status.liquid_weight - s_session.start_liquid_weight;

    if (delta < 0.0f) {
        delta = 0.0f;
    }

    s_device_stats.blob.total_water += delta;
    device_statistics_mark_dirty();
    device_statistics_save();

    ESP_LOGI(TAG, "stats water success delta=%.1f totalWater=%.1f",
             (double)delta,
             (double)s_device_stats.blob.total_water);
    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_STATISTICS,
                                            "water_statistics_updated",
                                            0U);             
    device_statistics_reset_session();
}

static void device_statistics_finish_grind_success(void)
{
    float delta = machine_status.powder_weight - s_session.start_powder_weight;

    if (delta < 0.0f) {
        delta = 0.0f;
    }

    s_device_stats.blob.total_grind += delta;
    device_statistics_mark_dirty();
    device_statistics_save();

    ESP_LOGI(TAG, "stats grind success delta=%.1f totalGrind=%.1f",
             (double)delta,
             (double)s_device_stats.blob.total_grind);
    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_STATISTICS,
                                            "grind_statistics_updated",
                                            0U);                  
    device_statistics_reset_session();
}

static void device_statistics_finish_steam_success(void)
{
    int64_t duration_ms = device_statistics_now_ms() - s_session.start_ms;

    if (duration_ms >= DEVICE_STATS_STEAM_MIN_MS) {
        s_device_stats.blob.steam_time_sec += (uint32_t)(duration_ms / 1000LL);
        s_device_stats.blob.steam_clean_count++;
        device_statistics_mark_dirty();
        device_statistics_save();
        ESP_LOGI(TAG, "stats steam success duration=%lldms totalSteam=%us steamCount=%u",
                 duration_ms,
                 (unsigned int)s_device_stats.blob.steam_time_sec,
                 (unsigned int)s_device_stats.blob.steam_clean_count);
    } else {
        ESP_LOGI(TAG, "stats steam ignored because duration=%lldms < %lldms",
                 duration_ms,
                 DEVICE_STATS_STEAM_MIN_MS);
    }
    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_STATISTICS,
                                            "steam_statistics_updated",
                                            0U);     
    device_statistics_reset_session();
}

static void device_statistics_finish_maintain_success(app_state_t state)
{
    switch (state) {
    case ST_MAINT_BREW:
        s_device_stats.blob.maintain_brew_count++;
        s_device_stats.blob.backwash_count = 0;
        break;
    case ST_MAINT_DES:
        s_device_stats.blob.descaling_count++;
        s_device_stats.blob.descaling_water_count = 0;
        break;
    case ST_MAINT_STEAM:
        s_device_stats.blob.maintain_steam_count++;
        s_device_stats.blob.steam_clean_count = 0;
        break;
    default:
        return;
    }

    device_statistics_mark_dirty();
    device_statistics_save();
    ESP_LOGI(TAG,
             "stats maintain success state=%d brewClean=%u descaling=%u steamClean=%u backwashCount=%u steamCount=%u descalingWater=%u",
             (int)state,
             (unsigned int)s_device_stats.blob.maintain_brew_count,
             (unsigned int)s_device_stats.blob.descaling_count,
             (unsigned int)s_device_stats.blob.maintain_steam_count,
             (unsigned int)s_device_stats.blob.backwash_count,
             (unsigned int)s_device_stats.blob.steam_clean_count,
             (unsigned int)s_device_stats.blob.descaling_water_count);
    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_STATISTICS,
                                            "maintain_statistics_updated",
                                            0U);                  
}

static void device_statistics_try_finish(device_stats_op_t op)
{
    switch (op) {
    case DEVICE_STATS_OP_BEVERAGE:
        device_statistics_finish_beverage_success();
        break;
    case DEVICE_STATS_OP_WATER:
        device_statistics_finish_water_success();
        break;
    case DEVICE_STATS_OP_STEAM:
        device_statistics_finish_steam_success();
        break;
    case DEVICE_STATS_OP_GRIND:
        device_statistics_finish_grind_success();
        break;
    default:
        device_statistics_reset_session();
        break;
    }
}

static bool device_statistics_is_active_signal(device_stats_op_t op, const MACHINE_STATUS *status)
{
    if (!status) {
        return false;
    }

    switch (op) {
    case DEVICE_STATS_OP_BEVERAGE:
        return (status->drink_making_flg != DRINK_MAKER_NONE) || (status->hot_water_flg != 0U);
    case DEVICE_STATS_OP_WATER:
        return status->hot_water_flg != 0U;
    case DEVICE_STATS_OP_STEAM:
        return status->steam_flag == STEAM_RUNNING;
    case DEVICE_STATS_OP_GRIND:
        return status->grind_run_flg == 1U;
    default:
        return false;
    }
}

static int64_t device_statistics_finish_idle_ms(device_stats_op_t op)
{
    switch (op) {
    case DEVICE_STATS_OP_BEVERAGE:
        return DEVICE_STATS_BEVERAGE_IDLE_MS;
    case DEVICE_STATS_OP_WATER:
        return DEVICE_STATS_WATER_IDLE_MS;
    case DEVICE_STATS_OP_STEAM:
        return DEVICE_STATS_STEAM_IDLE_MS;
    case DEVICE_STATS_OP_GRIND:
        return DEVICE_STATS_GRIND_IDLE_MS;
    default:
        return 0;
    }
}

static void device_statistics_aggregate_beverage_period(int period_index, beverage_period_data_t *period_data)
{
    int items = 0;
    uint32_t drink_count[DEVICE_STATS_DRINK_KIND_COUNT] = {0};
    uint32_t now_day = (uint32_t)(device_statistics_now_ms() / DEVICE_STATS_DAY_MS);
    int period_days = s_beverage_period_days[period_index];

    memset(period_data, 0, sizeof(*period_data));
    period_data->period = period_index;
    period_data->data = s_beverage_period_items[period_index];

    for (int i = 0; i < DEVICE_STATS_BUCKET_COUNT; i++) {
        const beverage_day_bucket_t *bucket = &s_device_stats.blob.beverage_buckets[i];
        if (bucket->day_index == DEVICE_STATS_INVALID_DAY_INDEX) {
            continue;
        }

        if (period_days > 0) {
            int64_t age_days = (int64_t)now_day - (int64_t)bucket->day_index;
            if (age_days < 0 || age_days >= period_days) {
                continue;
            }
        }

        for (int drink_id = 0; drink_id < DEVICE_STATS_DRINK_KIND_COUNT; drink_id++) {
            drink_count[drink_id] += bucket->drink_count[drink_id];
        }
    }

    for (int drink_id = 0; drink_id < DEVICE_STATS_DRINK_KIND_COUNT; drink_id++) {
        if (drink_count[drink_id] == 0U) {
            continue;
        }

        snprintf(period_data->data[items].formula_name,
                 sizeof(period_data->data[items].formula_name),
                 "%s",
                 device_statistics_drink_name((uint8_t)drink_id));
        period_data->data[items].drink_id = drink_id;
        period_data->data[items].drink_count = (int)drink_count[drink_id];
        items++;
    }

    period_data->data_count = items;
}

bool device_statistics_store_init(void)
{
    device_statistics_init_defaults();
    device_statistics_load();
    device_statistics_reset_session();
    s_device_stats.initialized = true;
    s_device_stats.dirty = false;
    ESP_LOGI(TAG, "device statistics init done: totalGrind=%.1f totalExtraction=%.1f steamTime=%us totalWater=%.1f beverageTotal=%u",
             (double)s_device_stats.blob.total_grind,
             (double)s_device_stats.blob.total_extraction,
             (unsigned int)s_device_stats.blob.steam_time_sec,
             (double)s_device_stats.blob.total_water,
             (unsigned int)s_device_stats.blob.beverage_total);
    return true;
}

bool device_statistics_factory_reset(void)
{
    device_statistics_init_defaults();
    device_statistics_reset_session();
    s_device_stats.initialized = true;
    s_device_stats.dirty = false;

    if (!device_statistics_erase_nvs_data()) {
        return false;
    }

    ESP_LOGW(TAG, "device statistics reset to factory defaults");
    return true;
}

void device_statistics_notify_local_state_start(app_state_t state)
{
    device_stats_op_t op = device_statistics_state_to_op(state);

    if (op == DEVICE_STATS_OP_NONE) {
        return;
    }

    if (s_session.active && s_session.local_state_session && s_session.local_state == state) {
        return;
    }

    device_statistics_start_session(op,
                                    true,
                                    state,
                                    device_statistics_state_to_drink_id(state),
                                    NULL);
}

void device_statistics_notify_local_state_cancel(app_state_t state)
{
    if (!s_session.active || !s_session.local_state_session || s_session.local_state != state) {
        return;
    }

    ESP_LOGI(TAG, "stats session cancel local state=%d machineStarted=%d", (int)state, s_session.machine_started ? 1 : 0);
    device_statistics_reset_session();
}

void device_statistics_notify_remote_action_start(control_action_t action, const formula_info_t *formula)
{
    device_stats_op_t op = DEVICE_STATS_OP_NONE;
    uint8_t drink_id = DRINK_ID_MASTER;
    const char *formula_name = NULL;

    switch (action) {
    case CTRL_ACT_ESPRESSO:
    case CTRL_ACT_AMERICANO_BREW:
    case CTRL_ACT_COLD_BREW:
        op = DEVICE_STATS_OP_BEVERAGE;
        drink_id = formula ? formula->drink_id : device_statistics_action_drink_id(action);
        formula_name = formula ? formula->formula_name : NULL;
        break;
    case CTRL_ACT_HOT_WATER:
        op = DEVICE_STATS_OP_WATER;
        break;
    case CTRL_ACT_STEAM_START:
        op = DEVICE_STATS_OP_STEAM;
        break;
    case CTRL_ACT_GRIND_START:
        op = DEVICE_STATS_OP_GRIND;
        break;
    default:
        return;
    }

    device_statistics_start_session(op,
                                    false,
                                    ST_READY,
                                    drink_id,
                                    formula_name);
}

void device_statistics_notify_remote_cancel(void)
{
    if (!s_session.active || s_session.local_state_session) {
        return;
    }

    ESP_LOGI(TAG, "stats session cancel remote op=%d machineStarted=%d", (int)s_session.op, s_session.machine_started ? 1 : 0);
    device_statistics_reset_session();
}

void device_statistics_notify_maintain_success(app_state_t state)
{
    if (!s_device_stats.initialized) {
        return;
    }

    device_statistics_finish_maintain_success(state);
}

void device_statistics_handle_machine_status(const MACHINE_STATUS *status)
{
    bool active_signal;
    int64_t now_ms;

    if (!s_device_stats.initialized || !status || !s_session.active) {
        return;
    }

    active_signal = device_statistics_is_active_signal(s_session.op, status);
    now_ms = device_statistics_now_ms();

    if (active_signal) {
        s_session.machine_started = true;
        s_session.idle_since_ms = 0;
        return;
    }

    if (!s_session.machine_started) {
        return;
    }

    if (s_session.idle_since_ms == 0) {
        s_session.idle_since_ms = now_ms;
        return;
    }

    if ((now_ms - s_session.idle_since_ms) >= device_statistics_finish_idle_ms(s_session.op)) {
        device_statistics_try_finish(s_session.op);
    }
}

bool device_statistics_fill_snapshot(statistics_info_t *statistics)
{
    if (!statistics || !s_device_stats.initialized) {
        return false;
    }

    memset(statistics, 0, sizeof(*statistics));

    statistics->material_statistics.total_grind = device_statistics_round_to_int(s_device_stats.blob.total_grind);
    statistics->material_statistics.total_extraction = device_statistics_round_to_int(s_device_stats.blob.total_extraction);
    statistics->material_statistics.steam_time = (int)s_device_stats.blob.steam_time_sec;
    statistics->material_statistics.total_water = device_statistics_round_to_int(s_device_stats.blob.total_water);
    statistics->maintain_statistics.breawing_head_cleaning_count = (int)s_device_stats.blob.maintain_brew_count;
    statistics->maintain_statistics.descaling_count = (int)s_device_stats.blob.descaling_count;
    statistics->maintain_statistics.steam_pole_cleaning_count = (int)s_device_stats.blob.maintain_steam_count;
    statistics->clean_statistics.back_wash_count = (int)s_device_stats.blob.backwash_count;
    statistics->clean_statistics.back_wash_total_count = DEVICE_STATS_BACKWASH_TOTAL_COUNT;
    statistics->clean_statistics.steam_pole_cleaning_count = (int)s_device_stats.blob.steam_clean_count;
    statistics->clean_statistics.steam_pole_cleaning_total_count = DEVICE_STATS_STEAM_CLEAN_TOTAL;
    statistics->clean_statistics.descaling_water_count = (int)s_device_stats.blob.descaling_water_count;
    statistics->clean_statistics.descaling_water_total_count = DEVICE_STATS_DESCALING_TOTAL_COUNT;

    for (int i = 0; i < DEVICE_STATS_BEVERAGE_PERIOD_COUNT; i++) {
        device_statistics_aggregate_beverage_period(i, &s_beverage_periods[i]);
    }

    statistics->beverage_statistics.data = s_beverage_periods;
    statistics->beverage_statistics.data_count = DEVICE_STATS_BEVERAGE_PERIOD_COUNT;
    statistics->beverage_statistics.total = (int)s_device_stats.blob.beverage_total;

    return true;
}

bool device_statistics_should_raise_notice(maint_type_t type)
{
    if (!s_device_stats.initialized) {
        return false;
    }

    switch (type) {
    case MAINT_TYPE_BREW:
        return s_device_stats.blob.backwash_count >= DEVICE_STATS_BACKWASH_TOTAL_COUNT;
    case MAINT_TYPE_DES:
        return s_device_stats.blob.descaling_water_count >= DEVICE_STATS_DESCALING_TOTAL_COUNT;
    case MAINT_TYPE_STEAM:
        return s_device_stats.blob.steam_clean_count >= DEVICE_STATS_STEAM_CLEAN_TOTAL;
    default:
        return false;
    }
}
