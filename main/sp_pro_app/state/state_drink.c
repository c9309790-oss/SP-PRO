#include "sp_pro_app.h"
#include "sp_pro_app_types.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_state.h"
#include "sp_pro_build_flags.h"
#include <string.h>
#include <stddef.h>
#include "esp_log.h"
#include "device_statistics_store.h"
#include "drink_record_service.h"
#include "extraction_curve_service.h"

static const char *TAG = "state_drink";
static const uint32_t STEAM_TAIL_SPRAY_DELAY_TICKS = STAY_TICKS_15S;
static const uint32_t STEAM_TAIL_SPRAY_DURATION_TICKS = STAY_TICKS_3S;
static const uint32_t STEAM_TAIL_SPRAY_MIN_RUN_TICKS = STAY_TICKS(10000);
static const uint32_t STEAM_MAX_RUN_TICKS = STAY_TICKS(90000);
static const uint32_t GRIND_RESUME_TIMEOUT_TICKS = STAY_TICKS(600000);
static const uint32_t DRINK_FINISH_CONFIRM_TICKS = STAY_TICKS_1S;
static const float DRINK_FINISH_IDLE_FLOW_MAX_ML_S = 0.05f;
static const float GRIND_RESUME_MIN_REMAIN_G = 1.0f;
static const float WATER_DISPLAY_FLOW_MIN_ML_S = 0.02f;
static const float WATER_DISPLAY_FLOW_MAX_ML_S = 6.0f;
static const float WATER_DISPLAY_FLOW_ALPHA = 0.25f;
static const float WATER_COUNTER_RESET_REBASE_THRESHOLD_ML = 20.0f;
static const float DRINK_COUNTER_RESET_REBASE_THRESHOLD_ML = 2.0f;
static const float DRINK_COUNTER_RESET_NEAR_ZERO_ML = 2.0f;
static const float DRINK_SELF_ABORT_NO_OUTPUT_MAX_ML = 0.5f;
static const uint32_t DRINK_CANCEL_RETRY_TICKS = STAY_TICKS(300);

static inline void ui_anim_start_preheat(app_ctx_t *ctx);
static inline void ui_anim_stop_preheat(app_ctx_t *ctx);
static bool steam_stop_requested(app_ctx_t *ctx);
static void steam_enter_tail_spray_wait(app_ctx_t *ctx);
static void steam_enter_tail_spray_countdown(app_ctx_t *ctx);
static void drink_capture_liquid_session_base(app_ctx_t *ctx);
static bool drink_local_target_reached(app_ctx_t *ctx, const char *label);
static void drink_water_display_reset(app_ctx_t *ctx);
static void drink_water_capture_start_snapshot(app_ctx_t *ctx);
static bool drink_water_rebase_if_counter_reset(app_ctx_t *ctx, const char *label);
static bool drink_rebase_if_counter_reset(app_ctx_t *ctx, const char *label);
static bool drink_water_ready_for_new_session(app_ctx_t *ctx);
static void drink_water_restart(app_ctx_t *ctx);
static void drink_water_display_update(app_ctx_t *ctx);
static control_action_t drink_stop_action_for(drink_type_t drink);
static void drink_clear_finish_wait(app_ctx_t *ctx);
static bool drink_brew_finish_confirmed(app_ctx_t *ctx, const char *label);
static bool drink_controller_self_abort_confirmed(app_ctx_t *ctx, const char *label);
static bool drink_handle_cancel_pending(app_ctx_t *ctx, const char *label);
static void drink_enter_remote_countdown(app_ctx_t *ctx, bool water_stage);
static const char *steam_flag_name(uint8_t steam_flag);
static bool drink_supports_parallel_steam(const app_ctx_t *ctx);
static void drink_clear_parallel_steam(app_ctx_t *ctx);
static bool drink_start_parallel_steam(app_ctx_t *ctx);
static void drink_promote_parallel_steam(app_ctx_t *ctx);
static float drink_grind_remaining_g(const app_ctx_t *ctx);

static bool drink_is_remote_flow(const app_ctx_t *ctx)
{
    return ctx && ctx->ctrl.src == CTRL_SRC_MQTT && ctx->ctrl.formula != NULL;
}

static void drink_clear_remote_flow(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->ctrl.src == CTRL_SRC_MQTT) {
        ctx->ctrl.formula = NULL;
        memset(&ctx->ctrl.remote_formula, 0, sizeof(ctx->ctrl.remote_formula));
        ctx->ctrl.busy = false;
        ctx->ctrl.request_drink = DRINK_ESPRESSO;
        ctx->ctrl.src = CTRL_SRC_UI;
    }

    ctx->state_runtime.drink.remote_action_started = false;
}

static void drink_apply_remote_targets(app_ctx_t *ctx, bool water_stage)
{
    const formula_info_t *formula;

    if (!ctx || !drink_is_remote_flow(ctx)) {
        return;
    }

    formula = ctx->ctrl.formula;
    switch (ctx->drink.target_drink) {
    case DRINK_ESPRESSO:
    case DRINK_MASTER:
        ctx->drink.target_ml = formula->preset_liquid_weight;
        ctx->drink.target_temp = (float)formula->preset_temperature;
        break;

    case DRINK_AMERICANO:
        if (water_stage) {
            ctx->drink.target_ml = formula->water_weight;
            ctx->drink.target_temp = (float)formula->water_temperature;
        } else {
            ctx->drink.target_ml = formula->preset_liquid_weight;
            ctx->drink.target_temp = (float)formula->preset_temperature;
        }
        break;

    case DRINK_COLD_BREW:
        ctx->drink.target_ml = formula->preset_liquid_weight > 0U ?
                               formula->preset_liquid_weight :
                               formula->water_weight;
        ctx->drink.target_temp = (float)formula->preset_temperature;
        break;

    case DRINK_WATER:
        ctx->drink.target_ml = formula->water_weight;
        ctx->drink.target_temp = (float)formula->water_temperature;
        break;

    default:
        break;
    }
}

static bool drink_remote_countdown_done(const app_ctx_t *ctx)
{
    return ctx && (ctx->timer.tick - ctx->drink.target_time) >= STAY_TICKS_3S;
}

static void drink_enter_remote_countdown(app_ctx_t *ctx, bool water_stage)
{
    if (!ctx) {
        return;
    }

    ctx->core.substate = BREW_SUB_REMOTE_COUNTDOWN;
    ctx->drink.target_time = ctx->timer.tick;
    drink_apply_remote_targets(ctx, water_stage);
    voice_manager_play_touch_tone();
}

static bool drink_start_remote_action(app_ctx_t *ctx,
                                      control_action_t action,
                                      app_state_t state,
                                      brew_substate_t next_substate)
{
    if (!ctx || !drink_is_remote_flow(ctx)) {
        return false;
    }

    ctx->ctrl.src = CTRL_SRC_MQTT;
    if (!ctr_cmd_action(action, &ctx->ctrl.remote_formula)) {
        return false;
    }

    device_statistics_notify_remote_action_start(action, &ctx->ctrl.remote_formula);
    drink_record_notify_remote_action_start(action, &ctx->ctrl.remote_formula);
    extraction_curve_notify_remote_action_start(action, &ctx->ctrl.remote_formula);
    ctx->state_runtime.drink.remote_action_started = true;
    ctx->state_runtime.drink.liquid_output_seen = false;
    ctx->state_runtime.drink.liquid_session_base_ml = ctx->ms.liquid_weight;
    ctx->state_runtime.drink.last_encoder_evt_seq = ctx->ms.encoder.evt_seq;
    ctx->state_runtime.drink.exit_reason = DRINK_EXIT_NONE;
    ctx->drink.start_tick = ctx->timer.tick;
    ctx->drink.elapsed_tick = 0U;
    ui_anim_start_preheat(ctx);
    ctx->core.substate = next_substate;
    ESP_LOGI(TAG, "Remote drink action started: state=%d action=%d", (int)state, (int)action);
    return true;
}

static bool check_drink_flag_ready(app_ctx_t *ctx)
{
    ctx->drink.current_temp = ctx->ms.hot_current_temp;

    if (ctx->ms.drink_making_flg > DRINK_MAKER_NONE) {
        ESP_LOGI(TAG, "Preheat complete: current=%.1f C, making_flg=%d",
                 ctx->drink.current_temp,
                 ctx->ms.drink_making_flg);
        return true;
    }

    return false;
}

static bool check_water_flag_ready(app_ctx_t *ctx)
{
    ctx->drink.current_temp = ctx->ms.hot_current_temp;
    return drink_water_ready_for_new_session(ctx);
}

static inline void ui_anim_start_preheat(app_ctx_t *ctx)
{
    ctx->anim.active = true;
    ctx->anim.step = 0;
    ctx->anim.tick = 0;
    ctx->anim.interval = 25;
}

static inline void ui_anim_stop_preheat(app_ctx_t *ctx)
{
    ctx->anim.active = false;
}

static inline uint32_t drink_elapsed_seconds(const app_ctx_t *ctx)
{
    return (ctx->timer.tick - ctx->drink.start_tick) / STAY_TICKS_1S;
}

static inline void drink_reset_runtime(app_ctx_t *ctx)
{
    ctx->drink.start_tick = ctx->timer.tick;
    ctx->drink.elapsed_tick = 0;
    ctx->drink.finish_tick = 0;
    ctx->drink.target_time = 0;
    ctx->drink.progress = 0;
    ctx->state_runtime.drink.grind_prepare_wait_logged = false;
    ctx->state_runtime.drink.grind_allow_without_handle = false;
    ctx->state_runtime.drink.prepare_cmd_sent = false;
    ctx->state_runtime.drink.grind_notice_pause_sent = false;
    ctx->state_runtime.drink.grind_resume_pending = false;
    ctx->state_runtime.drink.americano_water_started = false;
    ctx->state_runtime.drink.force_target_display_active = false;
    ctx->state_runtime.drink.liquid_output_seen = false;
    ctx->state_runtime.drink.finish_wait_none_started = false;
    ctx->state_runtime.drink.cancel_pending_ignore_flow = false;
    ctx->state_runtime.drink.water_idle_seen_after_start = false;
    ctx->state_runtime.drink.grind_resume_remaining_g = 0.0f;
    ctx->state_runtime.drink.liquid_session_base_ml = ctx->ms.liquid_weight;
    ctx->state_runtime.drink.display_liquid_base_ml = 0.0f;
    ctx->state_runtime.drink.display_liquid_ml = 0.0f;
    ctx->state_runtime.drink.display_flow_rate_ml_s = 0.0f;
    ctx->state_runtime.drink.force_target_display_ml = 0.0f;
    ctx->state_runtime.drink.water_start_liquid_ml = 0.0f;
    ctx->state_runtime.drink.target_display_cap_tick = 0U;
    ctx->state_runtime.drink.grind_resume_pause_tick = 0U;
    ctx->state_runtime.drink.finish_wait_tick = 0U;
    ctx->state_runtime.drink.last_encoder_evt_seq = ctx->ms.encoder.evt_seq;
    ctx->state_runtime.drink.last_water_log_sec = 0xFFFFFFFFU;
    ctx->state_runtime.drink.last_steam_log_sec = 0xFFFFFFFFU;
    ctx->state_runtime.drink.last_steam_flag = 0xFFU;
    ctx->state_runtime.drink.water_start_hot_flag = 0U;
    ctx->state_runtime.drink.parallel_steam_active = false;
    ctx->state_runtime.drink.parallel_steam_start_tick = 0U;
    ctx->state_runtime.drink.exit_reason = DRINK_EXIT_NONE;
}

static float drink_grind_remaining_g(const app_ctx_t *ctx)
{
    float remaining;

    if (!ctx) {
        return 0.0f;
    }

    remaining = ctx->setting.grind_w - ctx->ms.powder_weight;
    return remaining > 0.0f ? remaining : 0.0f;
}

static void drink_capture_liquid_session_base(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.drink.liquid_session_base_ml = ctx->ms.liquid_weight;
    ctx->state_runtime.drink.liquid_output_seen = false;
}

static bool drink_local_target_reached(app_ctx_t *ctx, const char *label)
{
    drink_liquid_compensation_t comp;
    float raw_progress;
    float display_progress;
    float stop_threshold;

    if (!ctx || drink_is_remote_flow(ctx) || ctx->drink.target_ml == 0U) {
        return false;
    }

    raw_progress = sp_pro_get_raw_liquid_progress_ml(ctx);
    display_progress = sp_pro_get_display_liquid_ml(ctx);
    comp = sp_pro_get_active_liquid_compensation(ctx);
    /* Keep display compensation and stop compensation independent.
     * display_offset_ml only affects what the user sees on screen.
     * stop_ahead_ml only affects when we stop the flow. */
    stop_threshold = (float)ctx->drink.target_ml - comp.stop_ahead_ml;
    if (stop_threshold < 0.0f) {
        stop_threshold = 0.0f;
    }

    if (!ctx->state_runtime.drink.liquid_output_seen &&
        (display_progress > 0.1f || ctx->ms.flow_rate > WATER_DISPLAY_FLOW_MIN_ML_S)) {
        ctx->state_runtime.drink.liquid_output_seen = true;
        ESP_LOGI(TAG,
                 "%s first compensated liquid raw=%.1f display=%.1f target=%u scale=%.3f offset=%.1f stopAhead=%.1f",
                 label ? label : "DRINK",
                 raw_progress,
                 display_progress,
                 (unsigned)ctx->drink.target_ml,
                 comp.display_scale,
                 comp.display_offset_ml,
                 comp.stop_ahead_ml);
    }

    if (!ctx->state_runtime.drink.liquid_output_seen) {
        return false;
    }

    if (raw_progress >= stop_threshold) {
        ctx->state_runtime.drink.force_target_display_active = true;
        ctx->state_runtime.drink.force_target_display_ml = (float)ctx->drink.target_ml;
        if (ctx->core.state == ST_WATER ||
            (ctx->core.state == ST_AMERICANO &&
             (ctx->core.substate == BREW_SUB_RUNNING_2 ||
              ctx->core.substate == BREW_SUB_FINISH))) {
            ctx->state_runtime.drink.display_liquid_ml = (float)ctx->drink.target_ml;
        }
        ESP_LOGI(TAG,
                 "%s local target reached raw=%.1f display=%.1f target=%u stop_threshold=%.1f scale=%.3f offset=%.1f stopAhead=%.1f",
                 label ? label : "DRINK",
                 raw_progress,
                 display_progress,
                 (unsigned)ctx->drink.target_ml,
                 stop_threshold,
                 comp.display_scale,
                 comp.display_offset_ml,
                 comp.stop_ahead_ml);
        return true;
    }

    return false;
}

static bool drink_hot_water_finish_confirmed(app_ctx_t *ctx, const char *label)
{
    if (!ctx) {
        return false;
    }

    if (ctx->ms.hot_water_flg != 0U ||
        ctx->ms.flow_rate > DRINK_FINISH_IDLE_FLOW_MAX_ML_S) {
        if (ctx->state_runtime.drink.finish_wait_none_started) {
            ESP_LOGI(TAG,
                     "%s hot-water finish cleared tick=%lu hot_water_flg=%u flow_rate=%.2f",
                     label ? label : "WATER",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.hot_water_flg,
                     ctx->ms.flow_rate);
        }
        drink_clear_finish_wait(ctx);
        return false;
    }

    if (!ctx->state_runtime.drink.finish_wait_none_started) {
        ctx->state_runtime.drink.finish_wait_none_started = true;
        ctx->state_runtime.drink.finish_wait_tick = ctx->timer.tick;
        ESP_LOGI(TAG,
                 "%s hot-water finish start tick=%lu liquid=%.1f display=%.1f flow_rate=%.2f",
                 label ? label : "WATER",
                 (unsigned long)ctx->timer.tick,
                 ctx->ms.liquid_weight,
                 sp_pro_get_display_liquid_ml(ctx),
                 ctx->ms.flow_rate);
        return false;
    }

    return (ctx->timer.tick - ctx->state_runtime.drink.finish_wait_tick) >=
           DRINK_FINISH_CONFIRM_TICKS;
}

static void drink_water_display_reset(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.drink.display_liquid_base_ml =
        (ctx->ms.liquid_weight > 0.0f) ? ctx->ms.liquid_weight : 0.0f;
    ctx->state_runtime.drink.display_flow_rate_ml_s = 0.0f;
    ctx->state_runtime.drink.display_liquid_ml = 0.0f;
}

static void drink_water_capture_start_snapshot(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.drink.water_start_hot_flag = (uint8_t)ctx->ms.hot_water_flg;
    ctx->state_runtime.drink.water_start_liquid_ml = ctx->ms.liquid_weight;
    ctx->state_runtime.drink.water_idle_seen_after_start = (ctx->ms.hot_water_flg == 0U);
}

static bool drink_water_rebase_if_counter_reset(app_ctx_t *ctx, const char *label)
{
    float current_liquid;
    float session_drop;
    float display_drop;

    if (!ctx) {
        return false;
    }

    current_liquid = ctx->ms.liquid_weight;
    session_drop = ctx->state_runtime.drink.liquid_session_base_ml - current_liquid;
    display_drop = ctx->state_runtime.drink.display_liquid_base_ml - current_liquid;

    if (session_drop < WATER_COUNTER_RESET_REBASE_THRESHOLD_ML &&
        display_drop < WATER_COUNTER_RESET_REBASE_THRESHOLD_ML) {
        return false;
    }

    ESP_LOGW(TAG,
             "%s liquid counter reset detected, rebase session %.1f->%.1f display %.1f->%.1f",
             label ? label : "WATER",
             ctx->state_runtime.drink.liquid_session_base_ml,
             current_liquid,
             ctx->state_runtime.drink.display_liquid_base_ml,
             current_liquid);

    ctx->state_runtime.drink.liquid_session_base_ml = current_liquid;
    ctx->state_runtime.drink.display_liquid_base_ml = current_liquid;
    ctx->state_runtime.drink.display_liquid_ml = 0.0f;
    ctx->state_runtime.drink.display_flow_rate_ml_s = 0.0f;
    ctx->state_runtime.drink.liquid_output_seen = false;
    return true;
}

static bool drink_rebase_if_counter_reset(app_ctx_t *ctx, const char *label)
{
    float current_liquid;
    float old_base;
    float drop;

    if (!ctx) {
        return false;
    }

    current_liquid = ctx->ms.liquid_weight;
    old_base = ctx->state_runtime.drink.liquid_session_base_ml;
    drop = old_base - current_liquid;

    if (drop < DRINK_COUNTER_RESET_REBASE_THRESHOLD_ML) {
        return false;
    }

    if (current_liquid > DRINK_COUNTER_RESET_NEAR_ZERO_ML &&
        drop < WATER_COUNTER_RESET_REBASE_THRESHOLD_ML) {
        return false;
    }

    ctx->state_runtime.drink.liquid_session_base_ml = current_liquid;
    ESP_LOGW(TAG,
             "%s liquid counter reset detected, rebase session %.1f->%.1f",
             label ? label : "DRINK",
             old_base,
             current_liquid);
    return true;
}

static bool drink_water_ready_for_new_session(app_ctx_t *ctx)
{
    float liquid_delta;

    if (!ctx) {
        return false;
    }

    if (ctx->ms.hot_water_flg == 0U) {
        ctx->state_runtime.drink.water_idle_seen_after_start = true;
    }

    if (ctx->ms.flow_rate > 0.01f) {
        ESP_LOGI(TAG,
                 "Water ready by flow-rate: current=%.1f C, hot_water_flg=%d flow_rate=%.2f liquid=%.1f",
                 ctx->drink.current_temp,
                 ctx->ms.hot_water_flg,
                 ctx->ms.flow_rate,
                 ctx->ms.liquid_weight);
        return true;
    }

    liquid_delta = ctx->ms.liquid_weight - ctx->state_runtime.drink.water_start_liquid_ml;
    if (liquid_delta < 0.0f) {
        liquid_delta = -liquid_delta;
    }
    if (liquid_delta > 0.1f) {
        ESP_LOGI(TAG,
                 "Water ready by liquid change: current=%.1f C, hot_water_flg=%d flow_rate=%.2f liquid=%.1f start_liquid=%.1f",
                 ctx->drink.current_temp,
                 ctx->ms.hot_water_flg,
                 ctx->ms.flow_rate,
                 ctx->ms.liquid_weight,
                 ctx->state_runtime.drink.water_start_liquid_ml);
        return true;
    }

    if (ctx->state_runtime.drink.water_start_hot_flag == 0U &&
        ctx->ms.hot_water_flg > DRINK_MAKER_NONE) {
        ESP_LOGI(TAG, "Water ready by hot-water flag rise: current=%.1f C, hot_water_flg=%d",
                 ctx->drink.current_temp,
                 ctx->ms.hot_water_flg);
        return true;
    }

    if (ctx->state_runtime.drink.water_start_hot_flag > DRINK_MAKER_NONE &&
        ctx->state_runtime.drink.water_idle_seen_after_start &&
        ctx->ms.hot_water_flg > DRINK_MAKER_NONE) {
        ESP_LOGI(TAG,
                 "Water ready by hot-water flag re-arm: current=%.1f C, hot_water_flg=%d start_flag=%u",
                 ctx->drink.current_temp,
                 ctx->ms.hot_water_flg,
                 (unsigned)ctx->state_runtime.drink.water_start_hot_flag);
        return true;
    }

    return false;
}

static void drink_water_display_update(app_ctx_t *ctx)
{
    drink_liquid_compensation_t comp;
    float flow_rate;
    float display;
    float raw_progress;

    if (!ctx) {
        return;
    }

    flow_rate = ctx->ms.flow_rate;
    if (flow_rate < WATER_DISPLAY_FLOW_MIN_ML_S) {
        flow_rate = 0.0f;
    } else if (flow_rate > WATER_DISPLAY_FLOW_MAX_ML_S) {
        flow_rate = WATER_DISPLAY_FLOW_MAX_ML_S;
    }

    raw_progress = ctx->ms.liquid_weight - ctx->state_runtime.drink.display_liquid_base_ml;
    if (raw_progress < 0.0f) {
        raw_progress = 0.0f;
    }

    ctx->state_runtime.drink.display_flow_rate_ml_s +=
        (flow_rate - ctx->state_runtime.drink.display_flow_rate_ml_s) * WATER_DISPLAY_FLOW_ALPHA;
    if (ctx->state_runtime.drink.display_flow_rate_ml_s < WATER_DISPLAY_FLOW_MIN_ML_S) {
        ctx->state_runtime.drink.display_flow_rate_ml_s = 0.0f;
    }

    comp = sp_pro_get_active_liquid_compensation(ctx);
    display = sp_pro_apply_liquid_display_compensation(raw_progress, comp);

    ctx->state_runtime.drink.display_liquid_ml = display;
}

static const char *steam_flag_name(uint8_t steam_flag)
{
    switch (steam_flag) {
    case STEAM_IDLE:
        return "IDLE";
    case STEAM_UNREADY:
        return "UNREADY";
    case STEAM_READY:
        return "READY";
    case STEAM_RUNNING:
        return "RUNNING";
    default:
        return "UNKNOWN";
    }
}

static inline void drink_begin_prepare(app_ctx_t *ctx,
                                       control_action_t action,
                                       bool with_preheat)
{
    ctx->core.substate = BREW_SUB_PREPARE;
    if (with_preheat) {
        ui_anim_start_preheat(ctx);
    } else {
        ui_anim_stop_preheat(ctx);
    }

    ctx->ctrl.src = CTRL_SRC_UI;
    ctr_cmd_action(action, NULL);
    drink_reset_runtime(ctx);
}

static void drink_water_restart(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    drink_begin_prepare(ctx, CTRL_ACT_HOT_WATER, true);
    drink_water_display_reset(ctx);
    drink_water_capture_start_snapshot(ctx);
    device_statistics_notify_local_state_start(ST_WATER);
    ESP_LOGI(TAG,
             "WATER restart tick=%lu target_ml=%u target_temp=%.1f hot_water_flg=%u temp=%.1f liquid=%.1f",
             (unsigned long)ctx->timer.tick,
             (unsigned)ctx->drink.target_ml,
             ctx->drink.target_temp,
             (unsigned)ctx->ms.hot_water_flg,
             ctx->ms.hot_current_temp,
             ctx->ms.liquid_weight);
}

static inline void drink_prepare_local_entry(app_ctx_t *ctx, bool with_preheat)
{
    if (!ctx) {
        return;
    }

    ctx->core.substate = BREW_SUB_PREPARE;
    if (with_preheat) {
        ui_anim_start_preheat(ctx);
    } else {
        ui_anim_stop_preheat(ctx);
    }

    drink_reset_runtime(ctx);
}

static bool drink_prepare_send_local_action(app_ctx_t *ctx, control_action_t action)
{
    if (!ctx || ctx->state_runtime.drink.prepare_cmd_sent) {
        return false;
    }

    ctx->ctrl.src = CTRL_SRC_UI;
    if (!ctr_cmd_action(action, NULL)) {
        return false;
    }

    ctx->state_runtime.drink.prepare_cmd_sent = true;
    ctx->drink.start_tick = ctx->timer.tick;
    ctx->drink.elapsed_tick = 0U;
    drink_capture_liquid_session_base(ctx);
    return true;
}

static inline void drink_begin_direct(app_ctx_t *ctx, control_action_t action)
{
    ctx->core.substate = BREW_SUB_PREPARE;
    ui_anim_stop_preheat(ctx);
    drink_reset_runtime(ctx);
    ctx->ctrl.src = CTRL_SRC_UI;
    ctr_cmd_action(action, NULL);
    ctx->drink.start_tick = ctx->timer.tick;
    drink_capture_liquid_session_base(ctx);
    ctx->core.substate = BREW_SUB_RUNNING_1;
}

static inline void drink_enter_running(app_ctx_t *ctx, brew_substate_t substate)
{
    ctx->drink.start_tick = ctx->timer.tick;
    ctx->core.substate = substate;
    ui_anim_stop_preheat(ctx);
    drink_clear_finish_wait(ctx);
}

static inline void drink_enter_finish(app_ctx_t *ctx, control_action_t stop_action)
{
    ctx->drink.finish_tick = ctx->timer.tick;
    ctx->core.substate = BREW_SUB_FINISH;
    ctr_cmd_action(stop_action, NULL);
}

static void drink_enter_cancel_pending(app_ctx_t *ctx)
{
    control_action_t stop_action;
    bool cancel_during_prepare;

    if (!ctx) {
        return;
    }

    stop_action = drink_stop_action_for(ctx->drink.target_drink);
    cancel_during_prepare =
        (ctx->core.substate == BREW_SUB_PREPARE ||
         ctx->core.substate == BREW_SUB_REMOTE_COUNTDOWN) &&
        !ctx->state_runtime.drink.liquid_output_seen;
    ctx->state_runtime.drink.exit_reason = DRINK_EXIT_CANCEL;
    ctx->state_runtime.drink.cancel_pending_ignore_flow = cancel_during_prepare;
    ctx->core.substate = BREW_SUB_CANCEL_PENDING;
    ctx->drink.target_time = ctx->timer.tick;
    ui_anim_stop_preheat(ctx);
    drink_clear_finish_wait(ctx);
    ctr_cmd_action(stop_action, NULL);
    ESP_LOGI(TAG,
             "Enter cancel pending ignore_flow=%d making=%u hot_water=%u grind=%u steam=%s flow=%.2f liquid=%.1f",
             cancel_during_prepare ? 1 : 0,
             (unsigned)ctx->ms.drink_making_flg,
             (unsigned)ctx->ms.hot_water_flg,
             (unsigned)ctx->ms.grind_run_flg,
             steam_flag_name((uint8_t)ctx->ms.steam_flag),
             ctx->ms.flow_rate,
             ctx->ms.liquid_weight);
}

static inline bool drink_finish_delay_done(app_ctx_t *ctx)
{
    return (ctx->timer.tick - ctx->drink.finish_tick) >= STAY_TICKS_3S;
}

static void drink_clear_finish_wait(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.drink.finish_wait_none_started = false;
    ctx->state_runtime.drink.finish_wait_tick = 0U;
}

static bool drink_cancel_confirmed(app_ctx_t *ctx, const char *label)
{
    if (!ctx) {
        return false;
    }

    if (ctx->ms.drink_making_flg != DRINK_MAKER_NONE ||
        ctx->ms.hot_water_flg != 0U ||
        ctx->ms.grind_run_flg != 0U ||
        ctx->ms.steam_flag == STEAM_RUNNING ||
        (!ctx->state_runtime.drink.cancel_pending_ignore_flow &&
         ctx->ms.flow_rate > DRINK_FINISH_IDLE_FLOW_MAX_ML_S)) {
        if (ctx->state_runtime.drink.finish_wait_none_started) {
            ESP_LOGI(TAG,
                     "%s cancel confirm cleared tick=%lu making=%u hot_water=%u grind=%u steam=%s flow=%.2f",
                     label ? label : "DRINK",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.drink_making_flg,
                     (unsigned)ctx->ms.hot_water_flg,
                     (unsigned)ctx->ms.grind_run_flg,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.flow_rate);
        }
        drink_clear_finish_wait(ctx);
        return false;
    }

    if (!ctx->state_runtime.drink.finish_wait_none_started) {
        ctx->state_runtime.drink.finish_wait_none_started = true;
        ctx->state_runtime.drink.finish_wait_tick = ctx->timer.tick;
        ESP_LOGI(TAG,
                 "%s cancel confirm start tick=%lu making=%u hot_water=%u grind=%u steam=%s flow=%.2f",
                 label ? label : "DRINK",
                 (unsigned long)ctx->timer.tick,
                 (unsigned)ctx->ms.drink_making_flg,
                 (unsigned)ctx->ms.hot_water_flg,
                 (unsigned)ctx->ms.grind_run_flg,
                 steam_flag_name((uint8_t)ctx->ms.steam_flag),
                 ctx->ms.flow_rate);
        return false;
    }

    return (ctx->timer.tick - ctx->state_runtime.drink.finish_wait_tick) >=
           DRINK_FINISH_CONFIRM_TICKS;
}

static bool drink_handle_cancel_pending(app_ctx_t *ctx, const char *label)
{
    control_action_t stop_action;

    if (!ctx) {
        return false;
    }

    if (drink_cancel_confirmed(ctx, label)) {
        ESP_LOGI(TAG, "%s cancel confirmed -> READY", label ? label : "DRINK");
        ctx->core.state = ST_READY;
        return true;
    }

    if ((ctx->timer.tick - ctx->drink.target_time) >= DRINK_CANCEL_RETRY_TICKS) {
        stop_action = drink_stop_action_for(ctx->drink.target_drink);
        ctx->drink.target_time = ctx->timer.tick;
        ctr_cmd_action(stop_action, NULL);
        ESP_LOGI(TAG,
                 "%s cancel pending retry tick=%lu action=%d making=%u hot_water=%u grind=%u steam=%s flow=%.2f",
                 label ? label : "DRINK",
                 (unsigned long)ctx->timer.tick,
                 (int)stop_action,
                 (unsigned)ctx->ms.drink_making_flg,
                 (unsigned)ctx->ms.hot_water_flg,
                 (unsigned)ctx->ms.grind_run_flg,
                 steam_flag_name((uint8_t)ctx->ms.steam_flag),
                 ctx->ms.flow_rate);
    }

    ctx->input.key_pressed = 0U;
    ctx->input.key_long = 0U;
    ctx->drink.elapsed_tick = drink_elapsed_seconds(ctx);
    return false;
}

static bool drink_brew_finish_confirmed(app_ctx_t *ctx, const char *label)
{
    if (!ctx) {
        return false;
    }

    if (ctx->ms.drink_making_flg != DRINK_MAKER_NONE ||
        ctx->ms.flow_rate > DRINK_FINISH_IDLE_FLOW_MAX_ML_S) {
        if (ctx->state_runtime.drink.finish_wait_none_started) {
            ESP_LOGI(TAG,
                     "%s finish confirm cleared tick=%lu making=%u flow_rate=%.2f",
                     label ? label : "DRINK",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.drink_making_flg,
                     ctx->ms.flow_rate);
        }
        drink_clear_finish_wait(ctx);
        return false;
    }

    if (!ctx->state_runtime.drink.finish_wait_none_started) {
        ctx->state_runtime.drink.finish_wait_none_started = true;
        ctx->state_runtime.drink.finish_wait_tick = ctx->timer.tick;
        ESP_LOGI(TAG,
                 "%s finish confirm start tick=%lu liquid=%.1f flow_rate=%.2f",
                 label ? label : "DRINK",
                 (unsigned long)ctx->timer.tick,
                 ctx->ms.liquid_weight,
                 ctx->ms.flow_rate);
        return false;
    }

    return (ctx->timer.tick - ctx->state_runtime.drink.finish_wait_tick) >=
           DRINK_FINISH_CONFIRM_TICKS;
}

static bool drink_controller_self_abort_confirmed(app_ctx_t *ctx, const char *label)
{
    float raw_progress;

    if (!ctx) {
        return false;
    }

    if (ctx->core.substate != BREW_SUB_RUNNING_1) {
        return false;
    }

    switch (ctx->core.state) {
    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_AMERICANO:
    case ST_COLD_BREW:
        break;

    default:
        return false;
    }

    if (ctx->ms.drink_making_flg != DRINK_MAKER_NONE) {
        drink_clear_finish_wait(ctx);
        return false;
    }

    raw_progress = sp_pro_get_raw_liquid_progress_ml(ctx);
    if (ctx->state_runtime.drink.liquid_output_seen ||
        raw_progress > DRINK_SELF_ABORT_NO_OUTPUT_MAX_ML) {
        drink_clear_finish_wait(ctx);
        return false;
    }

    if (!ctx->state_runtime.drink.finish_wait_none_started) {
        ctx->state_runtime.drink.finish_wait_none_started = true;
        ctx->state_runtime.drink.finish_wait_tick = ctx->timer.tick;
        ESP_LOGW(TAG,
                 "%s controller self-stop observed tick=%lu liquid=%.1f raw=%.1f flow_rate=%.2f",
                 label ? label : "DRINK",
                 (unsigned long)ctx->timer.tick,
                 ctx->ms.liquid_weight,
                 raw_progress,
                 ctx->ms.flow_rate);
        return false;
    }

    return (ctx->timer.tick - ctx->state_runtime.drink.finish_wait_tick) >=
           DRINK_FINISH_CONFIRM_TICKS;
}

static inline void drink_return_ready(app_ctx_t *ctx)
{
    if (ctx && ctx->state_runtime.drink.parallel_steam_active) {
        drink_promote_parallel_steam(ctx);
        return;
    }

    ctx->core.substate = 0;
    ctx->core.state = ST_READY;
}

static bool drink_scale_cup_removal_cancelled(app_ctx_t *ctx)
{
    (void)ctx;
    /* TODO: implement cup-removal cancellation with a dedicated and stable cup-present sensor.
     * weight_fluid_flag only indicates the display unit source and is not reliable enough. */
    return false;
}

static bool drink_grind_sensor_cancelled(const app_ctx_t *ctx)
{
    if (!ctx || ctx->drink.target_drink != DRINK_GRIND) {
        return false;
    }

    if (ctx->core.substate != BREW_SUB_RUNNING_1) {
        return false;
    }

    if ((ctx->ms.grind_handle_postion_flag != 1U) &&
        !ctx->state_runtime.drink.grind_allow_without_handle) {
        return true;
    }

    return (ctx->ms.beanbox_in_place != 1U);
}

static bool drink_has_fault_cancel_condition(const app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

#if APP_TEST_DISABLE_LOCAL_ALARM
    return false;
#endif

    return ctx->ms.error_code != 0U;
}

static uint8_t drink_cancel_key_for(drink_type_t drink)
{
    switch (drink) {
    case DRINK_ESPRESSO:
        return KEY_ESPRESSO;
    case DRINK_MASTER:
        return KEY_ESPRESSO;
    case DRINK_AMERICANO:
        return KEY_AMERICANO;
    case DRINK_COLD_BREW:
        return KEY_COLD_BREW;
    case DRINK_WATER:
        return KEY_WATER;
    case DRINK_STEAM:
        return KEY_STEAM;
    case DRINK_GRIND:
        return KEY_GRIND;
    default:
        return KEY_MAX;
    }
}

static bool drink_cancel_requested_now(const app_ctx_t *ctx)
{
    uint16_t pressed_or_long;
    uint8_t cancel_key;

    if (!ctx) {
        return false;
    }

    pressed_or_long = (uint16_t)(ctx->input.key_pressed | ctx->input.key_long);
    if (pressed_or_long == 0U) {
        return false;
    }

    if (drink_is_remote_flow(ctx)) {
        return (pressed_or_long & (1U << KEY_ESPRESSO)) != 0U;
    }

    cancel_key = drink_cancel_key_for(ctx->drink.target_drink);
    return ctx->drink.target_drink != DRINK_STEAM &&
           cancel_key < KEY_MAX &&
           ((pressed_or_long & (1U << cancel_key)) != 0U);
}

static control_action_t drink_stop_action_for(drink_type_t drink)
{
    switch (drink) {
    case DRINK_STEAM:
        return CTRL_ACT_STEAM_STOP;
    case DRINK_GRIND:
        return CTRL_ACT_GRIND_STOP;
    case DRINK_ESPRESSO:
    case DRINK_MASTER:
    case DRINK_AMERICANO:
    case DRINK_COLD_BREW:
    case DRINK_WATER:
    default:
        return CTRL_ACT_CANCEL;
    }
}

static bool drink_supports_parallel_steam(const app_ctx_t *ctx)
{
    if (!ctx || drink_is_remote_flow(ctx)) {
        return false;
    }

    if (ctx->core.substate != BREW_SUB_PREPARE &&
        ctx->core.substate != BREW_SUB_REMOTE_COUNTDOWN &&
        ctx->core.substate != BREW_SUB_RUNNING_1) {
        return false;
    }

    switch (ctx->core.state) {
    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_COLD_BREW:
        return true;

    case ST_AMERICANO:
        return true;

    default:
        return false;
    }
}

static void drink_clear_parallel_steam(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.drink.parallel_steam_active = false;
    ctx->state_runtime.drink.parallel_steam_start_tick = 0U;
}

static bool drink_start_parallel_steam(app_ctx_t *ctx)
{
    if (!ctx ||
        ctx->state_runtime.drink.parallel_steam_active ||
        !drink_supports_parallel_steam(ctx)) {
        return false;
    }

    ctx->ctrl.src = CTRL_SRC_UI;
    if (!ctr_cmd_action(CTRL_ACT_STEAM_START, NULL)) {
        ESP_LOGW(TAG,
                 "Parallel steam start rejected state=%d sub=%d steam_flag=%s",
                 ctx->core.state,
                 ctx->core.substate,
                 steam_flag_name((uint8_t)ctx->ms.steam_flag));
        return false;
    }

    ctx->state_runtime.drink.parallel_steam_active = true;
    ctx->state_runtime.drink.parallel_steam_start_tick = ctx->timer.tick;
    ctx->state_runtime.drink.last_steam_flag = (uint8_t)ctx->ms.steam_flag;
    ctx->state_runtime.drink.last_steam_log_sec = 0xFFFFFFFFU;
    ESP_LOGI(TAG,
             "Parallel steam start state=%d sub=%d steam_flag=%s",
             ctx->core.state,
             ctx->core.substate,
             steam_flag_name((uint8_t)ctx->ms.steam_flag));
    return true;
}

static void drink_promote_parallel_steam(app_ctx_t *ctx)
{
    if (!ctx || !ctx->state_runtime.drink.parallel_steam_active) {
        return;
    }

    ctx->core.state = ST_STEAM;
    ctx->core.substate = (ctx->ms.steam_flag == STEAM_RUNNING) ?
                         BREW_SUB_RUNNING_1 :
                         BREW_SUB_PREPARE;
    ctx->drink.target_drink = DRINK_STEAM;
    ctx->drink.steam_level = ctx->setting.steam_level;
    ctx->drink.start_tick = (ctx->state_runtime.drink.parallel_steam_start_tick > 0U) ?
                            ctx->state_runtime.drink.parallel_steam_start_tick :
                            ctx->timer.tick;
    ctx->state_runtime.drink.steam_started = true;
    ctx->state_runtime.drink.last_steam_flag = (uint8_t)ctx->ms.steam_flag;
    ctx->state_runtime.drink.last_steam_log_sec = 0xFFFFFFFFU;
    drink_clear_parallel_steam(ctx);
    ESP_LOGI(TAG,
             "Promote parallel steam to standalone sub=%d steam_flag=%s",
             ctx->core.substate,
             steam_flag_name((uint8_t)ctx->ms.steam_flag));
}

static void drink_abort_active_process(app_ctx_t *ctx, drink_exit_reason_t reason)
{
    control_action_t stop_action = drink_stop_action_for(ctx->drink.target_drink);

    ctx->state_runtime.drink.exit_reason = reason;
    if (drink_is_remote_flow(ctx) && ctx->state_runtime.drink.remote_action_started) {
        ctx->ctrl.src = CTRL_SRC_MQTT;
        ctr_cmd_action(stop_action, NULL);
    } else if (!drink_is_remote_flow(ctx)) {
        ctr_cmd_action(stop_action, NULL);
    }
    ctx->timer.standby_timer = ctx->timer.tick + 9000;
}

static bool steam_stop_requested(app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    if ((ctx->input.key_pressed & (1U << KEY_STEAM)) != 0U) {
        ctx->input.key_pressed &= ~(1U << KEY_STEAM);
        ESP_LOGI(TAG, "Steam stop by steam key");
        return true;
    }

    if (ctx->ms.encoder.evt_seq != ctx->state_runtime.drink.last_encoder_evt_seq) {
        ctx->state_runtime.drink.last_encoder_evt_seq = ctx->ms.encoder.evt_seq;
        if (ctx->ms.encoder.evt_type == ENC_EVT_CLICK) {
            ESP_LOGI(TAG, "Steam stop by encoder click");
            return true;
        }
    }

    return false;
}

static void steam_enter_tail_spray_wait(app_ctx_t *ctx)
{
    uint32_t steam_run_ticks;

    if (!ctx) {
        return;
    }

    steam_run_ticks = ctx->timer.tick - ctx->drink.start_tick;
    if (steam_run_ticks < STEAM_TAIL_SPRAY_MIN_RUN_TICKS) {
        ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
        ESP_LOGI(TAG,
                 "STEAM skip tail-spray tick=%lu run_elapsed=%lus threshold=%lus",
                 (unsigned long)ctx->timer.tick,
                 (unsigned long)(steam_run_ticks / STAY_TICKS_1S),
                 (unsigned long)(STEAM_TAIL_SPRAY_MIN_RUN_TICKS / STAY_TICKS_1S));
        drink_return_ready(ctx);
        return;
    }

    ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
    ctx->drink.finish_tick = ctx->timer.tick;
    ctx->drink.target_time = ctx->timer.tick;
    ctx->core.substate = BREW_SUB_WAIT_TAIL_SPRAY;
}

static void steam_enter_tail_spray_countdown(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->drink.target_time = ctx->timer.tick;
    ctx->core.substate = BREW_SUB_TAIL_SPRAY_COUNTDOWN;
    ctx->state_runtime.drink.last_steam_log_sec = 0xFFFFFFFFU;
    voice_manager_play_interrupt(VOICE_CAUTIONHOTSTEAM);
    ESP_LOGI(TAG,
             "STEAM tail-wait -> tail-countdown tick=%lu steam_flag=%s pressure=%.2f",
             (unsigned long)ctx->timer.tick,
             steam_flag_name((uint8_t)ctx->ms.steam_flag),
             ctx->ms.pressure);
}

static bool drink_handle_finish_state(app_ctx_t *ctx)
{
    if (drink_finish_delay_done(ctx)) {
        drink_return_ready(ctx);
        return true;
    }

    return false;
}

static app_state_t drink_leave_state(app_ctx_t *ctx,
                                     app_state_t active_state,
                                     bool *started)
{
    if (ctx->core.state != active_state) {
        drink_exit_reason_t exit_reason = ctx->state_runtime.drink.exit_reason;
        bool remote_flow = drink_is_remote_flow(ctx);
        bool remote_action_started = ctx->state_runtime.drink.remote_action_started;

        if (started) {
            *started = false;
        }

        if (remote_flow) {
            switch (exit_reason) {
            case DRINK_EXIT_CANCEL:
            case DRINK_EXIT_SWITCH_TO_STEAM:
                if (remote_action_started) {
                    device_statistics_notify_remote_cancel();
                    drink_record_notify_remote_cancel();
                    extraction_curve_notify_remote_cancel();
                }
                break;

            case DRINK_EXIT_FAIL:
                if (remote_action_started) {
                    device_statistics_notify_remote_cancel();
                    drink_record_notify_remote_fail();
                    extraction_curve_notify_remote_fail();
                }
                break;

            case DRINK_EXIT_NONE:
            default:
                break;
            }
        } else {
            switch (exit_reason) {
            case DRINK_EXIT_CANCEL:
            case DRINK_EXIT_SWITCH_TO_STEAM:
                device_statistics_notify_local_state_cancel(active_state);
                drink_record_notify_local_state_cancel(active_state);
                extraction_curve_notify_local_state_cancel(active_state);
                break;

            case DRINK_EXIT_FAIL:
                device_statistics_notify_local_state_cancel(active_state);
                drink_record_notify_local_state_fail(active_state);
                extraction_curve_notify_local_state_fail(active_state);
                break;

            case DRINK_EXIT_NONE:
            default:
                break;
            }
        }

        drink_clear_remote_flow(ctx);
        ctx->state_runtime.drink.exit_reason = DRINK_EXIT_NONE;
        return ctx->core.state;
    }

    return active_state;
}

app_state_t state_handle_espresso(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.espresso_started;
    uint32_t elapsed_sec = drink_elapsed_seconds(ctx);

    if (!*started) {
        if (drink_is_remote_flow(ctx)) {
            drink_enter_remote_countdown(ctx, false);
        } else {
            app_beverage_settings_t settings = {0};
            sp_pro_app_get_beverage_settings(&settings);
            drink_prepare_local_entry(ctx, true);
            device_statistics_notify_local_state_start(ST_ESPRESSO);
            drink_record_notify_local_state_start(ST_ESPRESSO, &settings);
            extraction_curve_notify_local_state_start(ST_ESPRESSO, &settings);
        }
        *started = true;
    }

    switch (ctx->core.substate) {
    case BREW_SUB_REMOTE_COUNTDOWN:
        if (drink_remote_countdown_done(ctx)) {
            if (!drink_start_remote_action(ctx, CTRL_ACT_ESPRESSO, ST_ESPRESSO, BREW_SUB_PREPARE)) {
                drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
                ctx->core.state = ST_READY;
            }
        }
        break;

    case BREW_SUB_PREPARE:
        if (!drink_is_remote_flow(ctx) && drink_cancel_requested_now(ctx)) {
            drink_enter_cancel_pending(ctx);
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
            break;
        }

        if (!drink_is_remote_flow(ctx) &&
            !ctx->state_runtime.drink.prepare_cmd_sent &&
            sp_pro_app_is_brew_handle_in_place()) {
            drink_prepare_send_local_action(ctx, CTRL_ACT_ESPRESSO);
        }

        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "ESP prepare tick=%lu elapsed=%lus making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     (unsigned)ctx->ms.drink_making_flg,
                     ctx->ms.flow_rate,
                     ctx->ms.hot_current_temp,
                     ctx->ms.liquid_weight,
                     (unsigned)ctx->drink.target_ml);
        }

        if (check_drink_flag_ready(ctx)) {
            ESP_LOGI(TAG,
                     "ESP prepare -> running tick=%lu making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.drink_making_flg,
                     ctx->ms.flow_rate,
                     ctx->ms.hot_current_temp,
                     ctx->ms.liquid_weight,
                     (unsigned)ctx->drink.target_ml);
            drink_enter_running(ctx, BREW_SUB_RUNNING_1);
            ctx->state_runtime.drink.last_water_log_sec = 0xFFFFFFFFU;
        }
        break;

    case BREW_SUB_CANCEL_PENDING:
        if (drink_handle_cancel_pending(ctx, "ESP")) {
            return drink_leave_state(ctx, ST_ESPRESSO, started);
        }
        break;

    case BREW_SUB_RUNNING_1:
        elapsed_sec = drink_elapsed_seconds(ctx);
        drink_rebase_if_counter_reset(ctx, "ESP");
        if (!ctx->state_runtime.drink.liquid_output_seen &&
            (sp_pro_get_raw_liquid_progress_ml(ctx) > 0.1f ||
             ctx->ms.flow_rate > WATER_DISPLAY_FLOW_MIN_ML_S)) {
            ctx->state_runtime.drink.liquid_output_seen = true;
            ESP_LOGI(TAG,
                     "ESP first liquid output tick=%lu elapsed=%lus liquid=%.1f raw=%.1f display=%.1f flow_rate=%.2f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     ctx->ms.flow_rate,
                     (unsigned)ctx->drink.target_ml);
        }
        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "ESP running tick=%lu elapsed=%lus liquid=%.1f raw=%.1f display=%.1f flow_rate=%.2f target=%u making_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     ctx->ms.flow_rate,
                     (unsigned)ctx->drink.target_ml,
                     (unsigned)ctx->ms.drink_making_flg);
        }
        if (drink_local_target_reached(ctx, "ESP")) {
            ESP_LOGE(TAG, "ESP LOCAL TARGET FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_ESPRESSO);
                extraction_curve_notify_local_state_success(ST_ESPRESSO);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        } else if (ctx->state_runtime.drink.liquid_output_seen &&
                   drink_brew_finish_confirmed(ctx, "ESP")) {
            ESP_LOGE(TAG, "ESP FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_ESPRESSO);
                extraction_curve_notify_local_state_success(ST_ESPRESSO);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        }
        break;

    case BREW_SUB_FINISH:
        drink_handle_finish_state(ctx);
        break;

    default:
        break;
    }

    if (ctx->core.substate != BREW_SUB_CANCEL_PENDING) {
        handle_making_base(ctx);
    }
    return drink_leave_state(ctx, ST_ESPRESSO, started);
}

app_state_t state_handle_master(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.master_started;

    if (!*started) {
        if (drink_is_remote_flow(ctx)) {
            drink_enter_remote_countdown(ctx, false);
        } else {
            app_beverage_settings_t settings = {0};
            sp_pro_app_get_beverage_settings(&settings);
            drink_prepare_local_entry(ctx, true);
            device_statistics_notify_local_state_start(ST_MASTER);
            drink_record_notify_local_state_start(ST_MASTER, &settings);
            extraction_curve_notify_local_state_start(ST_MASTER, &settings);
        }
        *started = true;
    }

    switch (ctx->core.substate) {
    case BREW_SUB_REMOTE_COUNTDOWN:
        if (drink_remote_countdown_done(ctx)) {
            if (!drink_start_remote_action(ctx, CTRL_ACT_ESPRESSO, ST_MASTER, BREW_SUB_PREPARE)) {
                drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
                ctx->core.state = ST_READY;
            }
        }
        break;

    case BREW_SUB_PREPARE:
        if (!drink_is_remote_flow(ctx) && drink_cancel_requested_now(ctx)) {
            drink_enter_cancel_pending(ctx);
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
            break;
        }

        if (!drink_is_remote_flow(ctx) &&
            !ctx->state_runtime.drink.prepare_cmd_sent &&
            sp_pro_app_is_brew_handle_in_place()) {
            drink_prepare_send_local_action(ctx, CTRL_ACT_ESPRESSO);
        }

        if (check_drink_flag_ready(ctx)) {
            drink_enter_running(ctx, BREW_SUB_RUNNING_1);
        }
        break;

    case BREW_SUB_CANCEL_PENDING:
        if (drink_handle_cancel_pending(ctx, "MASTER")) {
            return drink_leave_state(ctx, ST_MASTER, started);
        }
        break;

    case BREW_SUB_RUNNING_1:
        drink_rebase_if_counter_reset(ctx, "MASTER");
        if (drink_local_target_reached(ctx, "MASTER")) {
            ESP_LOGE(TAG, "MASTER LOCAL TARGET FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_MASTER);
                extraction_curve_notify_local_state_success(ST_MASTER);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        } else if (ctx->state_runtime.drink.liquid_output_seen &&
                   drink_brew_finish_confirmed(ctx, "MASTER")) {
            ESP_LOGE(TAG, "MASTER FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_MASTER);
                extraction_curve_notify_local_state_success(ST_MASTER);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        }
        break;

    case BREW_SUB_FINISH:
        drink_handle_finish_state(ctx);
        break;

    default:
        break;
    }

    if (ctx->core.substate != BREW_SUB_CANCEL_PENDING) {
        handle_making_base(ctx);
    }
    return drink_leave_state(ctx, ST_MASTER, started);
}

app_state_t state_handle_americano(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.americano_started;
    bool *wait_none_started = &ctx->state_runtime.drink.americano_wait_none_started;
    uint32_t elapsed_sec = drink_elapsed_seconds(ctx);

    if (!*started) {
        if (drink_is_remote_flow(ctx)) {
            drink_enter_remote_countdown(ctx, false);
        } else {
            app_beverage_settings_t settings = {0};
            sp_pro_app_get_beverage_settings(&settings);
            drink_prepare_local_entry(ctx, true);
            device_statistics_notify_local_state_start(ST_AMERICANO);
            drink_record_notify_local_state_start(ST_AMERICANO, &settings);
            extraction_curve_notify_local_state_start(ST_AMERICANO, &settings);
        }
        *started = true;
        *wait_none_started = false;
    }

    switch (ctx->core.substate) {
    case BREW_SUB_REMOTE_COUNTDOWN:
        if (drink_remote_countdown_done(ctx)) {
            if (!drink_start_remote_action(ctx, CTRL_ACT_AMERICANO_BREW, ST_AMERICANO, BREW_SUB_PREPARE)) {
                drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
                ctx->core.state = ST_READY;
            }
        }
        break;

    case BREW_SUB_PREPARE:
        if (!drink_is_remote_flow(ctx) && drink_cancel_requested_now(ctx)) {
            drink_enter_cancel_pending(ctx);
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
            break;
        }

        if (drink_is_remote_flow(ctx)) {
            if (!sp_pro_app_is_brew_handle_in_place()) {
                break;
            }

            if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
                ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
                ESP_LOGI(TAG,
                         "AME_BREW prepare tick=%lu elapsed=%lus making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                         (unsigned long)ctx->timer.tick,
                         (unsigned long)elapsed_sec,
                         (unsigned)ctx->ms.drink_making_flg,
                         ctx->ms.flow_rate,
                         ctx->ms.hot_current_temp,
                         ctx->ms.liquid_weight,
                         (unsigned)ctx->drink.target_ml);
            }

            if (check_drink_flag_ready(ctx)) {
                drink_apply_remote_targets(ctx, false);
                ESP_LOGI(TAG,
                         "AME_BREW prepare -> running tick=%lu making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                         (unsigned long)ctx->timer.tick,
                         (unsigned)ctx->ms.drink_making_flg,
                         ctx->ms.flow_rate,
                         ctx->ms.hot_current_temp,
                         ctx->ms.liquid_weight,
                         (unsigned)ctx->drink.target_ml);
                drink_enter_running(ctx, BREW_SUB_RUNNING_1);
                ctx->state_runtime.drink.last_water_log_sec = 0xFFFFFFFFU;
            }
        } else {
            if (!ctx->state_runtime.drink.prepare_cmd_sent &&
                sp_pro_app_is_brew_handle_in_place()) {
                if (drink_cancel_requested_now(ctx)) {
                    break;
                }
                drink_prepare_send_local_action(ctx, CTRL_ACT_AMERICANO_BREW);
            }

            if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
                ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
                ESP_LOGI(TAG,
                         "AME_BREW prepare tick=%lu elapsed=%lus making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                         (unsigned long)ctx->timer.tick,
                         (unsigned long)elapsed_sec,
                         (unsigned)ctx->ms.drink_making_flg,
                         ctx->ms.flow_rate,
                         ctx->ms.hot_current_temp,
                         ctx->ms.liquid_weight,
                         (unsigned)ctx->drink.target_ml);
            }

            if (check_drink_flag_ready(ctx)) {
                ESP_LOGI(TAG,
                         "AME_BREW prepare -> running tick=%lu making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                         (unsigned long)ctx->timer.tick,
                         (unsigned)ctx->ms.drink_making_flg,
                         ctx->ms.flow_rate,
                         ctx->ms.hot_current_temp,
                         ctx->ms.liquid_weight,
                         (unsigned)ctx->drink.target_ml);
                drink_enter_running(ctx, BREW_SUB_RUNNING_1);
                ctx->state_runtime.drink.last_water_log_sec = 0xFFFFFFFFU;
            }
        }
        break;

    case BREW_SUB_CANCEL_PENDING:
        if (drink_handle_cancel_pending(ctx, "AME")) {
            return drink_leave_state(ctx, ST_AMERICANO, started);
        }
        break;

    case BREW_SUB_RUNNING_1:
        elapsed_sec = drink_elapsed_seconds(ctx);
        drink_rebase_if_counter_reset(ctx, "AME_BREW");
        if (!ctx->state_runtime.drink.liquid_output_seen &&
            (sp_pro_get_raw_liquid_progress_ml(ctx) > 0.1f ||
             ctx->ms.flow_rate > WATER_DISPLAY_FLOW_MIN_ML_S)) {
            ctx->state_runtime.drink.liquid_output_seen = true;
            ESP_LOGI(TAG,
                     "AME_BREW first liquid output tick=%lu elapsed=%lus liquid=%.1f raw=%.1f display=%.1f flow_rate=%.2f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     ctx->ms.flow_rate,
                     (unsigned)ctx->drink.target_ml);
        }
        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "AME_BREW running tick=%lu elapsed=%lus liquid=%.1f raw=%.1f display=%.1f flow_rate=%.2f target=%u making_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     ctx->ms.flow_rate,
                     (unsigned)ctx->drink.target_ml,
                     (unsigned)ctx->ms.drink_making_flg);
        }
        if (drink_is_remote_flow(ctx)) {
            if (ctx->ms.drink_making_flg != DRINK_MAKER_NONE) {
                *wait_none_started = false;
                drink_clear_finish_wait(ctx);
                break;
            }

            if (!(ctx->state_runtime.drink.liquid_output_seen &&
                  drink_brew_finish_confirmed(ctx, "AME_BREW_REMOTE"))) {
                break;
            }

            if (!*wait_none_started) {
                ctx->drink.target_time = ctx->timer.tick;
                *wait_none_started = true;
                ESP_LOGI(TAG, "AMERICANO remote transition wait start=%lu", (unsigned long)ctx->drink.target_time);
            }

            if (ctx->timer.tick - ctx->drink.target_time >= STAY_TICKS_2S) {
                ctx->ctrl.src = CTRL_SRC_MQTT;
                if (!ctr_cmd_action(CTRL_ACT_AMERICANO_WATER, &ctx->ctrl.remote_formula)) {
                    drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
                    ctx->core.state = ST_READY;
                    break;
                }

                drink_apply_remote_targets(ctx, true);
                drink_capture_liquid_session_base(ctx);
                drink_water_display_reset(ctx);
                ctx->state_runtime.drink.americano_water_started = false;
                drink_clear_finish_wait(ctx);
                ctx->core.substate = BREW_SUB_RUNNING_2;
                ctx->drink.target_time = ctx->timer.tick;
                *wait_none_started = false;
                ESP_LOGI(TAG,
                         "AMERICANO remote water stage start target_ml=%u target_temp=%.1f",
                         (unsigned)ctx->drink.target_ml,
                         ctx->drink.target_temp);
            }
        } else {
            bool brew_target_hit = drink_local_target_reached(ctx, "AME_BREW");

            if (!brew_target_hit &&
                ctx->ms.drink_making_flg != DRINK_MAKER_NONE) {
                *wait_none_started = false;
                drink_clear_finish_wait(ctx);
                break;
            }

            if (!brew_target_hit &&
                !(ctx->state_runtime.drink.liquid_output_seen &&
                  drink_brew_finish_confirmed(ctx, "AME_BREW"))) {
                break;
            }

            if (!*wait_none_started) {
                ctr_cmd_action(CTRL_ACT_CANCEL, NULL);
                ctx->drink.target_time = ctx->timer.tick;
                *wait_none_started = true;
                ESP_LOGE(TAG, "AMERICANO transition wait start=%lu", (unsigned long)ctx->drink.target_time);
            }

            if (ctx->timer.tick - ctx->drink.target_time >= STAY_TICKS_2S) {
                if (drink_cancel_requested_now(ctx)) {
                    break;
                }
                ctx->ctrl.src = CTRL_SRC_UI;
                ctr_cmd_action(CTRL_ACT_AMERICANO_WATER, NULL);
                ctx->drink.target_ml = (uint16_t)(ctx->setting.ame_water_w + 0.5f);
                ctx->drink.target_temp = ctx->setting.ame_water_t;
                drink_capture_liquid_session_base(ctx);
                drink_water_display_reset(ctx);
                ctx->state_runtime.drink.americano_water_started = false;
                drink_clear_finish_wait(ctx);
                ctx->core.substate = BREW_SUB_RUNNING_2;
                ctx->drink.target_time = ctx->timer.tick;
                *wait_none_started = false;
            }
        }
        break;

    case BREW_SUB_RUNNING_2:
        drink_water_rebase_if_counter_reset(ctx, "AME_WATER");
        drink_water_display_update(ctx);
        if (!ctx->state_runtime.drink.americano_water_started &&
            (ctx->ms.hot_water_flg != 0U ||
             sp_pro_get_raw_liquid_progress_ml(ctx) > 0.1f)) {
            ctx->state_runtime.drink.americano_water_started = true;
            ESP_LOGI(TAG,
                     "AME_WATER stage started tick=%lu hot_water_flg=%u flow_rate=%.2f raw=%.1f display=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.hot_water_flg,
                     ctx->ms.flow_rate,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     (unsigned)ctx->drink.target_ml);
        }

        if (drink_local_target_reached(ctx, "AME_WATER")) {
            ESP_LOGE(TAG, "AME LOCAL TARGET FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_AMERICANO);
                extraction_curve_notify_local_state_success(ST_AMERICANO);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        } else if (ctx->state_runtime.drink.americano_water_started &&
                   drink_hot_water_finish_confirmed(ctx, "AME_WATER")) {
            ESP_LOGE(TAG, "AME FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_AMERICANO);
                extraction_curve_notify_local_state_success(ST_AMERICANO);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        }
        break;

    case BREW_SUB_FINISH:
        drink_handle_finish_state(ctx);
        break;

    default:
        break;
    }

    if (ctx->core.substate != BREW_SUB_CANCEL_PENDING) {
        handle_making_base(ctx);
    }

    if (ctx->core.state != ST_AMERICANO) {
        *wait_none_started = false;
    }

    return drink_leave_state(ctx, ST_AMERICANO, started);
}

app_state_t state_handle_cold_brew(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.cold_brew_started;
    uint32_t elapsed_sec = drink_elapsed_seconds(ctx);

    if (!*started) {
        if (drink_is_remote_flow(ctx)) {
            drink_enter_remote_countdown(ctx, false);
        } else {
            app_beverage_settings_t settings = {0};
            sp_pro_app_get_beverage_settings(&settings);
            drink_prepare_local_entry(ctx, true);
            device_statistics_notify_local_state_start(ST_COLD_BREW);
            drink_record_notify_local_state_start(ST_COLD_BREW, &settings);
            extraction_curve_notify_local_state_start(ST_COLD_BREW, &settings);
        }
        *started = true;
    }

    switch (ctx->core.substate) {
    case BREW_SUB_REMOTE_COUNTDOWN:
        if (drink_remote_countdown_done(ctx)) {
            if (!drink_start_remote_action(ctx, CTRL_ACT_COLD_BREW, ST_COLD_BREW, BREW_SUB_PREPARE)) {
                drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
                ctx->core.state = ST_READY;
            }
        }
        break;

    case BREW_SUB_PREPARE:
        if (!drink_is_remote_flow(ctx) && drink_cancel_requested_now(ctx)) {
            drink_enter_cancel_pending(ctx);
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
            break;
        }

        if (!drink_is_remote_flow(ctx) &&
            !ctx->state_runtime.drink.prepare_cmd_sent &&
            sp_pro_app_is_brew_handle_in_place()) {
            drink_prepare_send_local_action(ctx, CTRL_ACT_COLD_BREW);
        }

        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "COLD prepare tick=%lu elapsed=%lus making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     (unsigned)ctx->ms.drink_making_flg,
                     ctx->ms.flow_rate,
                     ctx->ms.hot_current_temp,
                     ctx->ms.liquid_weight,
                     (unsigned)ctx->drink.target_ml);
        }

        if (check_drink_flag_ready(ctx)) {
            ESP_LOGI(TAG,
                     "COLD prepare -> running tick=%lu making_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.drink_making_flg,
                     ctx->ms.flow_rate,
                     ctx->ms.hot_current_temp,
                     ctx->ms.liquid_weight,
                     (unsigned)ctx->drink.target_ml);
            drink_enter_running(ctx, BREW_SUB_RUNNING_1);
            ctx->state_runtime.drink.last_water_log_sec = 0xFFFFFFFFU;
        }
        break;

    case BREW_SUB_CANCEL_PENDING:
        if (drink_handle_cancel_pending(ctx, "COLD")) {
            return drink_leave_state(ctx, ST_COLD_BREW, started);
        }
        break;

    case BREW_SUB_RUNNING_1:
        drink_rebase_if_counter_reset(ctx, "COLD");
        elapsed_sec = drink_elapsed_seconds(ctx);
        if (!ctx->state_runtime.drink.liquid_output_seen &&
            (sp_pro_get_raw_liquid_progress_ml(ctx) > 0.1f ||
             ctx->ms.flow_rate > WATER_DISPLAY_FLOW_MIN_ML_S)) {
            ctx->state_runtime.drink.liquid_output_seen = true;
            ESP_LOGI(TAG,
                     "COLD first liquid output tick=%lu elapsed=%lus liquid=%.1f raw=%.1f display=%.1f flow_rate=%.2f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     ctx->ms.flow_rate,
                     (unsigned)ctx->drink.target_ml);
        }
        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "COLD running tick=%lu elapsed=%lus liquid=%.1f raw=%.1f display=%.1f flow_rate=%.2f target=%u making_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     sp_pro_get_raw_liquid_progress_ml(ctx),
                     sp_pro_get_display_liquid_ml(ctx),
                     ctx->ms.flow_rate,
                     (unsigned)ctx->drink.target_ml,
                     (unsigned)ctx->ms.drink_making_flg);
        }
        if (drink_local_target_reached(ctx, "COLD")) {
            ESP_LOGE(TAG, "COLD LOCAL TARGET FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_COLD_BREW);
                extraction_curve_notify_local_state_success(ST_COLD_BREW);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        } else if (ctx->state_runtime.drink.liquid_output_seen &&
                   drink_brew_finish_confirmed(ctx, "COLD")) {
            ESP_LOGE(TAG, "COLD FINISH");
            if (!drink_is_remote_flow(ctx)) {
                drink_record_notify_local_state_success(ST_COLD_BREW);
                extraction_curve_notify_local_state_success(ST_COLD_BREW);
            }
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        }
        break;

    case BREW_SUB_FINISH:
        drink_handle_finish_state(ctx);
        break;

    default:
        break;
    }

    if (ctx->core.substate != BREW_SUB_CANCEL_PENDING) {
        handle_making_base(ctx);
    }
    return drink_leave_state(ctx, ST_COLD_BREW, started);
}

app_state_t state_handle_water(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.water_started;
    uint32_t elapsed_sec;

    if (!*started) {
        if (drink_is_remote_flow(ctx)) {
            drink_enter_remote_countdown(ctx, false);
        } else {
            drink_begin_prepare(ctx, CTRL_ACT_HOT_WATER, true);
            drink_water_display_reset(ctx);
            drink_water_capture_start_snapshot(ctx);
            device_statistics_notify_local_state_start(ST_WATER);
            ESP_LOGI(TAG,
                     "WATER start tick=%lu target_ml=%u target_temp=%.1f hot_water_flg=%u temp=%.1f liquid=%.1f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->drink.target_ml,
                     ctx->drink.target_temp,
                     (unsigned)ctx->ms.hot_water_flg,
                     ctx->ms.hot_current_temp,
                     ctx->ms.liquid_weight);
        }
        *started = true;
    }

    elapsed_sec = (ctx->timer.tick - ctx->drink.start_tick) / STAY_TICKS_1S;

    switch (ctx->core.substate) {
    case BREW_SUB_REMOTE_COUNTDOWN:
        if (drink_remote_countdown_done(ctx)) {
            if (!drink_start_remote_action(ctx, CTRL_ACT_HOT_WATER, ST_WATER, BREW_SUB_PREPARE)) {
                drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
                ctx->core.state = ST_READY;
            } else {
                drink_water_display_reset(ctx);
                drink_water_capture_start_snapshot(ctx);
            }
        }
        break;

    case BREW_SUB_PREPARE:
        if (!drink_is_remote_flow(ctx) && drink_cancel_requested_now(ctx)) {
            drink_enter_cancel_pending(ctx);
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
            break;
        }

        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "WATER prepare tick=%lu elapsed=%lus hot_water_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     (unsigned)ctx->ms.hot_water_flg,
                     ctx->ms.flow_rate,
                     ctx->ms.hot_current_temp,
                     ctx->ms.liquid_weight,
                     (unsigned)ctx->drink.target_ml);
        }
        if (check_water_flag_ready(ctx)) {
            ESP_LOGI(TAG,
                     "WATER prepare -> running tick=%lu hot_water_flg=%u flow_rate=%.2f temp=%.1f liquid=%.1f target=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.hot_water_flg,
                     ctx->ms.flow_rate,
                      ctx->ms.hot_current_temp,
                      ctx->ms.liquid_weight,
                      (unsigned)ctx->drink.target_ml);
            drink_enter_running(ctx, BREW_SUB_RUNNING_1);
            ctx->state_runtime.drink.last_water_log_sec = 0xFFFFFFFFU;
        }
        break;

    case BREW_SUB_CANCEL_PENDING:
        if (drink_handle_cancel_pending(ctx, "WATER")) {
            return drink_leave_state(ctx, ST_WATER, started);
        }
        break;

    case BREW_SUB_RUNNING_1:
        elapsed_sec = drink_elapsed_seconds(ctx);
        drink_water_rebase_if_counter_reset(ctx, "WATER");
        drink_water_display_update(ctx);
        if (!ctx->state_runtime.drink.liquid_output_seen &&
            (ctx->state_runtime.drink.display_liquid_ml > 0.1f ||
             ctx->ms.flow_rate > WATER_DISPLAY_FLOW_MIN_ML_S)) {
            ctx->state_runtime.drink.liquid_output_seen = true;
            ESP_LOGI(TAG,
                     "WATER first liquid output tick=%lu elapsed=%lus liquid=%.1f display=%.1f target=%u temp=%.1f hot_water_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     ctx->state_runtime.drink.display_liquid_ml,
                     (unsigned)ctx->drink.target_ml,
                     ctx->ms.hot_current_temp,
                     (unsigned)ctx->ms.hot_water_flg);
        }
        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "WATER running tick=%lu elapsed=%lus liquid=%.1f display=%.1f flow_rate=%.2f target=%u temp=%.1f hot_water_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     ctx->ms.liquid_weight,
                     ctx->state_runtime.drink.display_liquid_ml,
                     ctx->state_runtime.drink.display_flow_rate_ml_s,
                     (unsigned)ctx->drink.target_ml,
                     ctx->ms.hot_current_temp,
                     (unsigned)ctx->ms.hot_water_flg);
        }
        if (drink_local_target_reached(ctx, "WATER")) {
            ESP_LOGI(TAG,
                     "WATER running -> finish tick=%lu liquid=%.1f display=%.1f target=%u temp=%.1f hot_water_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     ctx->ms.liquid_weight,
                     sp_pro_get_display_liquid_ml(ctx),
                      (unsigned)ctx->drink.target_ml,
                      ctx->ms.hot_current_temp,
                      (unsigned)ctx->ms.hot_water_flg);
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        } else if (ctx->state_runtime.drink.liquid_output_seen &&
                   drink_hot_water_finish_confirmed(ctx, "WATER")) {
            ESP_LOGI(TAG,
                     "WATER controller-side finish -> finish tick=%lu liquid=%.1f display=%.1f target=%u temp=%.1f hot_water_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     ctx->ms.liquid_weight,
                     sp_pro_get_display_liquid_ml(ctx),
                     (unsigned)ctx->drink.target_ml,
                     ctx->ms.hot_current_temp,
                     (unsigned)ctx->ms.hot_water_flg);
            drink_enter_finish(ctx, CTRL_ACT_CANCEL);
        }
        break;

    case BREW_SUB_FINISH:
        if ((ctx->input.key_pressed & (1U << KEY_WATER)) != 0U) {
            ctx->input.key_pressed &= ~(1U << KEY_WATER);
            drink_water_restart(ctx);
            break;
        }
        drink_water_display_update(ctx);
        if (ctx->state_runtime.drink.last_water_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_water_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "WATER finish-wait tick=%lu elapsed=%lus finish_age=%lu hot_water_flg=%u liquid=%.1f display=%.1f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     (unsigned long)(ctx->timer.tick - ctx->drink.finish_tick),
                     (unsigned)ctx->ms.hot_water_flg,
                     ctx->ms.liquid_weight,
                     ctx->state_runtime.drink.display_liquid_ml);
        }
        if (drink_handle_finish_state(ctx)) {
            ESP_LOGI(TAG,
                     "WATER finish -> ready tick=%lu final_liquid=%.1f final_temp=%.1f hot_water_flg=%u",
                     (unsigned long)ctx->timer.tick,
                     ctx->ms.liquid_weight,
                     ctx->ms.hot_current_temp,
                     (unsigned)ctx->ms.hot_water_flg);
        }
        break;

    default:
        break;
    }

    if (ctx->core.substate != BREW_SUB_CANCEL_PENDING) {
        handle_making_base(ctx);
    }
    return drink_leave_state(ctx, ST_WATER, started);
}

app_state_t state_handle_steam(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.steam_started;
    uint32_t elapsed_sec;

    if (!*started) {
        if (ctx->ms.steam_flag == STEAM_UNREADY) {
            ESP_LOGI(TAG,
                     "STEAM reject start tick=%lu steam_flag=%s steam_level=%u steam_temp=%.1f steam_target=%.1f milk_temp=%.1f milk_target=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     (unsigned)ctx->ms.steam_level,
                     ctx->ms.steam_current_temp,
                     ctx->ms.steam_target_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.milk_target_temp,
                     ctx->ms.pressure);
            voice_manager_play(VOICE_STEAMNOTREADY);
            ctx->core.state = ST_READY;
            return drink_leave_state(ctx, ST_STEAM, started);
        }

        drink_begin_direct(ctx, CTRL_ACT_STEAM_START);
        device_statistics_notify_local_state_start(ST_STEAM);
        ctx->state_runtime.drink.last_steam_flag = (uint8_t)ctx->ms.steam_flag;
        ESP_LOGI(TAG,
                 "STEAM start tick=%lu steam_flag=%s steam_level=%u setting_level=%u steam_temp=%.1f steam_target=%.1f milk_temp=%.1f milk_target=%.1f pressure=%.2f",
                 (unsigned long)ctx->timer.tick,
                 steam_flag_name((uint8_t)ctx->ms.steam_flag),
                 (unsigned)ctx->ms.steam_level,
                 (unsigned)ctx->drink.steam_level,
                 ctx->ms.steam_current_temp,
                 ctx->ms.steam_target_temp,
                 ctx->ms.milk_current_temp,
                 ctx->ms.milk_target_temp,
                 ctx->ms.pressure);
        *started = true;
    }

    elapsed_sec = (ctx->timer.tick - ctx->drink.start_tick) / STAY_TICKS_1S;

    switch (ctx->core.substate) {
    case BREW_SUB_PREPARE:
        if (ctx->state_runtime.drink.last_steam_flag != (uint8_t)ctx->ms.steam_flag) {
            ctx->state_runtime.drink.last_steam_flag = (uint8_t)ctx->ms.steam_flag;
            ESP_LOGI(TAG,
                     "STEAM prepare flag update tick=%lu steam_flag=%s",
                     (unsigned long)ctx->timer.tick,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag));
        }

        if (steam_stop_requested(ctx)) {
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            ctx->core.state = ST_READY;
            break;
        }

        if (ctx->ms.steam_flag == STEAM_RUNNING) {
            ctx->drink.start_tick = ctx->timer.tick;
            ctx->core.substate = BREW_SUB_RUNNING_1;
            ctx->state_runtime.drink.last_steam_log_sec = 0xFFFFFFFFU;
            ESP_LOGI(TAG, "STEAM prepare -> running tick=%lu", (unsigned long)ctx->timer.tick);
        }
        break;

    case BREW_SUB_RUNNING_1:
        if (ctx->state_runtime.drink.last_steam_flag != (uint8_t)ctx->ms.steam_flag) {
            ESP_LOGI(TAG,
                     "STEAM flag changed tick=%lu elapsed=%lus %s -> %s steam_temp=%.1f steam_target=%.1f milk_temp=%.1f milk_target=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name(ctx->state_runtime.drink.last_steam_flag),
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.steam_current_temp,
                     ctx->ms.steam_target_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.milk_target_temp,
                     ctx->ms.pressure);
            ctx->state_runtime.drink.last_steam_flag = (uint8_t)ctx->ms.steam_flag;
        }
        if (ctx->state_runtime.drink.last_steam_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_steam_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "STEAM running tick=%lu elapsed=%lus steam_flag=%s steam_temp=%.1f steam_target=%.1f milk_temp=%.1f milk_target=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.steam_current_temp,
                     ctx->ms.steam_target_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.milk_target_temp,
                     ctx->ms.pressure);
        }
        if (steam_stop_requested(ctx)) {
            ESP_LOGI(TAG,
                     "STEAM running -> tail-wait by user tick=%lu elapsed=%lus steam_flag=%s pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.pressure);
            steam_enter_tail_spray_wait(ctx);
            break;
        }

        if ((ctx->timer.tick - ctx->drink.start_tick) >= STEAM_MAX_RUN_TICKS) {
            ESP_LOGI(TAG,
                     "STEAM running -> tail-wait by safety-timeout tick=%lu elapsed=%lus steam_flag=%s pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.pressure);
            steam_enter_tail_spray_wait(ctx);
            break;
        }

        if (ctx->ms.steam_flag != STEAM_RUNNING &&
            ctx->timer.tick - ctx->drink.start_tick >= STAY_TICKS_3S) {
            ESP_LOGI(TAG,
                     "STEAM running -> tail-wait by steam-flag tick=%lu elapsed=%lus steam_flag=%s steam_temp=%.1f milk_temp=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.steam_current_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.pressure);
            steam_enter_tail_spray_wait(ctx);
        }
        break;

    case BREW_SUB_WAIT_TAIL_SPRAY:
        elapsed_sec = (ctx->timer.tick - ctx->drink.finish_tick) / STAY_TICKS_1S;
        if (ctx->state_runtime.drink.last_steam_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_steam_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "STEAM tail-wait tick=%lu wait_elapsed=%lus steam_flag=%s steam_temp=%.1f milk_temp=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.steam_current_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.pressure);
        }
        if (drink_has_fault_cancel_condition(ctx)) {
            ctx->state_runtime.drink.exit_reason = DRINK_EXIT_FAIL;
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            ESP_LOGI(TAG,
                     "STEAM tail-wait -> ready by fault tick=%lu error=0x%08lX",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)ctx->ms.error_code);
            ctx->core.state = ST_READY;
            break;
        }

        if (ctx->timer.tick - ctx->drink.target_time >= STEAM_TAIL_SPRAY_DELAY_TICKS) {
            steam_enter_tail_spray_countdown(ctx);
        }
        break;

    case BREW_SUB_TAIL_SPRAY_COUNTDOWN:
        elapsed_sec = (ctx->timer.tick - ctx->drink.target_time) / STAY_TICKS_1S;
        if (ctx->state_runtime.drink.last_steam_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_steam_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "STEAM tail-countdown tick=%lu elapsed=%lus steam_flag=%s steam_temp=%.1f milk_temp=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.steam_current_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.pressure);
        }
        if (drink_has_fault_cancel_condition(ctx)) {
            ctx->state_runtime.drink.exit_reason = DRINK_EXIT_FAIL;
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            ESP_LOGI(TAG,
                     "STEAM tail-countdown -> ready by fault tick=%lu error=0x%08lX",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)ctx->ms.error_code);
            ctx->core.state = ST_READY;
            break;
        }

        if (ctx->timer.tick - ctx->drink.target_time >= STAY_TICKS_3S &&
            !voice_play_is_busy()) {
            ctx->ctrl.src = CTRL_SRC_UI;
            ctr_cmd_action(CTRL_ACT_STEAM_START, NULL);
            ctx->drink.target_time = ctx->timer.tick;
            ctx->core.substate = BREW_SUB_TAIL_SPRAY_RUNNING;
            ctx->state_runtime.drink.last_steam_log_sec = 0xFFFFFFFFU;
            ESP_LOGI(TAG,
                     "STEAM tail-countdown -> tail-running tick=%lu steam_flag=%s pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.pressure);
        }
        break;

    case BREW_SUB_TAIL_SPRAY_RUNNING:
        elapsed_sec = (ctx->timer.tick - ctx->drink.target_time) / STAY_TICKS_1S;
        if (ctx->state_runtime.drink.last_steam_log_sec != elapsed_sec) {
            ctx->state_runtime.drink.last_steam_log_sec = elapsed_sec;
            ESP_LOGI(TAG,
                     "STEAM tail-running tick=%lu elapsed=%lus steam_flag=%s steam_temp=%.1f milk_temp=%.1f pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)elapsed_sec,
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.steam_current_temp,
                     ctx->ms.milk_current_temp,
                     ctx->ms.pressure);
        }
        if (drink_has_fault_cancel_condition(ctx) ||
            ctx->timer.tick - ctx->drink.target_time >= STEAM_TAIL_SPRAY_DURATION_TICKS) {
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            ESP_LOGI(TAG,
                     "STEAM tail-running -> ready tick=%lu reason=%s steam_flag=%s pressure=%.2f",
                     (unsigned long)ctx->timer.tick,
                     drink_has_fault_cancel_condition(ctx) ? "fault" : "duration",
                     steam_flag_name((uint8_t)ctx->ms.steam_flag),
                     ctx->ms.pressure);
            drink_return_ready(ctx);
        }
        break;

    case BREW_SUB_FINISH:
        drink_handle_finish_state(ctx);
        break;

    default:
        break;
    }

    if (ctx->core.substate == BREW_SUB_RUNNING_1) {
        handle_making_base(ctx);
    }

    return drink_leave_state(ctx, ST_STEAM, started);
}

app_state_t state_handle_grind(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.drink.grind_started;
    float remaining_g;

    if (!*started) {
        ctx->core.substate = BREW_SUB_PREPARE;
        ui_anim_stop_preheat(ctx);
        drink_reset_runtime(ctx);
        ESP_LOGI(TAG,
                 "GRIND enter prepare tick=%lu handle=%u hopper=%u grind_run=%u",
                 (unsigned long)ctx->timer.tick,
                 (unsigned)ctx->ms.grind_handle_postion_flag,
                 (unsigned)ctx->ms.beanbox_in_place,
                 (unsigned)ctx->ms.grind_run_flg);
        *started = true;
    }

    switch (ctx->core.substate) {
    case BREW_SUB_PREPARE:
        if (ctx->ms.beanbox_in_place != 1U) {
            ESP_LOGI(TAG,
                     "GRIND prepare blocked by hopper state tick=%lu hopper=%u -> READY",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.beanbox_in_place);
            ctx->core.state = ST_READY;
            break;
        }

        if (ctx->ms.grind_handle_postion_flag != 1U) {
            if (ctx->input.key_pressed & (1U << KEY_GRIND)) {
                ctx->input.key_pressed &= ~(1U << KEY_GRIND);
                ctx->state_runtime.drink.grind_allow_without_handle = true;
                ESP_LOGI(TAG,
                         "GRIND prepare override by grind key tick=%lu handle=%u hopper=%u",
                         (unsigned long)ctx->timer.tick,
                         (unsigned)ctx->ms.grind_handle_postion_flag,
                         (unsigned)ctx->ms.beanbox_in_place);
            }

            if (!ctx->state_runtime.drink.grind_prepare_wait_logged) {
                ESP_LOGI(TAG,
                         "GRIND prepare blocked tick=%lu handle=%u hopper=%u grind_run=%u",
                         (unsigned long)ctx->timer.tick,
                         (unsigned)ctx->ms.grind_handle_postion_flag,
                         (unsigned)ctx->ms.beanbox_in_place,
                         (unsigned)ctx->ms.grind_run_flg);
                ctx->state_runtime.drink.grind_prepare_wait_logged = true;
            }

            if (!ctx->state_runtime.drink.grind_allow_without_handle) {
                break;
            }
        }

        if (ctx->timer.tick - ctx->drink.start_tick >= STAY_TICKS_1S) {
            ESP_LOGI(TAG,
                     "GRIND start cmd tick=%lu elapsed=%lu handle=%u hopper=%u grind_run=%u direct=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned long)(ctx->timer.tick - ctx->drink.start_tick),
                     (unsigned)ctx->ms.grind_handle_postion_flag,
                     (unsigned)ctx->ms.beanbox_in_place,
                     (unsigned)ctx->ms.grind_run_flg,
                     (unsigned)ctx->state_runtime.drink.grind_allow_without_handle);
            ctx->ctrl.src = CTRL_SRC_UI;
            ctr_cmd_action(CTRL_ACT_GRIND_START, NULL);
            ctx->drink.start_tick = ctx->timer.tick;
            ctx->core.substate = BREW_SUB_RUNNING_1;
            ESP_LOGI(TAG, "GRIND substate -> RUNNING_1 tick=%lu", (unsigned long)ctx->timer.tick);
            device_statistics_notify_local_state_start(ST_GRIND);
        }
        break;

    case BREW_SUB_RUNNING_1:
        if (ctx->timer.tick == ctx->drink.start_tick) {
            ESP_LOGI(TAG,
                     "GRIND running tick=%lu grind_run=%u handle=%u hopper=%u direct=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.grind_run_flg,
                     (unsigned)ctx->ms.grind_handle_postion_flag,
                     (unsigned)ctx->ms.beanbox_in_place,
                     (unsigned)ctx->state_runtime.drink.grind_allow_without_handle);
        }

        if (ctx->state_runtime.drink.grind_resume_pending) {
            if ((ctx->timer.tick - ctx->state_runtime.drink.grind_resume_pause_tick) >=
                GRIND_RESUME_TIMEOUT_TICKS) {
                ESP_LOGI(TAG,
                         "GRIND resume timeout tick=%lu remain=%.1f -> READY",
                         (unsigned long)ctx->timer.tick,
                         (double)ctx->state_runtime.drink.grind_resume_remaining_g);
                drink_abort_active_process(ctx, DRINK_EXIT_CANCEL);
                ctx->core.state = ST_READY;
                return ST_READY;
            }

            if (ctx->input.key_long & (1U << KEY_GRIND)) {
                ctx->input.key_long &= ~(1U << KEY_GRIND);
                ESP_LOGI(TAG,
                         "GRIND resume cancelled by long grind key tick=%lu remain=%.1f",
                         (unsigned long)ctx->timer.tick,
                         (double)ctx->state_runtime.drink.grind_resume_remaining_g);
                drink_abort_active_process(ctx, DRINK_EXIT_CANCEL);
                ctx->core.state = ST_READY;
                return ST_READY;
            }

            if (ctx->input.key_pressed & (1U << KEY_GRIND)) {
                ctx->input.key_pressed &= ~(1U << KEY_GRIND);
                if (ctx->ms.bean_detect_flag == 0U) {
                    remaining_g = ctx->state_runtime.drink.grind_resume_remaining_g;
                    if (remaining_g >= GRIND_RESUME_MIN_REMAIN_G) {
                        ctx->ctrl.src = CTRL_SRC_UI;
                        ctr_cmd_action(CTRL_ACT_GRIND_START, &remaining_g);
                        ctx->drink.start_tick = ctx->timer.tick;
                        ctx->state_runtime.drink.grind_resume_pending = false;
                        ctx->state_runtime.drink.grind_notice_pause_sent = false;
                        ctx->state_runtime.drink.grind_resume_pause_tick = 0U;
                        ESP_LOGI(TAG,
                                 "GRIND resume by start cmd tick=%lu remain=%.1f",
                                 (unsigned long)ctx->timer.tick,
                                 (double)remaining_g);
                    }
                }
            }
            break;
        }

        if (ctx->ms.bean_detect_flag != 0U) {
            if (!ctx->state_runtime.drink.grind_notice_pause_sent) {
                remaining_g = drink_grind_remaining_g(ctx);
                if (remaining_g < GRIND_RESUME_MIN_REMAIN_G) {
                    ctx->ctrl.src = CTRL_SRC_UI;
                    ctr_cmd_action(CTRL_ACT_GRIND_STOP, NULL);
                    ctx->state_runtime.drink.grind_notice_pause_sent = true;
                    ctx->state_runtime.drink.grind_resume_pending = false;
                    ctx->state_runtime.drink.grind_resume_remaining_g = 0.0f;
                    ctx->state_runtime.drink.grind_resume_pause_tick = 0U;
                    ESP_LOGI(TAG,
                             "GRIND bean shortage ignored for resume tick=%lu powder=%.1f remain=%.1f",
                             (unsigned long)ctx->timer.tick,
                             (double)ctx->ms.powder_weight,
                             (double)remaining_g);
                } else {
                    ctx->ctrl.src = CTRL_SRC_UI;
                    ctr_cmd_action(CTRL_ACT_GRIND_STOP, NULL);
                    ctx->state_runtime.drink.grind_notice_pause_sent = true;
                    ctx->state_runtime.drink.grind_resume_pending = true;
                    ctx->state_runtime.drink.grind_resume_remaining_g = remaining_g;
                    ctx->state_runtime.drink.grind_resume_pause_tick = ctx->timer.tick;
                    ESP_LOGW(TAG,
                             "GRIND stopped by bean shortage tick=%lu grind_run=%u powder=%.1f remain=%.1f",
                             (unsigned long)ctx->timer.tick,
                             (unsigned)ctx->ms.grind_run_flg,
                             (double)ctx->ms.powder_weight,
                             (double)remaining_g);
                }
            }
            if (ctx->state_runtime.drink.grind_resume_pending) {
                break;
            }
        }

        if (ctx->ms.grind_run_flg != 1 &&
            ctx->timer.tick - ctx->drink.start_tick >= STAY_TICKS_3S) {
            ESP_LOGE(TAG,
                     "GRIND FINISH tick=%lu grind_run=%u handle=%u hopper=%u",
                     (unsigned long)ctx->timer.tick,
                     (unsigned)ctx->ms.grind_run_flg,
                     (unsigned)ctx->ms.grind_handle_postion_flag,
                     (unsigned)ctx->ms.beanbox_in_place);
            drink_enter_finish(ctx, CTRL_ACT_GRIND_STOP);
        }
        break;

    case BREW_SUB_FINISH:
        drink_handle_finish_state(ctx);
        break;

    default:
        break;
    }

    handle_making_base(ctx);
    return drink_leave_state(ctx, ST_GRIND, started);
}

void handle_making_base(app_ctx_t *ctx)
{
    uint16_t pressed = ctx->input.key_pressed;
    uint16_t long_pressed = ctx->input.key_long;
    uint8_t cancel_key;

    if (drink_has_fault_cancel_condition(ctx)) {
        ESP_LOGW(TAG,
                 "Making auto-cancelled by fault: state=%d error=0x%08lX",
                 ctx->core.state,
                 (unsigned long)ctx->ms.error_code);
        if (ctx->state_runtime.drink.parallel_steam_active) {
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            drink_clear_parallel_steam(ctx);
        }
        drink_abort_active_process(ctx, DRINK_EXIT_FAIL);
        ctx->core.state = ST_READY;
        return;
    }

    if (drink_controller_self_abort_confirmed(ctx, "DRINK")) {
        ESP_LOGW(TAG,
                 "Making aborted by controller self-stop: state=%d target=%d liquid=%.1f raw=%.1f flow_rate=%.2f",
                 ctx->core.state,
                 ctx->drink.target_drink,
                 ctx->ms.liquid_weight,
                 sp_pro_get_raw_liquid_progress_ml(ctx),
                 ctx->ms.flow_rate);
        if (ctx->state_runtime.drink.parallel_steam_active) {
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            drink_clear_parallel_steam(ctx);
        }
        drink_clear_finish_wait(ctx);
        ctx->state_runtime.drink.exit_reason = DRINK_EXIT_FAIL;
        ctx->core.state = ST_READY;
        return;
    }

    if (drink_scale_cup_removal_cancelled(ctx)) {
        ESP_LOGI(TAG, "Making cancelled by cup removal while scale is active");
        drink_abort_active_process(ctx, DRINK_EXIT_CANCEL);
        voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
        ctx->core.state = ST_READY;
        return;
    }

    if (drink_grind_sensor_cancelled(ctx)) {
        ESP_LOGI(TAG,
                 "Grind cancelled by sensor, handle=%u hopper=%u",
                 (unsigned)ctx->ms.grind_handle_postion_flag,
                 (unsigned)ctx->ms.beanbox_in_place);
        drink_abort_active_process(ctx, DRINK_EXIT_CANCEL);
        ctx->core.state = ST_READY;
        return;
    }

    if (drink_is_remote_flow(ctx)) {
        if (((pressed | long_pressed) & (1U << KEY_ESPRESSO)) != 0U) {
            ESP_LOGI(TAG, "Remote making cancelled by espresso key%s",
                     (long_pressed & (1U << KEY_ESPRESSO)) != 0U ? " long" : "");
            ctx->input.key_pressed &= ~(1U << KEY_ESPRESSO);
            ctx->input.key_long &= ~(1U << KEY_ESPRESSO);
            drink_abort_active_process(ctx, DRINK_EXIT_CANCEL);
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
            ctx->core.state = ST_READY;
            return;
        }

        if (pressed != 0U) {
            ctx->input.key_pressed = 0U;
        }
        if (long_pressed != 0U) {
            ctx->input.key_long = 0U;
        }

        ctx->drink.elapsed_tick = drink_elapsed_seconds(ctx);
        return;
    }

    if (ctx->input.key_pressed != 0 || ctx->input.key_long != 0) {
        ESP_LOGI(TAG,
                 "Making cancellation detected: key_pressed=0x%04X key_long=0x%04X, tick=%lu, state=%d target=%d sub=%d grind_run=%u handle=%u hopper=%u",
                 ctx->input.key_pressed,
                 ctx->input.key_long,
                 (unsigned long)ctx->timer.tick,
                 ctx->core.state,
                 ctx->drink.target_drink,
                 ctx->core.substate,
                 (unsigned)ctx->ms.grind_run_flg,
                 (unsigned)ctx->ms.grind_handle_postion_flag,
                 (unsigned)ctx->ms.beanbox_in_place);
    }

    if ((pressed & (1U << KEY_STEAM)) && drink_supports_parallel_steam(ctx)) {
        ctx->input.key_pressed &= ~(1U << KEY_STEAM);

        if (ctx->state_runtime.drink.parallel_steam_active) {
            ctr_cmd_action(CTRL_ACT_STEAM_STOP, NULL);
            drink_clear_parallel_steam(ctx);
            ESP_LOGI(TAG, "Parallel steam stopped by steam key");
        } else {
            (void)drink_start_parallel_steam(ctx);
        }
        return;
    }

    cancel_key = drink_cancel_key_for(ctx->drink.target_drink);
    if (ctx->drink.target_drink != DRINK_STEAM &&
        cancel_key < KEY_MAX &&
        (((pressed | long_pressed) & (1U << cancel_key)) != 0U)) {
        ESP_LOGI(TAG, "Making cancelled by key %u%s",
                 cancel_key,
                 (long_pressed & (1U << cancel_key)) != 0U ? " long" : "");
        ctx->input.key_pressed &= ~(1U << cancel_key);
        ctx->input.key_long &= ~(1U << cancel_key);
        drink_abort_active_process(ctx, DRINK_EXIT_CANCEL);
        if (ctx->drink.target_drink != DRINK_STEAM && ctx->drink.target_drink != DRINK_GRIND) {
            voice_manager_play_interrupt(VOICE_CANCELMAKECOFFEE);
        }
        if (ctx->state_runtime.drink.parallel_steam_active) {
            drink_promote_parallel_steam(ctx);
        } else {
            ctx->core.state = ST_READY;
        }
        return;
    }

    if (pressed != 0U) {
        ctx->input.key_pressed = 0;
    }
    if (long_pressed != 0U) {
        ctx->input.key_long = 0U;
    }

    ctx->drink.elapsed_tick = drink_elapsed_seconds(ctx);

}
