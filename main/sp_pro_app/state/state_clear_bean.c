#include "sp_pro_app.h"
#include "sp_pro_app_types.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_state.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "state_clear_bean";

#define CLEAR_BEAN_LOADING_TICKS     STAY_TICKS(10000)
#define CLEAR_BEAN_RUN_TICKS         STAY_TICKS(5000)
#define CLEAR_BEAN_STATUS_ABLE_TO_CLEAR 2U

static bool clear_bean_is_able_to_clear(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.beanbox_in_place == CLEAR_BEAN_STATUS_ABLE_TO_CLEAR);
}

static bool clear_bean_handle_present(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.grind_handle_postion_flag == 1U);
}

bool clear_bean_should_enter(const app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    return clear_bean_is_able_to_clear(ctx);
}

bool clear_bean_should_suppress_hopper_notice(const app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    return clear_bean_should_enter(ctx) &&
           (ctx->core.state == ST_READY || ctx->core.state == ST_CLEAR_BEAN);
}

static void clear_bean_enter_step(app_ctx_t *ctx, clear_bean_step_t step)
{
    if (!ctx || ctx->clear_bean.step == step) {
        return;
    }

    ctx->clear_bean.step = step;
    ctx->clear_bean.step_tick = ctx->timer.tick;
    ESP_LOGI(TAG, "step -> %d", step);
}

static void clear_bean_stop_grinder(app_ctx_t *ctx)
{
    if (!ctx || !ctx->state_runtime.clear_bean.grinder_started) {
        return;
    }

    ctx->ctrl.src = CTRL_SRC_UI;
    ctr_cmd_action(CTRL_ACT_GRIND_STOP, NULL);
    ctx->state_runtime.clear_bean.grinder_started = false;
}

static void clear_bean_reset(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    clear_bean_stop_grinder(ctx);
    ctx->clear_bean.step = CLEAR_BEAN_STEP_NONE;
    ctx->clear_bean.step_tick = 0U;
    ctx->state_runtime.clear_bean.entered = false;
}

static void clear_bean_maybe_play_voice(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    switch (ctx->clear_bean.step) {
    case CLEAR_BEAN_STEP_DISPLACED:
        voice_manager_interval(VOICE_ALERTBEANBINDISPLACED, 5000U);
        break;

    case CLEAR_BEAN_STEP_WAIT_HANDLE:
        voice_manager_interval(VOICE_PORTAFILTERNOTPLACED, 3000U);
        break;

    default:
        break;
    }
}

static void clear_bean_start_grinder(app_ctx_t *ctx)
{
    if (!ctx || ctx->state_runtime.clear_bean.grinder_started) {
        return;
    }

    ctx->ctrl.src = CTRL_SRC_UI;
    ctr_cmd_action(CTRL_ACT_GRIND_START, NULL);
    ctx->state_runtime.clear_bean.grinder_started = true;
    clear_bean_enter_step(ctx, CLEAR_BEAN_STEP_RUNNING);
}

app_state_t state_handle_clear_bean(app_ctx_t *ctx)
{
    bool unlock_position;
    bool handle_present;

    if (!ctx) {
        return ST_READY;
    }

    if (!ctx->state_runtime.clear_bean.entered) {
        ctx->state_runtime.clear_bean.entered = true;
        ctx->clear_bean.step = CLEAR_BEAN_STEP_NONE;
        ctx->clear_bean.step_tick = ctx->timer.tick;
    }

    unlock_position = clear_bean_is_able_to_clear(ctx);
    handle_present = clear_bean_handle_present(ctx);

    if (ctx->ms.error_code != 0U || ctx->ms.water_box_shortage_flag != 0U) {
        clear_bean_reset(ctx);
        return ST_READY;
    }

    if (!unlock_position) {
        clear_bean_stop_grinder(ctx);
        clear_bean_reset(ctx);
        return ST_READY;
    }

    if (ctx->clear_bean.step == CLEAR_BEAN_STEP_NONE) {
        clear_bean_enter_step(ctx, CLEAR_BEAN_STEP_UNLOCK_HINT);
        voice_manager_play_interrupt(VOICE_KNOBATUNLOCKEDPOSITION);
        ctx->input.key_pressed = 0;
        ctx->input.key_long = 0;
        return ST_CLEAR_BEAN;
    }

    if (ctx->clear_bean.step == CLEAR_BEAN_STEP_UNLOCK_HINT) {
        clear_bean_enter_step(ctx, CLEAR_BEAN_STEP_WAIT_RUN_DELAY);
        ctx->input.key_pressed = 0;
        ctx->input.key_long = 0;
        return ST_CLEAR_BEAN;
    }

    if (ctx->clear_bean.step == CLEAR_BEAN_STEP_WAIT_RUN_DELAY) {
        if (!handle_present) {
            clear_bean_maybe_play_voice(ctx);
        }

        if (ctx->timer.tick - ctx->clear_bean.step_tick < CLEAR_BEAN_LOADING_TICKS) {
            ctx->input.key_pressed = 0;
            ctx->input.key_long = 0;
            return ST_CLEAR_BEAN;
        }

        if (!handle_present) {
            clear_bean_stop_grinder(ctx);
            clear_bean_enter_step(ctx, CLEAR_BEAN_STEP_WAIT_HANDLE);
            clear_bean_maybe_play_voice(ctx);
            ctx->input.key_pressed = 0;
            ctx->input.key_long = 0;
            return ST_CLEAR_BEAN;
        }
    }

    if (!handle_present) {
        clear_bean_stop_grinder(ctx);
        clear_bean_enter_step(ctx, CLEAR_BEAN_STEP_WAIT_HANDLE);
        clear_bean_maybe_play_voice(ctx);
        ctx->input.key_pressed = 0;
        ctx->input.key_long = 0;
        return ST_CLEAR_BEAN;
    }

    if (ctx->clear_bean.step != CLEAR_BEAN_STEP_RUNNING &&
        ctx->clear_bean.step != CLEAR_BEAN_STEP_DONE &&
        voice_play_is_busy()) {
        ctx->input.key_pressed = 0;
        ctx->input.key_long = 0;
        return ST_CLEAR_BEAN;
    }

    if (ctx->clear_bean.step != CLEAR_BEAN_STEP_RUNNING &&
        ctx->clear_bean.step != CLEAR_BEAN_STEP_DONE) {
        clear_bean_start_grinder(ctx);
        ctx->input.key_pressed = 0;
        ctx->input.key_long = 0;
        return ST_CLEAR_BEAN;
    }

    if (ctx->clear_bean.step == CLEAR_BEAN_STEP_RUNNING &&
        (ctx->timer.tick - ctx->clear_bean.step_tick >= CLEAR_BEAN_RUN_TICKS)) {
        clear_bean_stop_grinder(ctx);
        clear_bean_enter_step(ctx, CLEAR_BEAN_STEP_DONE);
        voice_manager_play(VOICE_KEY);
        ctx->input.key_pressed = 0;
        ctx->input.key_long = 0;
        return ST_CLEAR_BEAN;
    }

    if (ctx->clear_bean.step == CLEAR_BEAN_STEP_DONE) {
        if (voice_play_is_busy()) {
            ctx->input.key_pressed = 0;
            ctx->input.key_long = 0;
            return ST_CLEAR_BEAN;
        }

        clear_bean_reset(ctx);
        return ST_READY;
    }

    ctx->input.key_pressed = 0;
    ctx->input.key_long = 0;
    return ST_CLEAR_BEAN;
}
