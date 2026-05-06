#include "sp_pro_app_context.h"
#include "sp_pro_app_types.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_state.h"
#include "sp_pro_app.h"
#include "device_statistics_store.h"
#include "formula_store.h"
#include "mqtt_protocol.h"
#include "system_runtime.h"
#include <stddef.h>
#include "esp_log.h"

static const char *TAG = "state_setting";
extern FLASH_FACTORY_DATA factory_data;

#define SETTING_IDLE_TIMEOUT_TICKS STAY_TICKS(600000)
#define SETTING_PARAM_FLAG_TEMP              (1U << 0)
#define SETTING_PARAM_FLAG_SINGLE_CLICK_EXIT (1U << 1)
#define SETTING_PARAM_FLAG_DISCRETE          (1U << 2)

typedef struct {
    encoder_mode_t mode;
    float min;
    float max;
    float step;
    uint8_t flags;
} setting_param_desc_t;

static const setting_param_desc_t g_params[] = {
    { ENCODER_MODE_ESP_BREW_WEIGHT,  1.0f,   60.0f, 1.0f, 0U },
    { ENCODER_MODE_ESP_BREW_TEMP,    80.0f,  95.0f, 1.0f, SETTING_PARAM_FLAG_TEMP },
    { ENCODER_MODE_AME_BREW_WEIGHT,  1.0f,   60.0f, 1.0f, 0U },
    { ENCODER_MODE_AME_WATER_WEIGHT, 5.0f,   400.0f, 5.0f, 0U },
    { ENCODER_MODE_AME_BREW_TEMP,    40.0f,  95.0f, 1.0f, SETTING_PARAM_FLAG_TEMP },
    { ENCODER_MODE_AME_WATER_TEMP,   50.0f,  95.0f, 5.0f, SETTING_PARAM_FLAG_TEMP },
    { ENCODER_MODE_COLD_BREW_WEIGHT, 1.0f,   200.0f, 1.0f, SETTING_PARAM_FLAG_SINGLE_CLICK_EXIT },
    { ENCODER_MODE_HOT_WATER_WEIGHT, 5.0f,   400.0f, 5.0f, 0U },
    { ENCODER_MODE_HOT_WATER_TEMP,   40.0f,  95.0f, 5.0f, SETTING_PARAM_FLAG_TEMP },
    { ENCODER_MODE_GRIND_WEIGHT,     1.0f,   30.0f, 0.1f, SETTING_PARAM_FLAG_SINGLE_CLICK_EXIT },
    { ENCODER_MODE_CLEAN_VOLUME,     15.0f,  100.0f, 1.0f, SETTING_PARAM_FLAG_SINGLE_CLICK_EXIT },
    { ENCODER_MODE_STEAM_LEVEL,       1.0f,  2.0f, 1.0f, SETTING_PARAM_FLAG_SINGLE_CLICK_EXIT | SETTING_PARAM_FLAG_DISCRETE },
};

static voice_id_t setting_current_water_in_voice(setting_water_in_t mode)
{
    switch (mode) {
    case WATER_IN_MODE_TANK:
        return VOICE_CURRENTWATERTANKMODE;

    case WATER_IN_MODE_BUCKET:
        return VOICE_CURRENTWATERBUCKETMODE;

    default:
        return VOICE_NONE;
    }
}

static voice_id_t setting_switch_water_in_voice(setting_water_in_t mode)
{
    switch (mode) {
    case WATER_IN_MODE_TANK:
        return VOICE_WATERTANKMODE;

    case WATER_IN_MODE_BUCKET:
        return VOICE_WATERBUCKETMODE;

    default:
        return VOICE_NONE;
    }
}

static voice_id_t setting_current_hardness_voice(setting_water_hardness_t hardness)
{
    switch (hardness) {
    case HARDNESS_LEVEL_A:
        return VOICE_WATERHARDNESSLEVELA;

    case HARDNESS_LEVEL_B:
        return VOICE_WATERHARDNESSLEVELB;

    case HARDNESS_LEVEL_C:
        return VOICE_WATERHARDNESSLEVELC;

    default:
        return VOICE_NONE;
    }
}

static voice_id_t setting_switch_hardness_voice(setting_water_hardness_t hardness)
{
    switch (hardness) {
    case HARDNESS_LEVEL_A:
        return VOICE_SWITCHWATERHARDNESSLEVELA;

    case HARDNESS_LEVEL_B:
        return VOICE_SWITCHWATERHARDNESSLEVELB;

    case HARDNESS_LEVEL_C:
        return VOICE_SWITCHWATERHARDNESSLEVELC;

    default:
        return VOICE_NONE;
    }
}

static void setting_sync_water_in_mode(setting_water_in_t mode, bool push_ctr)
{
    int water_supply_mode;

    if (mode == WATER_IN_MODE_BUCKET) {
        water_supply_mode = 1;
    } else if (mode == WATER_IN_MODE_TANK) {
        water_supply_mode = 0;
    } else {
        return;
    }

    factory_data.water_supply_mode = water_supply_mode;
    mqtt_sync_runtime_setting_from_device();

    ESP_LOGI(TAG,
             "Sync water-in mode local=%u factory=%d push_ctr=%d state=%d "
             "ctr_status=%u shortage=%u error=%u",
             (unsigned)mode,
             water_supply_mode,
             push_ctr ? 1 : 0,
             (int)g_ctx.core.state,
             (unsigned)g_ctx.ms.ctr_status,
             (unsigned)g_ctx.ms.water_box_shortage_flag,
             (unsigned)g_ctx.ms.error_code);

    if (push_ctr && !ctr_cmd_action(CTRL_ACT_FACTORY_WRITE, NULL)) {
        ESP_LOGW(TAG,
                 "Water-in mode factory write rejected local=%u factory=%d "
                 "ctr_status=%u shortage=%u error=%u",
                 (unsigned)mode,
                 water_supply_mode,
                 (unsigned)g_ctx.ms.ctr_status,
                 (unsigned)g_ctx.ms.water_box_shortage_flag,
                 (unsigned)g_ctx.ms.error_code);
    }
}

static const encoder_mode_t espresso_modes[] = {
    ENCODER_MODE_ESP_BREW_WEIGHT,
    ENCODER_MODE_ESP_BREW_TEMP,
};

static const encoder_mode_t americano_modes[] = {
    ENCODER_MODE_AME_BREW_WEIGHT,
    ENCODER_MODE_AME_WATER_WEIGHT,
    ENCODER_MODE_AME_BREW_TEMP,
    ENCODER_MODE_AME_WATER_TEMP,
};

static const encoder_mode_t cold_modes[] = {
    ENCODER_MODE_COLD_BREW_WEIGHT,
};

static const encoder_mode_t water_modes[] = {
    ENCODER_MODE_HOT_WATER_WEIGHT,
    ENCODER_MODE_HOT_WATER_TEMP,
};

static const encoder_mode_t grind_modes[] = {
    ENCODER_MODE_GRIND_WEIGHT,
};

static const encoder_mode_t clean_modes[] = {
    ENCODER_MODE_CLEAN_VOLUME,
};

static const encoder_mode_t steam_modes[] = {
    ENCODER_MODE_STEAM_LEVEL,
};

const sp_pro_param_map_t g_param_map[] = {
    [KEY_ESPRESSO]  = { .count = 2, .modes = espresso_modes },
    [KEY_AMERICANO] = { .count = 4, .modes = americano_modes },
    [KEY_COLD_BREW] = { .count = 1, .modes = cold_modes },
    [KEY_WATER]     = { .count = 2, .modes = water_modes },
    [KEY_STEAM]     = { .count = 1, .modes = steam_modes },
    [KEY_GRIND]     = { .count = 1, .modes = grind_modes },
    [KEY_CLEAN]     = { .count = 1, .modes = clean_modes },
};

static void setting_commit_formula_if_needed(app_ctx_t *ctx);

static const setting_param_desc_t *find_param_desc(encoder_mode_t mode)
{
    for (size_t i = 0; i < sizeof(g_params) / sizeof(g_params[0]); i++) {
        if (g_params[i].mode == mode) {
            return &g_params[i];
        }
    }
    return NULL;
}

static bool setting_param_has_flag(const setting_param_desc_t *param_desc, uint8_t flag)
{
    return param_desc && ((param_desc->flags & flag) != 0U);
}

static float setting_param_normalize_value(const setting_param_desc_t *param_desc, float value)
{
    float normalized;
    int step_index;

    if (!param_desc) {
        return value;
    }

    if (value < param_desc->min) {
        value = param_desc->min;
    }
    if (value > param_desc->max) {
        value = param_desc->max;
    }

    if (param_desc->step <= 0.0f) {
        return value;
    }

    normalized = (value - param_desc->min) / param_desc->step;
    step_index = (int)(normalized + 0.5f);
    value = param_desc->min + ((float)step_index * param_desc->step);

    if (value < param_desc->min) {
        value = param_desc->min;
    }
    if (value > param_desc->max) {
        value = param_desc->max;
    }

    return value;
}

static float *setting_param_value_ptr(app_ctx_t *ctx, encoder_mode_t mode)
{
    if (!ctx) {
        return NULL;
    }

    switch (mode) {
    case ENCODER_MODE_ESP_BREW_WEIGHT:
        return &ctx->setting.esp_brew_w;
    case ENCODER_MODE_ESP_BREW_TEMP:
        return &ctx->setting.esp_brew_t;
    case ENCODER_MODE_AME_BREW_WEIGHT:
        return &ctx->setting.ame_brew_w;
    case ENCODER_MODE_AME_WATER_WEIGHT:
        return &ctx->setting.ame_water_w;
    case ENCODER_MODE_AME_BREW_TEMP:
        return &ctx->setting.ame_brew_t;
    case ENCODER_MODE_AME_WATER_TEMP:
        return &ctx->setting.ame_water_t;
    case ENCODER_MODE_COLD_BREW_WEIGHT:
        return &ctx->setting.cold_brew_w;
    case ENCODER_MODE_HOT_WATER_WEIGHT:
        return &ctx->setting.hot_water_w;
    case ENCODER_MODE_HOT_WATER_TEMP:
        return &ctx->setting.hot_water_t;
    case ENCODER_MODE_GRIND_WEIGHT:
        return &ctx->setting.grind_w;
    case ENCODER_MODE_CLEAN_VOLUME:
        return &ctx->setting.clean_v;
    case ENCODER_MODE_STEAM_LEVEL:
        return &ctx->setting.steam_level;
    case ENCODER_MODE_IDLE:
    case ENCODER_MODE_MAX:
    default:
        return NULL;
    }
}

static float store_param_get_value(app_ctx_t *ctx, encoder_mode_t mode)
{
    float *value = setting_param_value_ptr(ctx, mode);

    if (!value) {
        return 0.0f;
    }
    return *value;
}

static encoder_mode_t setting_sync_current_mode(app_ctx_t *ctx)
{
    encoder_mode_t mode;

    if (!ctx) {
        return ENCODER_MODE_IDLE;
    }

    mode = current_encoder_mode_for_ctx(ctx);
    ctx->setting.current_mode = mode;
    return mode;
}

static void setting_focus_current_param(app_ctx_t *ctx, setting_ui_phase_t phase)
{
    encoder_mode_t mode;
    float value;

    if (!ctx) {
        return;
    }

    mode = setting_sync_current_mode(ctx);
    value = store_param_get_value(ctx, mode);
    ctx->setting.current_val = value;
    ctx->setting.target_val = value;
    ctx->setting.ui_phase = phase;
}

encoder_mode_t current_encoder_mode_for_ctx(app_ctx_t *ctx)
{
    const sp_pro_param_map_t *map;

    if (!ctx) {
        return ENCODER_MODE_IDLE;
    }

    map = &g_param_map[ctx->setting.drink_type];
    if (map->count == 0 || !map->modes) {
        return ENCODER_MODE_IDLE;
    }

    if (ctx->setting.param_index >= map->count) {
        ctx->setting.param_index = 0;
    }

    return map->modes[ctx->setting.param_index];
}

float get_cur_param_value_for_ctx(app_ctx_t *ctx, encoder_mode_t mode)
{
    return store_param_get_value(ctx, mode);
}

bool mode_is_temp(encoder_mode_t mode)
{
    return setting_param_has_flag(find_param_desc(mode), SETTING_PARAM_FLAG_TEMP);
}

static void setting_exit(app_ctx_t *ctx)
{
    if (!ctx || !ctx->setting.active) {
        return;
    }

    setting_commit_formula_if_needed(ctx);
    ctx->setting.active = false;
    ctx->setting.formula_dirty = false;
    ESP_LOGI(TAG, "Exit drink setting");
}

void setting_send_param(app_ctx_t *ctx)
{
    encoder_mode_t mode;

    if (!ctx) {
        return;
    }

    mode = setting_sync_current_mode(ctx);
    if (mode == ENCODER_MODE_IDLE) {
        return;
    }

    ctr_cmd_param(mode);
}

static const setting_param_desc_t *setting_current_param_desc(app_ctx_t *ctx)
{
    encoder_mode_t mode = setting_sync_current_mode(ctx);
    return find_param_desc(mode);
}

static float *setting_current_param_value(app_ctx_t *ctx)
{
    encoder_mode_t mode = setting_sync_current_mode(ctx);
    return setting_param_value_ptr(ctx, mode);
}

static inline void setting_enter_adjust(app_ctx_t *ctx, float val)
{
    ctx->setting.current_val = val;
    ctx->setting.target_val = val;
    ctx->setting.ui_phase = SET_UI_PHASE_ADJUST;
}

static inline void setting_enter_hint(app_ctx_t *ctx, float val)
{
    ctx->setting.target_val = val;
    ctx->setting.current_val = val;
    ctx->setting.ui_phase = SET_UI_PHASE_HINT;
}

static inline void setting_exit_and_static(app_ctx_t *ctx, float val)
{
    ctx->setting.target_val = val;
    ctx->setting.current_val = val;
    ctx->setting.ui_phase = SET_UI_PHASE_STATIC;
    setting_exit(ctx);
}

static void setting_snapshot_encoder_status(app_ctx_t *ctx, const MACHINE_STATUS *status)
{
    if (!ctx || !status) {
        return;
    }

    ctx->setting.last_enc_seq = status->encoder.evt_seq;
    ctx->setting.last_enc_active = status->encoder.active;
    ctx->setting.last_enc_param_id = status->encoder.param_id;
    ctx->setting.last_enc_evt_type = status->encoder.evt_type;
    ctx->setting.last_enc_rotate = status->encoder.rotate;
    ctx->setting.last_enc_value = status->encoder.cur_value;
}

static bool setting_encoder_event_changed_without_seq(app_ctx_t *ctx,
                                                      const MACHINE_STATUS *status)
{
    if (!ctx || !status) {
        return false;
    }

    if (status->encoder.evt_type == ENC_EVT_NONE) {
        return false;
    }

    if (status->encoder.active != ctx->setting.last_enc_active ||
        status->encoder.param_id != ctx->setting.last_enc_param_id ||
        status->encoder.evt_type != ctx->setting.last_enc_evt_type) {
        return true;
    }

    switch (status->encoder.evt_type) {
    case ENC_EVT_ROTATE:
        return status->encoder.rotate != ctx->setting.last_enc_rotate ||
               status->encoder.cur_value != ctx->setting.last_enc_value;

    case ENC_EVT_CLICK:
        return status->encoder.cur_value != ctx->setting.last_enc_value;

    case ENC_EVT_NONE:
    default:
        return false;
    }
}

static bool setting_consume_encoder_event(app_ctx_t *ctx,
                                          const MACHINE_STATUS *status,
                                          encoder_event_t *evt_type)
{
    if (!ctx || !status || !evt_type) {
        return false;
    }

    if (status->encoder.evt_seq == ctx->setting.last_enc_seq &&
        !setting_encoder_event_changed_without_seq(ctx, status)) {
        return false;
    }

    if (status->encoder.evt_seq == ctx->setting.last_enc_seq) {
        ESP_LOGW(TAG,
                 "Consume encoder event without seq bump: mode=%u evt=%d rotate=%d value=%.1f",
                 (unsigned)status->encoder.param_id,
                 (int)status->encoder.evt_type,
                 (int)status->encoder.rotate,
                 status->encoder.cur_value);
    }

    setting_snapshot_encoder_status(ctx, status);
    *evt_type = status->encoder.evt_type;
    return true;
}

static void setting_handle_rotate(app_ctx_t *ctx, const MACHINE_STATUS *status)
{
    const setting_param_desc_t *param_desc = setting_current_param_desc(ctx);
    float *param_value = setting_current_param_value(ctx);
    encoder_mode_t current_mode = setting_sync_current_mode(ctx);
    float old_value;
    float val;

    if (!param_desc || !param_value || !status) {
        return;
    }

    if (!status->encoder.active) {
        ESP_LOGW(TAG, "Ignore encoder rotate: controller encoder inactive");
        return;
    }

    if (status->encoder.param_id != (uint8_t)current_mode) {
        ESP_LOGW(TAG,
                 "Ignore encoder rotate: param mismatch current=%u reported=%u",
                 (unsigned)current_mode,
                 (unsigned)status->encoder.param_id);
        return;
    }

    old_value = *param_value;
    if (status->encoder.rotate != ENC_ROT_NONE) {
        val = old_value;
        if (status->encoder.rotate == ENC_ROT_CW) {
            val += param_desc->step;
        } else if (status->encoder.rotate == ENC_ROT_CCW) {
            val -= param_desc->step;
        }
        val = setting_param_normalize_value(param_desc, val);
        ESP_LOGI(TAG,
                 "Rotate mode=%u by direction reported=%.1f local=%.1f -> %.1f",
                 (unsigned)current_mode,
                 status->encoder.cur_value,
                 old_value,
                 val);
    } else {
        val = setting_param_normalize_value(param_desc, status->encoder.cur_value);
    }
    *param_value = val;
    if (old_value != val) {
        ctx->setting.formula_dirty = true;
    }
    ESP_LOGI(TAG, "Rotate mode=%u value=%.1f", (unsigned)current_mode, *param_value);
    setting_enter_adjust(ctx, val);
}

static void setting_commit_formula_if_needed(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->setting.formula_dirty) {
        if (!formula_store_sync_local_setting(ctx->setting.drink_type)) {
            ESP_LOGW(TAG, "Skip formulaOverall sync for drink_type=%u", ctx->setting.drink_type);
        } else {
            mqtt_report_device_status();
        }
        ctx->setting.formula_dirty = false;
    }

    sp_pro_app_save_settings();
}

static void setting_handle_click(app_ctx_t *ctx, const MACHINE_STATUS *status)
{
    const sp_pro_param_map_t *map = &g_param_map[ctx->setting.drink_type];
    const setting_param_desc_t *param_desc = setting_current_param_desc(ctx);
    float current_value;

    (void)status;

    if (!ctx || !param_desc) {
        return;
    }

    current_value = store_param_get_value(ctx, setting_sync_current_mode(ctx));
    ESP_LOGI(TAG, "Click mode=%u value=%.1f", (unsigned)ctx->setting.current_mode, current_value);
    setting_commit_formula_if_needed(ctx);

    if (setting_param_has_flag(param_desc, SETTING_PARAM_FLAG_SINGLE_CLICK_EXIT) ||
        map->count <= 1U) {
        setting_exit_and_static(ctx, current_value);
        return;
    }

    if (ctx->setting.param_index < map->count - 1U) {
        ctx->setting.param_index++;
        setting_focus_current_param(ctx, SET_UI_PHASE_HINT);
        setting_send_param(ctx);
        return;
    }

    setting_exit_and_static(ctx, current_value);
}

static void setting_handle_encoder_event(app_ctx_t *ctx, const MACHINE_STATUS *status)
{
    encoder_event_t evt_type;

    if (!ctx || !status || !ctx->setting.active) {
        return;
    }

    if (!setting_consume_encoder_event(ctx, status, &evt_type)) {
        return;
    }

    ctx->setting.idle_timer = 0;

    switch (evt_type) {
    case ENC_EVT_ROTATE:
        setting_handle_rotate(ctx, status);
        break;
    case ENC_EVT_CLICK:
        setting_handle_click(ctx, status);
        break;
    default:
        break;
    }
}

static void setting_reset_system_runtime(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.setting.water_in_entered = false;
    ctx->state_runtime.setting.water_hardness_entered = false;
    ctx->state_runtime.setting.factory_reset_entered = false;
    ctx->state_runtime.setting.factory_reset_step_tick = 0U;
    ctx->state_runtime.setting.factory_reset_prompt_voice_count = 0U;
    ctx->state_runtime.setting.factory_reset_done_voice_count = 0U;
}

static bool setting_select_temp_param(app_ctx_t *ctx)
{
    const sp_pro_param_map_t *map = &g_param_map[ctx->setting.drink_type];

    for (uint8_t i = 0; i < map->count; i++) {
        if (setting_param_has_flag(find_param_desc(map->modes[i]), SETTING_PARAM_FLAG_TEMP)) {
            ctx->setting.param_index = i;
            setting_focus_current_param(ctx, SET_UI_PHASE_HINT);
            return true;
        }
    }

    return false;
}

static bool setting_select_non_temp_param(app_ctx_t *ctx)
{
    const sp_pro_param_map_t *map = &g_param_map[ctx->setting.drink_type];

    for (uint8_t i = 0; i < map->count; i++) {
        if (!setting_param_has_flag(find_param_desc(map->modes[i]), SETTING_PARAM_FLAG_TEMP)) {
            ctx->setting.param_index = i;
            setting_focus_current_param(ctx, SET_UI_PHASE_HINT);
            return true;
        }
    }

    return false;
}

app_state_t state_handle_drink_set(app_ctx_t *ctx)
{
    if (!ctx->setting.active) {
        return ST_READY;
    }

    if (++ctx->setting.idle_timer >= SETTING_IDLE_TIMEOUT_TICKS) {
        setting_exit(ctx);
        return ST_READY;
    }

    if (ctx->input.key_long & ((1U << KEY_ESPRESSO) |
                               (1U << KEY_AMERICANO) |
                               (1U << KEY_COLD_BREW) |
                               (1U << KEY_WATER) |
                               (1U << KEY_STEAM) |
                               (1U << KEY_GRIND) |
                               (1U << KEY_CLEAN))) {
        uint16_t drink_mask = ctx->input.key_long & ((1U << KEY_ESPRESSO) |
                                                     (1U << KEY_AMERICANO) |
                                                     (1U << KEY_COLD_BREW) |
                                                     (1U << KEY_WATER) |
                                                     (1U << KEY_STEAM) |
                                                     (1U << KEY_GRIND) |
                                                     (1U << KEY_CLEAN));
        uint16_t current_key_mask = (1U << ctx->setting.drink_type);

        ctx->input.key_long &= ~drink_mask;
        if (drink_mask & current_key_mask) {
            setting_exit(ctx);
            return ST_READY;
        }
    }

    setting_handle_encoder_event(ctx, &ctx->ms);

    if (ctx->input.key_pressed & (1U << ctx->setting.drink_type)) {
        ctx->input.key_pressed &= ~(1U << ctx->setting.drink_type);
        if (setting_select_non_temp_param(ctx)) {
            ctx->setting.idle_timer = 0;
            setting_send_param(ctx);
        }
    }

    if (ctx->input.key_pressed & (1U << KEY_TEMP)) {
        ctx->input.key_pressed &= ~(1U << KEY_TEMP);
        if (setting_select_temp_param(ctx)) {
            ctx->setting.idle_timer = 0;
            setting_send_param(ctx);
        }
    }

    return ST_DRINK_SET;
}

static void setting_init_system_page(app_ctx_t *ctx,
                                     bool *entered,
                                     uint8_t initial_value,
                                     encoder_mode_t mode)
{
    if (!ctx || !entered || *entered) {
        return;
    }

    *entered = true;
    setting_snapshot_encoder_status(ctx, &ctx->ms);
    ctx->setting.current_mode = mode;
    ctx->setting.current_val = initial_value;
    ctx->setting.target_val = initial_value;
    ctr_cmd_param(mode);
    ESP_LOGI(TAG, "Activate system encoder mode=%u", (unsigned)mode);
}

static app_state_t setting_handle_water_hardness(app_ctx_t *ctx)
{
    encoder_event_t evt_type;

    if (!ctx) {
        return ST_READY;
    }

    if (!ctx->state_runtime.setting.water_hardness_entered) {
        setting_init_system_page(ctx,
                                 &ctx->state_runtime.setting.water_hardness_entered,
                                 (ctx->setting.hardness <= HARDNESS_LEVEL_C)
                                     ? (uint8_t)ctx->setting.hardness
                                     : (uint8_t)HARDNESS_LEVEL_B,
                                 ENCODER_MODE_WATER_HARDNESS);
        ctx->setting.hardness = (ctx->setting.hardness <= HARDNESS_LEVEL_C)
                                    ? ctx->setting.hardness
                                    : HARDNESS_LEVEL_B;
        voice_manager_play_interrupt(setting_current_hardness_voice(ctx->setting.hardness));
        ESP_LOGI(TAG, "Enter water hardness setting");
    }

    if (!setting_consume_encoder_event(ctx, &ctx->ms, &evt_type)) {
        return ST_SETTING;
    }

    switch (evt_type) {
    case ENC_EVT_ROTATE:
        if (ctx->ms.encoder.rotate == ENC_ROT_CW) {
            ctx->setting.hardness = (setting_water_hardness_t)((ctx->setting.hardness + 1U) % 3U);
        } else if (ctx->ms.encoder.rotate == ENC_ROT_CCW) {
            ctx->setting.hardness = (setting_water_hardness_t)((ctx->setting.hardness + 2U) % 3U);
        }
        ctx->setting.current_val = (float)ctx->setting.hardness;
        ctx->setting.target_val = (float)ctx->setting.hardness;
        voice_manager_play_interrupt(setting_switch_hardness_voice(ctx->setting.hardness));
        break;

    case ENC_EVT_CLICK:
        sp_pro_app_save_settings();
        ctx->state_runtime.setting.water_hardness_entered = false;
        return ST_READY;

    default:
        break;
    }

    return ST_SETTING;
}

static app_state_t setting_handle_water_in(app_ctx_t *ctx)
{
    encoder_event_t evt_type;
    setting_water_in_t prev_mode;

    if (!ctx) {
        return ST_READY;
    }

    if (!ctx->state_runtime.setting.water_in_entered) {
        if (ctx->setting.water_in_mode != WATER_IN_MODE_TANK &&
            ctx->setting.water_in_mode != WATER_IN_MODE_BUCKET) {
            ctx->setting.water_in_mode = WATER_IN_MODE_BUCKET;
        }
        setting_init_system_page(ctx,
                                 &ctx->state_runtime.setting.water_in_entered,
                                 (uint8_t)ctx->setting.water_in_mode,
                                 ENCODER_MODE_WATER_IN);
        voice_manager_play_interrupt(setting_current_water_in_voice(ctx->setting.water_in_mode));
        ESP_LOGI(TAG,
                 "Enter water-in setting mode=%u enc_seq=%lu evt_type=%d rotate=%d",
                 (unsigned)ctx->setting.water_in_mode,
                 (unsigned long)ctx->ms.encoder.evt_seq,
                 (int)ctx->ms.encoder.evt_type,
                 (int)ctx->ms.encoder.rotate);
    }

    if (ctx->input.key_combo == KEY_COMBO_WATER_IN_MODE) {
        ctx->input.key_combo = 0;
        ESP_LOGI(TAG, "Water-in confirm by key combo mode=%u", (unsigned)ctx->setting.water_in_mode);
        sp_pro_app_save_settings();
        setting_sync_water_in_mode(ctx->setting.water_in_mode, true);
        ctx->state_runtime.setting.water_in_entered = false;
        voice_manager_play_interrupt(VOICE_EXITSWITCHINLETMODE);
        return ST_READY;
    }

    if (!setting_consume_encoder_event(ctx, &ctx->ms, &evt_type)) {
        return ST_SETTING;
    }

    switch (evt_type) {
    case ENC_EVT_ROTATE:
        prev_mode = ctx->setting.water_in_mode;
        ESP_LOGI(TAG,
                 "Water-in rotate evt rotate=%d prev=%u",
                 (int)ctx->ms.encoder.rotate,
                 (unsigned)prev_mode);
        if (ctx->ms.encoder.rotate == ENC_ROT_CW) {
            ctx->setting.water_in_mode = WATER_IN_MODE_BUCKET;
        } else if (ctx->ms.encoder.rotate == ENC_ROT_CCW) {
            ctx->setting.water_in_mode = WATER_IN_MODE_TANK;
        }
        ctx->setting.current_val = (float)ctx->setting.water_in_mode;
        ctx->setting.target_val = (float)ctx->setting.water_in_mode;
        ESP_LOGI(TAG,
                 "Water-in mode %s %u -> %u",
                 (prev_mode != ctx->setting.water_in_mode) ? "changed" : "kept",
                 (unsigned)prev_mode,
                 (unsigned)ctx->setting.water_in_mode);
        voice_manager_play_interrupt(setting_switch_water_in_voice(ctx->setting.water_in_mode));
        break;

    case ENC_EVT_CLICK:
        ESP_LOGI(TAG, "Water-in confirm mode=%u", (unsigned)ctx->setting.water_in_mode);
        sp_pro_app_save_settings();
        setting_sync_water_in_mode(ctx->setting.water_in_mode, true);
        ctx->state_runtime.setting.water_in_entered = false;
        voice_manager_play_interrupt(VOICE_EXITSWITCHINLETMODE);
        return ST_READY;

    default:
        break;
    }

    return ST_SETTING;
}

static app_state_t setting_handle_factory_reset(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    if (!ctx->state_runtime.setting.factory_reset_entered) {
        setting_reset_system_runtime(ctx);
        ctx->state_runtime.setting.factory_reset_entered = true;
        ctx->setting.reset_step = RESET_STEP_DOUBLE_CONFIRM;
        ctx->state_runtime.setting.factory_reset_step_tick = ctx->timer.tick;
        ctx->state_runtime.setting.factory_reset_prompt_voice_count =
            voice_manager_play_interrupt(VOICE_CONFIRMFACTORYRESET) ? 1U : 0U;
        ESP_LOGI(TAG, "Enter factory reset confirm");
    }

    switch (ctx->setting.reset_step) {
    case RESET_STEP_DOUBLE_CONFIRM:
        if (ctx->state_runtime.setting.factory_reset_prompt_voice_count == 0U) {
            ctx->state_runtime.setting.factory_reset_prompt_voice_count =
                voice_manager_play_interrupt(VOICE_CONFIRMFACTORYRESET) ? 1U : 0U;
        }

        if (ctx->input.key_long & (1U << KEY_ESPRESSO)) {
            ctx->input.key_long &= ~(1U << KEY_ESPRESSO);
            formula_store_factory_reset();
            mqtt_restore_local_defaults();
            device_statistics_factory_reset();
            mqtt_set_formula_force_update_pending(true);
            formula_store_set_force_update(true);
            ctx->setting.reset_step = RESET_STEP_DONE_AND_SHUTDOWN;
            ctx->state_runtime.setting.factory_reset_done_voice_count = 0U;
            ctx->state_runtime.setting.factory_reset_step_tick = ctx->timer.tick;
            ESP_LOGI(TAG, "Factory reset confirmed");
        }
        break;

    case RESET_STEP_DONE_AND_SHUTDOWN:
        if (ctx->timer.tick - ctx->state_runtime.setting.factory_reset_step_tick >= STAY_TICKS_2S) {
            if (ctx->state_runtime.setting.factory_reset_done_voice_count == 0U) {
                voice_manager_play_interrupt(VOICE_FACTORYRESETCOMPLETED);
                ctx->state_runtime.setting.factory_reset_done_voice_count = 1U;
            } else if (!voice_play_is_busy()) {
                setting_reset_system_runtime(ctx);
                ctx->setting.reset_step = RESET_STEP_NONE;
                return ST_OFF;
            }
        }
        break;

    case RESET_STEP_NONE:
    default:
        break;
    }

    return ST_SETTING;
}

app_state_t state_handle_setting(app_ctx_t *ctx)
{
    if (ctx->input.key_combo) {
        ctx->input.key_combo = 0;
        setting_reset_system_runtime(ctx);
        return ST_READY;
    }

    switch (ctx->setting.sub) {
    case SET_SUB_WATER_IN:
        return setting_handle_water_in(ctx);
    case SET_SUB_FACTORY_RESET:
        return setting_handle_factory_reset(ctx);
    case SET_SUB_WATER_HARDNESS:
        return setting_handle_water_hardness(ctx);
    default:
        setting_reset_system_runtime(ctx);
        return ST_READY;
    }
}
