#include "sp_pro_app_types.h"
#include "sp_pro_app.h"
#include "sp_pro_app_state.h"
#include "esp_log.h"

static bool key_should_play_touch_tone(const app_ctx_t *ctx, int key_index)
{
    if (!ctx) {
        return false;
    }

    /* Avoid extra audio allocations while Wi-Fi pairing is being exited. */
    if (ctx->core.state == ST_WIFI && key_index == KEY_WIFI) {
        return false;
    }

    return true;
}

void key_scan_ticks(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (int i = 0; i < KEY_COUNT; i++) {
        if (ctx->input.key_down & (1 << i)) {
            if (ctx->input.key_time[i] < 0xFFFF) {
                ctx->input.key_time[i]++;
            }

            if (ctx->input.key_time[i] >= KEY_LONG_TICKS &&
                (ctx->input.long_lock_mask & (1U << i)) == 0U) {
                if (key_should_play_touch_tone(ctx, i)) {
                    voice_manager_play_touch_tone();
                }
                ctx->input.key_long |= (1U << i);
                ctx->input.long_lock_mask |= (1U << i);
            }
        }
    }
}

void key_combo_tick(app_ctx_t *ctx)
{
    uint16_t down;

    if (!ctx) {
        return;
    }

    down = ctx->input.key_down;

    /* K6 + K7 enters water intake mode selection. */
    if ((down & ((1 << 5) | (1 << 6))) == ((1 << 5) | (1 << 6))) {
        if (ctx->input.key_time[5] >= KEY_LONG_TICKS &&
            ctx->input.key_time[6] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_WATER_IN_MODE;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K9 + K10 enters clear-pipe flow. */
    if ((down & ((1 << 8) | (1 << 9))) == ((1 << 8) | (1 << 9))) {
        if (ctx->input.key_time[8] >= KEY_LONG_TICKS &&
            ctx->input.key_time[9] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_CLEAR_PIPE;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K8 + K9 enters factory-reset setting. */
    if ((down & ((1 << 7) | (1 << 8))) == ((1 << 7) | (1 << 8))) {
        if (ctx->input.key_time[7] >= KEY_LONG_TICKS &&
            ctx->input.key_time[8] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_FACTORY_RESET;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K7 + K8 enters water-hardness setting. */
    if ((down & ((1 << 6) | (1 << 7))) == ((1 << 6) | (1 << 7))) {
        if (ctx->input.key_time[6] >= KEY_LONG_TICKS &&
            ctx->input.key_time[7] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_WATER_HARDNESS;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K1 + K5 enters powder calibration. */
    if ((down & ((1 << 0) | (1 << 4))) == ((1 << 0) | (1 << 4))) {
        if (ctx->input.key_time[0] >= KEY_LONG_TICKS &&
            ctx->input.key_time[4] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_CAL_POWDER;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K2 + K5 enters flow calibration. */
    if ((down & ((1 << 1) | (1 << 4))) == ((1 << 1) | (1 << 4))) {
        if (ctx->input.key_time[1] >= KEY_LONG_TICKS &&
            ctx->input.key_time[4] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_CAL_FLOW;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K3 + K5 enters detection. */
    if ((down & ((1 << 2) | (1 << 4))) == ((1 << 2) | (1 << 4))) {
        if (ctx->input.key_time[2] >= KEY_LONG_TICKS &&
            ctx->input.key_time[4] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ctx->input.key_combo = KEY_COMBO_DETECTION;
            ctx->input.combo_lock_mask = 1U;
        }
    }

#ifdef MAINT_TEST
    /* K9 + K1 enters brew maintenance test. */
    if ((down & ((1 << 0) | (1 << 8))) == ((1 << 0) | (1 << 8))) {
        if (ctx->input.key_time[0] >= KEY_LONG_TICKS &&
            ctx->input.key_time[8] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ESP_LOGI("key_scan", "Enter KEY_COMBO_MAINT_BREW");
            ctx->input.key_combo = KEY_COMBO_MAINT_BREW;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K9 + K2 enters descaling maintenance test. */
    if ((down & ((1 << 1) | (1 << 8))) == ((1 << 1) | (1 << 8))) {
        if (ctx->input.key_time[1] >= KEY_LONG_TICKS &&
            ctx->input.key_time[8] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ESP_LOGI("key_scan", "Enter KEY_COMBO_MAINT_DES");
            ctx->input.key_combo = KEY_COMBO_MAINT_DES;
            ctx->input.combo_lock_mask = 1U;
        }
    }

    /* K9 + K3 enters steam maintenance test. */
    if ((down & ((1 << 2) | (1 << 8))) == ((1 << 2) | (1 << 8))) {
        if (ctx->input.key_time[2] >= KEY_LONG_TICKS &&
            ctx->input.key_time[8] >= KEY_LONG_TICKS &&
            ctx->input.combo_lock_mask == 0U) {
            ESP_LOGI("key_scan", "Enter KEY_COMBO_MAINT_STEAM");
            ctx->input.key_combo = KEY_COMBO_MAINT_STEAM;
            ctx->input.combo_lock_mask = 1U;
        }
    }
#endif

    /* Allow a new combo after all keys are released. */
    if (down == 0U) {
        ctx->input.combo_lock_mask = 0U;
    }
}

void key_event_handle(app_ctx_t *ctx, const bf7613_key_event_t *event)
{
    uint16_t new_mask;
    uint16_t old_mask;

    if (!ctx || !event) {
        return;
    }

    new_mask = event->key_mask;
    old_mask = ctx->input.key_down;

    for (int i = 0; i < KEY_COUNT; i++) {
        bool now = (new_mask & (1 << i)) != 0;
        bool last = (old_mask & (1 << i)) != 0;

        /* Reset hold time on key press edge. */
        if (now && !last) {
            if (key_should_play_touch_tone(ctx, i)) {
                voice_manager_play_touch_tone();
            }
            ctx->input.key_time[i] = 0;
            ctx->input.long_lock_mask &= ~(1U << i);
        }

        /* Convert release edge into a short press only when long press has not fired. */
        if (!now && last) {
            uint16_t held_ticks = ctx->input.key_time[i];

            if (held_ticks < KEY_LONG_TICKS) {
                ctx->input.key_pressed |= (1 << i);
            }

            ctx->input.key_time[i] = 0;
            ctx->input.long_lock_mask &= ~(1U << i);
        }
    }

    ctx->input.key_down = new_mask;
}

