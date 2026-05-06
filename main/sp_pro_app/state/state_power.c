#include "sp_pro_app_state.h"
#include "sp_pro_app.h"
#include "sp_pro_build_flags.h"
#include "led_service.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "state_power";
static const uint16_t POWER_ON_FLASH_ON_TICKS = STAY_TICKS(200);
static const uint16_t POWER_ON_FLASH_OFF_TICKS = STAY_TICKS(200);
static const uint8_t POWER_ON_FLASH_CYCLE_COUNT = 3U;
static const uint16_t POWER_ON_LOADING_INTERVAL_TICKS = STAY_TICKS(250);
#if !APP_TEST_FORCE_POWER_ON_REPLENISH
static const uint16_t POWER_ON_FACTORY_WRITE_GUARD_TICKS = STAY_TICKS(500);
static const uint16_t POWER_ON_FACTORY_READ_GUARD_TICKS = STAY_TICKS(500);
static const uint16_t POWER_ON_FACTORY_POLL_INTERVAL_TICKS = STAY_TICKS(1000);
#endif
static const uint16_t POWER_ON_LOADING_MIN_TICKS = STAY_TICKS(1000);
static const uint16_t POWER_ON_LOADING_DIAG_TICKS = STAY_TICKS(1000);
#if APP_TEST_FORCE_POWER_ON_REPLENISH
static const uint16_t POWER_ON_FORCE_REPLENISH_SETTLE_TICKS = STAY_TICKS(1000);
#endif

extern FLASH_FACTORY_DATA factory_data;

#if !APP_TEST_FORCE_POWER_ON_REPLENISH
static bool power_on_factory_identity_missing(void)
{
    return factory_data.sn_num[0] == '\0' || factory_data.model_name[0] == '\0';
}

static void power_on_request_factory_write(app_ctx_t *ctx, const char *reason)
{
    if (!ctx) {
        return;
    }

    if (ctr_cmd_action(CTRL_ACT_FACTORY_WRITE, NULL)) {
        ctx->state_runtime.power_on.factory_write_sent = true;
        ctx->state_runtime.power_on.factory_write_tick = ctx->timer.tick;
        ESP_LOGI(TAG,
                 "Power-on factory write requested reason=%s sn_missing=%u model_missing=%u",
                 reason ? reason : "unknown",
                 (unsigned)(factory_data.sn_num[0] == '\0'),
                 (unsigned)(factory_data.model_name[0] == '\0'));
    } else {
        ESP_LOGW(TAG, "Power-on factory write rejected reason=%s", reason ? reason : "unknown");
    }
}

static void power_on_request_factory_read(app_ctx_t *ctx, const char *reason)
{
    if (!ctx) {
        return;
    }

    if (ctr_cmd_action(CTRL_ACT_FACTORY_READ, NULL)) {
        ctx->state_runtime.power_on.factory_read_sent = true;
        ctx->state_runtime.power_on.factory_read_tick = ctx->timer.tick;
        ctx->state_runtime.power_on.last_factory_poll_tick = ctx->timer.tick;
        ESP_LOGI(TAG,
                 "Power-on factory read requested reason=%s cached_first_powered_on=%d",
                 reason ? reason : "unknown",
                 factory_data.first_powered_on);
    } else {
        ESP_LOGW(TAG, "Power-on factory read rejected reason=%s", reason ? reason : "unknown");
    }
}

static void power_on_poll_factory_read_if_due(app_ctx_t *ctx, const char *reason)
{
    if (!ctx) {
        return;
    }

    if (!ctx->state_runtime.power_on.factory_read_sent) {
        power_on_request_factory_read(ctx, reason);
        return;
    }

    if ((ctx->timer.tick - ctx->state_runtime.power_on.last_factory_poll_tick) >=
        POWER_ON_FACTORY_POLL_INTERVAL_TICKS) {
        power_on_request_factory_read(ctx, reason);
    }
}
#endif

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
    {
        bool replenish_done;

        if (!ctx->anim.active) {
            ctx->anim.active = true;
            ctx->anim.step = 0U;
            ctx->anim.tick = 0U;
            ctx->anim.interval = POWER_ON_LOADING_INTERVAL_TICKS;
        }

        if (!ctx->state_runtime.power_on.replenish_cmd_sent) {
            if (ctr_cmd_action(CTRL_ACT_POWER, NULL)) {
                ctx->state_runtime.power_on.replenish_cmd_sent = true;
                ctx->state_runtime.power_on.replenish_tick = ctx->timer.tick;
                ctx->state_runtime.power_on.replenish_activity_seen = false;
                ESP_LOGI(TAG, "Power-on replenish POWER_ON sent");
            } else {
                ESP_LOGW(TAG, "Power-on replenish POWER_ON rejected");
            }
        }

        if (ctx->ms.drink_making_flg != DRINK_MAKER_NONE) {
            ctx->state_runtime.power_on.replenish_activity_seen = true;
        }

#if !APP_TEST_FORCE_POWER_ON_REPLENISH
        power_on_poll_factory_read_if_due(ctx, "replenish_wait");
#endif

        replenish_done = ctx->state_runtime.power_on.replenish_cmd_sent &&
                         ctx->ms.drink_making_flg == DRINK_MAKER_NONE &&
                         factory_data.first_powered_on == 1;
#if APP_TEST_FORCE_POWER_ON_REPLENISH
        if (ctx->state_runtime.power_on.replenish_cmd_sent &&
            (ctx->timer.tick - ctx->state_runtime.power_on.replenish_tick) >=
                POWER_ON_FORCE_REPLENISH_SETTLE_TICKS) {
            ctx->anim.active = false;
            voice_manager_play_touch_tone();
            ESP_LOGI(TAG,
                     "Power-on replenish forced once for cert build, skip CTR completion wait");
            return ST_READY;
        }
#endif

        if ((ctx->timer.tick - ctx->state_runtime.power_on.last_diag_tick) >= POWER_ON_LOADING_DIAG_TICKS) {
            ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
            ESP_LOGI(TAG,
                     "Power-on replenish wait: ctr_status=%u drink=%u first_powered_on=%d activity_seen=%u power_sent=%u",
                     (unsigned)ctx->ms.ctr_status,
                     (unsigned)ctx->ms.drink_making_flg,
                     factory_data.first_powered_on,
                     (unsigned)ctx->state_runtime.power_on.replenish_activity_seen,
                     (unsigned)ctx->state_runtime.power_on.replenish_cmd_sent);
        }

        if (replenish_done) {
            ctx->state_runtime.power_on.first_powered_on_checked = true;
            ctx->state_runtime.power_on.replenish_required = false;
            ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
            ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
            ctx->core.substate = (brew_substate_t)POWER_ON_SUB_LOADING;
            ESP_LOGI(TAG,
                     "Power-on replenish complete: first_powered_on=%d drink=%u, back to loading",
                     factory_data.first_powered_on,
                     (unsigned)ctx->ms.drink_making_flg);
        }
        return ST_ON;
    }

    case POWER_ON_SUB_LOADING:
    {
        uint32_t loading_elapsed;

        if (!ctx->anim.active) {
            ctx->anim.active = true;
            ctx->anim.step = 0U;
            ctx->anim.tick = 0U;
            ctx->anim.interval = POWER_ON_LOADING_INTERVAL_TICKS;
        }

        if (!ctx->state_runtime.power_on.first_powered_on_checked) {
#if APP_TEST_FORCE_POWER_ON_REPLENISH
            ctx->state_runtime.power_on.first_powered_on_checked = true;
            ctx->state_runtime.power_on.replenish_required = true;
            ESP_LOGI(TAG, "Power-on cert build forces one replenish without factory wait");
            ctx->core.substate = (brew_substate_t)POWER_ON_SUB_REPLENISH;
            ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
            ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
            led_service_set_state(LED_EFFECT_WHITE_BREATH);
            return ST_ON;
#else
            if (!ctx->state_runtime.power_on.factory_write_sent &&
                power_on_factory_identity_missing()) {
                power_on_request_factory_write(ctx, "loading_prepare_identity");
                return ST_ON;
            }

            if (ctx->state_runtime.power_on.factory_write_sent &&
                (ctx->timer.tick - ctx->state_runtime.power_on.factory_write_tick) <
                    POWER_ON_FACTORY_WRITE_GUARD_TICKS) {
                return ST_ON;
            }

            power_on_poll_factory_read_if_due(ctx, "loading_wait");
            if (!ctx->state_runtime.power_on.factory_read_sent ||
                (ctx->timer.tick - ctx->state_runtime.power_on.factory_read_tick) <
                    POWER_ON_FACTORY_READ_GUARD_TICKS) {
                return ST_ON;
            }

            ctx->state_runtime.power_on.first_powered_on_checked = true;
#if APP_TEST_FORCE_POWER_ON_REPLENISH
            ctx->state_runtime.power_on.replenish_required = true;
#else
            ctx->state_runtime.power_on.replenish_required = (factory_data.first_powered_on == 0);
#endif
            ESP_LOGI(TAG,
                     "Power-on initial factory state first_powered_on=%d replenish_required=%u force=%u",
                     factory_data.first_powered_on,
                     (unsigned)ctx->state_runtime.power_on.replenish_required,
                     (unsigned)APP_TEST_FORCE_POWER_ON_REPLENISH);

            if (ctx->state_runtime.power_on.replenish_required) {
                ctx->core.substate = (brew_substate_t)POWER_ON_SUB_REPLENISH;
                ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
                ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
                led_service_set_state(LED_EFFECT_WHITE_BREATH);
                ESP_LOGI(TAG, "Power-on enter replenish by policy");
                return ST_ON;
            }
#endif
        }

#if !APP_TEST_FORCE_POWER_ON_REPLENISH
        if (factory_data.first_powered_on == 0) {
            ctx->state_runtime.power_on.replenish_required = true;
            ctx->core.substate = (brew_substate_t)POWER_ON_SUB_REPLENISH;
            ctx->state_runtime.power_on.phase_tick = ctx->timer.tick;
            ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
            ESP_LOGI(TAG, "Power-on late factory update requires replenish, switch to replenish");
            return ST_ON;
        }
#endif

        loading_elapsed = ctx->timer.tick - ctx->state_runtime.power_on.phase_tick;

        if ((ctx->timer.tick - ctx->state_runtime.power_on.last_diag_tick) >= POWER_ON_LOADING_DIAG_TICKS) {
            ctx->state_runtime.power_on.last_diag_tick = ctx->timer.tick;
            ESP_LOGW(TAG,
                     "Power-on loading wait: elapsed=%lu ms ctr_status=%u drink=%u first_powered_on=%d checked=%u",
                     (unsigned long)(loading_elapsed * LOGIC_TASK_MS),
                     (unsigned)ctx->ms.ctr_status,
                     (unsigned)ctx->ms.drink_making_flg,
                     factory_data.first_powered_on,
                     (unsigned)ctx->state_runtime.power_on.first_powered_on_checked);
        }

        if (loading_elapsed >= POWER_ON_LOADING_MIN_TICKS &&
            factory_data.first_powered_on == 1 &&
            ctx->ms.ctr_status == 1U) {
            ctx->anim.active = false;
            voice_manager_play_touch_tone();
            ESP_LOGI(TAG, "CTR is ready, promote ST_ON to ST_READY");
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
    /* Ignore HMI touch keys while the screen is off.
     * Standby wake is handled only by the dedicated power-key path
     * in sp_pro_apply_power_key(). */
    ctx->input.key_pressed = 0U;
    ctx->input.key_long = 0U;
    ctx->input.key_combo = 0U;

    return ST_STANDBY;
}
