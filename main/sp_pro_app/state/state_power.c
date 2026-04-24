#include "sp_pro_app_state.h"
#include "sp_pro_app.h"
#include "led_service.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "state_power";
static const uint16_t POWER_ON_FLASH_ON_TICKS = STAY_TICKS(200);
static const uint16_t POWER_ON_FLASH_OFF_TICKS = STAY_TICKS(200);
static const uint8_t POWER_ON_FLASH_CYCLE_COUNT = 3U;
static const uint16_t POWER_ON_LOADING_INTERVAL_TICKS = STAY_TICKS(250);
static const uint16_t POWER_ON_REPLENISH_GUARD_TICKS = STAY_TICKS(1000);
static const uint16_t POWER_ON_LOADING_MIN_TICKS = STAY_TICKS(1000);
static const uint16_t POWER_ON_LOADING_DIAG_TICKS = STAY_TICKS(1000);
static const uint16_t POWER_ON_LOADING_TIMEOUT_TICKS = STAY_TICKS(5000);

static bool state_has_wake_event(const app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    return ctx->input.key_pressed != 0U ||
           ctx->input.key_long != 0U ||
           ctx->input.key_combo != 0U;
}

app_state_t state_handle_off(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_OFF;
    }

    ctx->anim.active = false;
    ctx->setting.active = false;
    ctx->input.key_pressed = 0;
    ctx->input.key_long = 0;
    ctx->input.key_combo = 0;
    memset(&ctx->state_runtime.power_on, 0, sizeof(ctx->state_runtime.power_on));
    return ST_OFF;
}

app_state_t state_handle_on(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    if (ctx->core.substate == 0U) {
        memset(&ctx->state_runtime.power_on, 0, sizeof(ctx->state_runtime.power_on));
        ctx->core.substate = (brew_substate_t)POWER_ON_SUB_FLASH;
        ctx->anim.active = false;
        ctx->anim.step = 0U;
        ctx->anim.tick = 0U;
        ctx->anim.interval = 0U;
        ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
        led_service_set_state(LED_EFFECT_WHITE_HIGHLIGHT);
        ESP_LOGI(TAG, "Enter ST_ON, flash screen");
        return ST_ON;
    }

    switch ((power_on_substate_t)ctx->core.substate) {
    case POWER_ON_SUB_FLASH:
        if ((ctx->anim.step & 0x01U) == 0U) {
            if ((ctx->timer.tick - ctx->state_runtime.power_on.phase_tick) >= POWER_ON_FLASH_ON_TICKS) {
                ctx->anim.step++;
                ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
                ESP_LOGI(TAG, "Power-on flash cycle %u bright done, display off",
                         (unsigned)((ctx->anim.step / 2U) + 1U));
            }
            return ST_ON;
        }

        if ((ctx->timer.tick - ctx->state_runtime.power_on.phase_tick) >= POWER_ON_FLASH_OFF_TICKS) {
            ctx->anim.step++;
            ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;

            if (ctx->anim.step < (POWER_ON_FLASH_CYCLE_COUNT * 2U)) {
                ESP_LOGI(TAG, "Power-on flash cycle %u off done, next flash",
                         (unsigned)(ctx->anim.step / 2U));
            } else {
                ctx->core.substate = (brew_substate_t)POWER_ON_SUB_LOADING;
                ctx->anim.active = true;
                ctx->anim.step = 0U;
                ctx->anim.tick = 0U;
                ctx->anim.interval = POWER_ON_LOADING_INTERVAL_TICKS;
                ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
                led_service_set_state(LED_EFFECT_WHITE_BREATH);
                ESP_LOGI(TAG, "Power-on flash complete, enter loading");
            }
        }
        return ST_ON;

    case POWER_ON_SUB_REPLENISH:
        ctx->core.substate = (brew_substate_t)POWER_ON_SUB_LOADING;
        ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
        ctx->anim.active = true;
        ctx->anim.step = 0U;
        ctx->anim.tick = 0U;
        ctx->anim.interval = POWER_ON_LOADING_INTERVAL_TICKS;
        ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
        led_service_set_state(LED_EFFECT_WHITE_BREATH);
        ESP_LOGI(TAG, "Power-on replenish stage collapsed into loading");
        return ST_ON;

    case POWER_ON_SUB_LOADING:
    {
        uint32_t loading_elapsed;
        bool replenish_done;

        if (!ctx->anim.active) {
            ctx->anim.active = true;
            ctx->anim.step = 0U;
            ctx->anim.tick = 0U;
            ctx->anim.interval = POWER_ON_LOADING_INTERVAL_TICKS;
        }

        if (!ctx->state_runtime.power_on.replenish_cmd_sent) {
            ctx->state_runtime.power_on.replenish_cmd_sent = true;
            ctx->state_runtime.power_on.replenish_tick = ctx->timer.tick;
            // ctr_cmd_action(CTRL_ACT_POWER, NULL);// TODO
            ESP_LOGI(TAG, "Power-on replenish command sent");
        }

        if (ctx->ms.drink_making_flg != DRINK_MAKER_NONE) {
            ctx->state_runtime.power_on.replenish_activity_seen = true;
        }

        replenish_done =
            (ctx->state_runtime.power_on.replenish_activity_seen &&
             ctx->ms.drink_making_flg == DRINK_MAKER_NONE) ||
            (!ctx->state_runtime.power_on.replenish_activity_seen &&
             (ctx->timer.tick - ctx->state_runtime.power_on.replenish_tick) >= POWER_ON_REPLENISH_GUARD_TICKS &&
             ctx->ms.drink_making_flg == DRINK_MAKER_NONE);

        loading_elapsed = ctx->timer.tick - ctx->state_runtime.power_on.phase_tick;

        if ((ctx->timer.tick - ctx->state_runtime.power_on.last_diag_tick) >= POWER_ON_LOADING_DIAG_TICKS) {
            ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
            ESP_LOGW(TAG,
                     "Power-on loading wait: elapsed=%lu ms ctr_status=%u drink=%u replenish_seen=%u replenish_done=%u cmd_sent=%u",
                     (unsigned long)(loading_elapsed * LOGIC_TASK_MS),
                     (unsigned)ctx->ms.ctr_status,
                     (unsigned)ctx->ms.drink_making_flg,
                     (unsigned)ctx->state_runtime.power_on.replenish_activity_seen,
                     (unsigned)replenish_done,
                     (unsigned)ctx->state_runtime.power_on.replenish_cmd_sent);
        }

        if (replenish_done &&
            loading_elapsed >= POWER_ON_LOADING_MIN_TICKS &&
            ctx->ms.ctr_status == 1U) {
            ctx->anim.active = false;
            voice_manager_play_touch_tone();
            ESP_LOGI(TAG, "CTR is ready, promote ST_ON to ST_READY");
            return ST_READY;
        }

        if (loading_elapsed >= POWER_ON_LOADING_TIMEOUT_TICKS) {
            ctx->anim.active = false;
            voice_manager_play_touch_tone();
            ESP_LOGW(TAG,
                     "Power-on loading timeout after %lu ms, fallback promote to ST_READY | ctr_status=%u drink=%u replenish_seen=%u replenish_done=%u",
                     (unsigned long)(loading_elapsed * LOGIC_TASK_MS),
                     (unsigned)ctx->ms.ctr_status,
                     (unsigned)ctx->ms.drink_making_flg,
                     (unsigned)ctx->state_runtime.power_on.replenish_activity_seen,
                     (unsigned)replenish_done);
            return ST_READY;
        }
        return ST_ON;
    }

    default:
        ctx->core.substate = 0U;
        return ST_ON;
    }
}

app_state_t state_handle_standby(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    ctx->anim.active = false;
    if (state_has_wake_event(ctx)) {
        ESP_LOGI(TAG, "Wake from standby");
        return ST_READY;
    }

    return ST_STANDBY;
}

