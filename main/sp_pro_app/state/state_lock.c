#include "sp_pro_app.h"
#include "sp_pro_app_types.h"
#include "sp_pro_app_state.h"
#include "esp_log.h"

static const char *TAG = "state_lock";

static bool child_lock_has_unrelated_key_event(const app_ctx_t *ctx)
{
    uint16_t key_mask;

    if (!ctx) {
        return false;
    }

    key_mask = (uint16_t)~(1U << KEY_CHILD);
    return ((ctx->input.key_pressed & key_mask) != 0U) ||
           ((ctx->input.key_long & key_mask) != 0U);
}

static void child_lock_show_locked_hint(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->child_lock.ui_hint = DISP_CHILD_LOCKED;
    voice_manager_play_interrupt(VOICE_PRSCHILDLOCK3S2UNLOCK);

    ctx->input.key_pressed = 0;
    ctx->input.key_long = 0;
}

static bool child_lock_key_is_down(const app_ctx_t *ctx)
{
    return ctx && ((ctx->input.key_down & (1U << KEY_CHILD)) != 0U);
}

static bool child_lock_hold_complete(app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    if (ctx->child_lock.press_tick == 0U) {
        ctx->child_lock.press_tick = ctx->timer.tick;
    }

    return (ctx->timer.tick - ctx->child_lock.press_tick) >= LOCK_HOLD_TICKS;
}

static app_state_t child_lock_release(app_ctx_t *ctx)
{
    ESP_LOGE(TAG, "Child lock released");

    ctx->child_lock.enabled = 0;
    ctx->child_lock.press_tick = 0;
    ctx->child_lock.ui_hint = DISP_CHILD_UNLOCK;
    return ST_READY;
}

app_state_t state_handle_child_lock(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    if (child_lock_has_unrelated_key_event(ctx)) {
        child_lock_show_locked_hint(ctx);
    }

    if (!child_lock_key_is_down(ctx)) {
        ctx->child_lock.press_tick = 0;
        return ST_LOCK;
    }

    if (child_lock_hold_complete(ctx)) {
        return child_lock_release(ctx);
    }

    return ST_LOCK;
}

