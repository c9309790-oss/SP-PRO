#include "sp_pro_app_context.h"
#include "sp_pro_app_types.h"
#include "sp_pro_app.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_state.h"
#include <stddef.h>
#include "esp_log.h"

static const char *TAG = "state_ready";
#define READY_CLEAR_BEAN_ENTRY_HOLD_TICKS STAY_TICKS(2000)

extern const sp_pro_param_map_t g_param_map[];
extern FLASH_FACTORY_DATA factory_data;

static void ready_clear_pending_steam_clicks(app_ctx_t *ctx);
static void ready_mark_detection_entry(app_ctx_t *ctx);

static voice_id_t ready_notice_voice(const app_ctx_t *ctx)
{
    if (!ctx) {
        return VOICE_NONE;
    }

    switch (ctx->alarm.warning) {
    case WARN_WATER_BEAN_MISS:
        return VOICE_BEANHOPPERMISSING_FILLWATERTOMAX;

    case WARN_WATER_EMPTY:
        return VOICE_FILLWATERTOMAX;

    case WARN_BEAN_MISS:
        return VOICE_BEANHOPPERMISSING;

    case WARN_STEAM_NOT_READY:
        return VOICE_STEAMNOTREADY;

    case WARN_NONE:
    case WARN_BEAN_EMPTY:
    case WARN_TRAY_MISS:
    case WARN_LIQUID_ABNM:
    case WARN_BREW_HD_MISS:
    case WARN_GRIND_HD_MISS:
    default:
        return VOICE_NONE;
    }
}

static bool ready_key_blocked_by_notice(const app_ctx_t *ctx, uint8_t key)
{
    if (!ctx || !ctx->alarm.active || !ctx->alarm.is_notice) {
        return false;
    }

    switch (ctx->alarm.warning) {
    case WARN_WATER_EMPTY:
        return key == KEY_ESPRESSO ||
               key == KEY_AMERICANO ||
               key == KEY_COLD_BREW ||
               key == KEY_WATER ||
               key == KEY_STEAM ||
               key == KEY_GRIND ||
               key == KEY_CLEAN;

    case WARN_WATER_BEAN_MISS:
        return key == KEY_ESPRESSO ||
               key == KEY_AMERICANO ||
               key == KEY_COLD_BREW ||
               key == KEY_WATER ||
               key == KEY_STEAM ||
               key == KEY_GRIND ||
               key == KEY_CLEAN;

    case WARN_BEAN_MISS:
        return key == KEY_GRIND;

    default:
        return false;
    }
}

static bool ready_consume_blocked_key(app_ctx_t *ctx, uint8_t key, bool is_long_press)
{
    voice_id_t voice;

    if (!ctx || key >= KEY_MAX || !ready_key_blocked_by_notice(ctx, key)) {
        return false;
    }

    if (is_long_press) {
        ctx->input.key_long &= ~(1U << key);
    } else {
        ctx->input.key_pressed &= ~(1U << key);
    }

    voice = ready_notice_voice(ctx);
    if (voice != VOICE_NONE) {
        voice_manager_play_interrupt(voice);
    }
    return true;
}

static bool ready_maybe_enter_clear_bean(app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    if (ctx->ms.beanbox_in_place == 1U &&
        ctx->state_runtime.clear_bean.post_clean_notice_pending) {
        ctx->state_runtime.clear_bean.post_clean_notice_pending = false;
        ESP_LOGI(TAG, "Clear-bean post notice cleared after hopper relock");
    }

    if (!clear_bean_should_enter(ctx)) {
        ctx->state_runtime.ready.clear_bean_unlock_tick = 0U;
        return false;
    }

    if (ctx->state_runtime.ready.clear_bean_unlock_tick == 0U) {
        ctx->state_runtime.ready.clear_bean_unlock_tick = ctx->timer.tick;
        ESP_LOGI(TAG,
                 "Clear-bean unlock hold start tick=%lu hopper=%u",
                 (unsigned long)ctx->timer.tick,
                 (unsigned)ctx->ms.beanbox_in_place);
        return false;
    }

    if ((ctx->timer.tick - ctx->state_runtime.ready.clear_bean_unlock_tick) <
        READY_CLEAR_BEAN_ENTRY_HOLD_TICKS) {
        return false;
    }

    ctx->state_runtime.ready.clear_bean_unlock_tick = 0U;
    ESP_LOGI(TAG,
             "Clear-bean unlock hold satisfied tick=%lu hopper=%u -> ST_CLEAR_BEAN",
             (unsigned long)ctx->timer.tick,
             (unsigned)ctx->ms.beanbox_in_place);
    return true;
}

typedef struct {
    drink_type_t drink;
    app_state_t next_state;
} drink_map_t;

static const drink_map_t drink_map[6] = {
    [KEY_ESPRESSO]  = { DRINK_ESPRESSO,  ST_ESPRESSO },
    [KEY_AMERICANO] = { DRINK_AMERICANO, ST_AMERICANO },
    [KEY_COLD_BREW] = { DRINK_COLD_BREW, ST_COLD_BREW },
    [KEY_WATER]     = { DRINK_WATER,     ST_WATER },
    [KEY_STEAM]     = { DRINK_STEAM,     ST_STEAM },
    [KEY_GRIND]     = { DRINK_GRIND,     ST_GRIND },
};

static void ready_prepare_drink_target(app_ctx_t *ctx, drink_type_t drink)
{
    ctx->drink.target_drink = drink;

    switch (drink) {
    case DRINK_ESPRESSO:
        ctx->drink.target_ml = ctx->setting.esp_brew_w;
        ctx->drink.target_temp = ctx->setting.esp_brew_t;
        break;

    case DRINK_AMERICANO:
        ctx->drink.target_ml = ctx->setting.ame_brew_w;
        ctx->drink.target_temp = ctx->setting.ame_brew_t;
        break;

    case DRINK_COLD_BREW:
        ctx->drink.target_ml = ctx->setting.cold_brew_w;
        break;

    case DRINK_WATER:
        ctx->drink.target_ml = ctx->setting.hot_water_w;
        ctx->drink.target_temp = ctx->setting.hot_water_t;
        break;

    case DRINK_STEAM:
        ctx->drink.steam_level = ctx->setting.steam_level;
        break;

    default:
        break;
    }
}

static void ready_enter_drink_set(app_ctx_t *ctx, uint8_t drink_key)
{
    encoder_mode_t mode;

    ctx->setting.active = true;
    ctx->setting.drink_type = drink_key;
    ctx->setting.param_index = 0;
    ctx->setting.idle_timer = 0;
    ctx->setting.ui_phase = SET_UI_PHASE_HINT;
    ctx->setting.last_enc_seq = ctx->ms.encoder.evt_seq;
    ctx->setting.last_enc_active = ctx->ms.encoder.active;
    ctx->setting.last_enc_param_id = ctx->ms.encoder.param_id;
    ctx->setting.last_enc_evt_type = ctx->ms.encoder.evt_type;
    ctx->setting.last_enc_rotate = ctx->ms.encoder.rotate;
    ctx->setting.last_enc_value = ctx->ms.encoder.cur_value;
    ctx->setting.formula_dirty = false;

    mode = current_encoder_mode_for_ctx(ctx);
    ctx->setting.current_mode = mode;
    ctx->setting.current_val = get_cur_param_value_for_ctx(ctx, mode);
    ctx->setting.target_val = ctx->setting.current_val;

    setting_send_param(ctx);
}

static app_state_t ready_enter_setting_by_combo(app_ctx_t *ctx)
{
    switch (ctx->input.key_combo) {
#ifdef MAINT_TEST
    case KEY_COMBO_MAINT_BREW:
        ESP_LOGI(TAG, "Enter KEY_COMBO_MAINT_BREW: 0x%02X", ctx->input.key_combo);
        ctx->input.key_combo = 0;
        return ST_MAINT_BREW;

    case KEY_COMBO_MAINT_DES:
        ESP_LOGI(TAG, "Enter KEY_COMBO_MAINT_DES: 0x%02X", ctx->input.key_combo);
        ctx->input.key_combo = 0;
        return ST_MAINT_DES;

    case KEY_COMBO_MAINT_STEAM:
        ESP_LOGI(TAG, "Enter KEY_COMBO_MAINT_STEAM: 0x%02X", ctx->input.key_combo);
        ctx->input.key_combo = 0;
        return ST_MAINT_STEAM;
#endif

    case KEY_COMBO_WATER_IN_MODE:
        ctx->input.key_combo = 0;
        ctx->setting.sub = SET_SUB_WATER_IN;
        ESP_LOGI(TAG, "Enter setting SET_SUB_WATER_IN");
        return ST_SETTING;

    case KEY_COMBO_CLEAR_PIPE:
        ctx->input.key_combo = 0;
        ctx->setting.sub = SET_SUB_CLEAR_WATERWAY;
        ctx->setting.clear_step = CLEAR_STEP_NONE;
        ctx->setting.reset_step = RESET_STEP_NONE;
        ESP_LOGI(TAG, "Enter maintenance ST_MAINT_DRAIN");
        return ST_MAINT_DRAIN;

    case KEY_COMBO_FACTORY_RESET:
        ctx->input.key_combo = 0;
        ctx->setting.sub = SET_SUB_FACTORY_RESET;
        ctx->setting.reset_step = RESET_STEP_NONE;
        return ST_SETTING;

    case KEY_COMBO_WATER_HARDNESS:
        ctx->input.key_combo = 0;
        ctx->setting.sub = SET_SUB_WATER_HARDNESS;
        return ST_SETTING;

    case KEY_COMBO_CAL_POWDER:
        ctx->input.key_combo = 0;
        ctx->calibration.mode = CAL_MODE_POWDER;
        ctx->calibration.step = CAL_STEP_NONE;
        return ST_CALIBRATION;

    case KEY_COMBO_CAL_FLOW:
        ctx->input.key_combo = 0;
        ctx->calibration.mode = CAL_MODE_NONE;
        ctx->calibration.step = CAL_STEP_NONE;
        return ST_CALIBRATION;

    case KEY_COMBO_DETECTION:
        ctx->input.key_combo = 0;
        ready_mark_detection_entry(ctx);
        return ST_DETECTION;

    default:
        return ST_READY;
    }
}

static void ready_clear_pending_steam_clicks(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.ready.steam_click_count = 0U;
    ctx->state_runtime.ready.steam_click_deadline = 0U;
}

static void ready_mark_detection_entry(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ready_clear_pending_steam_clicks(ctx);
    ctx->detection.step = DET_STEP_NONE;
}

static bool ready_select_temp_param(app_ctx_t *ctx, uint8_t drink_key)
{
    const sp_pro_param_map_t *map = &g_param_map[drink_key];

    for (uint8_t i = 0; i < map->count; i++) {
        if (mode_is_temp(map->modes[i])) {
            ctx->setting.param_index = i;
            ctx->setting.current_mode = current_encoder_mode_for_ctx(ctx);
            ctx->setting.current_val = get_cur_param_value_for_ctx(ctx, ctx->setting.current_mode);
            ctx->setting.target_val = ctx->setting.current_val;
            ctx->setting.ui_phase = SET_UI_PHASE_HINT;
            return true;
        }
    }

    return false;
}

static app_state_t ready_handle_long_press(app_ctx_t *ctx)
{
    if (ctx->input.key_long & (1U << KEY_WIFI)) {
        ready_clear_pending_steam_clicks(ctx);
        ctx->input.key_long &= ~(1U << KEY_WIFI);
        ESP_LOGI(TAG, "Enter WIFI state");
        return ST_WIFI;
    }

    if ((ctx->input.key_long & (1U << KEY_CLEAN)) != 0U &&
        ctx->alarm.active &&
        ctx->alarm.is_notice &&
        (ctx->alarm.notice_type == MAINT_TYPE_STEAM ||
         ctx->alarm.notice_type == MAINT_TYPE_BREW ||
         ctx->alarm.notice_type == MAINT_TYPE_DES)) {
        ready_clear_pending_steam_clicks(ctx);
        ctx->input.key_long &= ~(1U << KEY_CLEAN);
        ESP_LOGI(TAG, "Dismiss maintain notice by clean key long press");
        sp_pro_dismiss_notice(ctx);
        return ST_READY;
    }

    uint16_t setting_mask = ctx->input.key_long &
                            ((1U << KEY_ESPRESSO) |
                             (1U << KEY_AMERICANO) |
                             (1U << KEY_COLD_BREW) |
                             (1U << KEY_WATER) |
                             (1U << KEY_STEAM) |
                             (1U << KEY_GRIND) |
                             (1U << KEY_CLEAN));
    uint8_t drink;

    if (setting_mask == 0U) {
        return ST_READY;
    }

    drink = (uint8_t)__builtin_ctz(setting_mask);
    ready_clear_pending_steam_clicks(ctx);

    ESP_LOGI(TAG, "key_long: %d", ctx->input.key_long);
    if (ready_consume_blocked_key(ctx, drink, true)) {
        return ST_READY;
    }
    ctx->input.key_long &= ~(1U << drink);
    ESP_LOGI(TAG, "Enter DRINK_SET: %d", drink);
    ready_enter_drink_set(ctx, drink);
    return ST_DRINK_SET;
}

static app_state_t ready_handle_drink_press(app_ctx_t *ctx)
{
    uint16_t drink_mask = ctx->input.key_pressed & 0x3FU;
    uint8_t drink;
    const drink_map_t *map;

    if (drink_mask == 0U) {
        return ST_READY;
    }

    drink = (uint8_t)__builtin_ctz(drink_mask);
    if (drink != KEY_STEAM && ready_consume_blocked_key(ctx, drink, false)) {
        ready_clear_pending_steam_clicks(ctx);
        return ST_READY;
    }

    if (drink == KEY_STEAM) {
        ctx->input.key_pressed &= ~(1U << KEY_STEAM);
        ready_clear_pending_steam_clicks(ctx);

        if (ready_consume_blocked_key(ctx, KEY_STEAM, false)) {
            return ST_READY;
        }

        if (ctx->ms.steam_flag == STEAM_UNREADY) {
            ctx->state_runtime.ready.steam_not_ready_notice_deadline =
                ctx->timer.tick + STAY_TICKS_2S;
            voice_manager_play_interrupt(VOICE_STEAMNOTREADY);
            return ST_READY;
        }

        ctx->state_runtime.ready.steam_not_ready_notice_deadline = 0U;
        ready_prepare_drink_target(ctx, DRINK_STEAM);
        return ST_STEAM;
    }

    ESP_LOGI(TAG, "key_pressed: %d", ctx->input.key_pressed);
    ready_clear_pending_steam_clicks(ctx);
    ctx->input.key_pressed &= ~(1U << drink);
    ESP_LOGI(TAG, "Enter DRINK_MAKE: %d", drink);
#ifdef AUTO_TEST
    if (drink == KEY_GRIND) {
        ctx->auto_test.auto_test_enable = true;
        ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        ctx->auto_test.auto_test_substate = 0;
        ESP_LOGI(TAG, "Auto Test START");
        return ST_AUTO_TEST;
    }
#endif

    map = &drink_map[drink];
    ready_prepare_drink_target(ctx, map->drink);
    if (drink == KEY_GRIND) {
        ESP_LOGI(TAG,
                 "Ready -> ST_GRIND tick=%lu key_pressed=0x%04X handle=%u hopper=%u grind_run=%u",
                 (unsigned long)ctx->timer.tick,
                 (unsigned)ctx->input.key_pressed,
                 (unsigned)ctx->ms.grind_handle_postion_flag,
                 (unsigned)ctx->ms.beanbox_in_place,
                 (unsigned)ctx->ms.grind_run_flg);
    }
    return map->next_state;
}

static app_state_t ready_handle_temp_shortcut(app_ctx_t *ctx)
{
    if ((ctx->input.key_pressed & (1U << KEY_TEMP)) == 0U) {
        return ST_READY;
    }

    ready_clear_pending_steam_clicks(ctx);
    ctx->input.key_pressed &= ~(1U << KEY_TEMP);
    ready_enter_drink_set(ctx, KEY_ESPRESSO);
    ready_select_temp_param(ctx, KEY_ESPRESSO);
    setting_send_param(ctx);
    return ST_DRINK_SET;
}

static app_state_t ready_handle_system_key(app_ctx_t *ctx)
{
    if (ctx->input.key_pressed & (1U << KEY_CHILD)) {
        ready_clear_pending_steam_clicks(ctx);
        ctx->input.key_pressed &= ~(1U << KEY_CHILD);
        ESP_LOGI(TAG, "Enter LOCK state");
        ctx->child_lock.enabled = 1;
        ctx->child_lock.press_tick = 0;
        return ST_LOCK;
    }

    if (ctx->input.key_pressed & (1U << KEY_CLEAN)) {
        ready_clear_pending_steam_clicks(ctx);
        if (ready_consume_blocked_key(ctx, KEY_CLEAN, false)) {
            return ST_READY;
        }
        ctx->input.key_pressed &= ~(1U << KEY_CLEAN);
        if (ctx->alarm.active && ctx->alarm.is_notice) {
            switch (ctx->alarm.notice_type) {
            case MAINT_TYPE_STEAM:
                ESP_LOGI(TAG, "Enter MAINT_STEAM by clean key notice");
                sp_pro_consume_notice(ctx);
                return ST_MAINT_STEAM;

            case MAINT_TYPE_BREW:
                ESP_LOGI(TAG, "Enter MAINT_BREW by clean key notice");
                sp_pro_consume_notice(ctx);
                return ST_MAINT_BREW;

            case MAINT_TYPE_DES:
                ESP_LOGI(TAG, "Enter MAINT_DES by clean key notice");
                sp_pro_consume_notice(ctx);
                return ST_MAINT_DES;

            case MAINT_TYPE_NONE:
            default:
                break;
            }
        }

        ESP_LOGI(TAG, "Enter CLEAN");
        return ST_CLEAN_BREW;
    }

    return ST_READY;
}

app_state_t state_handle_ready(app_ctx_t *ctx)
{
    app_state_t next_state;

    if (ctx->state_runtime.clear_bean.post_clean_notice_pending) {
        ready_clear_pending_steam_clicks(ctx);
        return ST_READY;
    }

    if (ready_maybe_enter_clear_bean(ctx)) {
        ready_clear_pending_steam_clicks(ctx);
        return ST_CLEAR_BEAN;
    }

    if (ctx->input.key_combo) {
        ESP_LOGI(TAG, "Enter setting by combo: 0x%02X", ctx->input.key_combo);
        ready_clear_pending_steam_clicks(ctx);
        return ready_enter_setting_by_combo(ctx);
    }

    next_state = ready_handle_long_press(ctx);
    if (next_state != ST_READY) {
        return next_state;
    }

    next_state = ready_handle_drink_press(ctx);
    if (next_state != ST_READY) {
        return next_state;
    }

    next_state = ready_handle_temp_shortcut(ctx);
    if (next_state != ST_READY) {
        return next_state;
    }

    next_state = ready_handle_system_key(ctx);
    if (next_state != ST_READY) {
        return next_state;
    }

    return ST_READY;
}

#define AUTO_GRIND_SEC     10
#define AUTO_WAIT_SEC      5
#define AUTO_ESPRESSO_SEC  30
#define AUTO_WATER_SEC     30
#define AUTO_STEAM_SEC     30

app_state_t state_handle_auto_test(app_ctx_t *ctx)
{
    if (!ctx->state_runtime.auto_test.started) {
        ctx->auto_test.auto_test_substate = 0;
        ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        ctx->state_runtime.auto_test.started = true;
        ctr_cmd_action(CTRL_ACT_GRIND_START, NULL);
        ESP_LOGE(TAG, "GRIND START...");
    }

    uint32_t elapsed_ms = (ctx->timer.tick - ctx->auto_test.auto_test_start_tick) * LOGIC_TASK_MS;

    switch (ctx->auto_test.auto_test_substate) {
    case 0:
        if (elapsed_ms >= AUTO_GRIND_SEC * 1000) {
            ESP_LOGE(TAG, "GRIND STOP...");
            ctr_cmd_action(CTRL_ACT_GRIND_STOP, NULL);
            ctx->auto_test.auto_test_substate = 1;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        }
        break;

    case 1:
        if (elapsed_ms >= AUTO_WAIT_SEC * 1000) {
            ctx->auto_test.auto_test_substate = 2;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
            ctr_cmd_action(CTRL_ACT_ESPRESSO, NULL);
            ESP_LOGE(TAG, "ESPRESSO START...");
        }
        break;

    case 2:
        if (elapsed_ms >= AUTO_ESPRESSO_SEC * 1000) {
            ESP_LOGE(TAG, "ESPRESSO STOP...");
            ctr_cmd_action(CTRL_ACT_CANCEL, NULL);
            ctx->auto_test.auto_test_substate = 3;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        }
        break;

    case 3:
        if (elapsed_ms >= AUTO_WAIT_SEC * 1000) {
            ctx->auto_test.auto_test_substate = 4;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
            ctr_cmd_action(CTRL_ACT_HOT_WATER, NULL);
            ESP_LOGE(TAG, "WATER START...");
        }
        break;

    case 4:
        if (elapsed_ms >= AUTO_WATER_SEC * 1000) {
            ESP_LOGE(TAG, "WATER STOP...");
            ctr_cmd_action(CTRL_ACT_CANCEL, NULL);
            ctx->auto_test.auto_test_substate = 5;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        }
        break;

    case 5:
        if (elapsed_ms >= AUTO_WAIT_SEC * 1000) {
            ctx->auto_test.auto_test_substate = 6;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
            ctr_cmd_action(CTRL_ACT_STEAM_START, NULL);
            ESP_LOGE(TAG, "STEAM START...");
        }
        break;

    case 6:
        if (elapsed_ms >= AUTO_STEAM_SEC * 1000) {
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            ctx->auto_test.auto_test_substate = 7;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
            ESP_LOGE(TAG, "STEAM STOP...");
        }
        break;

    case 7:
        if (elapsed_ms >= AUTO_WAIT_SEC * 1000) {
            ctx->state_runtime.auto_test.started = false;
            ctx->auto_test.auto_test_substate = 0;
            ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        }
        break;

    default:
        ctx->state_runtime.auto_test.started = false;
        ctx->auto_test.auto_test_substate = 0;
        ctx->auto_test.auto_test_start_tick = ctx->timer.tick;
        break;
    }

    if (ctx->input.key_pressed) {
        ESP_LOGI(TAG, "Auto Test STOP by key press");
        ctx->auto_test.auto_test_enable = false;
        ctx->state_runtime.auto_test.started = false;
        ctx->input.key_pressed = 0;
        return ST_READY;
    }

    return ST_AUTO_TEST;
}
