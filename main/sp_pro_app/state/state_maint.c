#include "sp_pro_app.h"
#include "sp_pro_app_types.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_state.h"
#include <stddef.h>
#include "esp_log.h"
#include "device_statistics_store.h"
#include "event_record_service.h"
#include "formula_store.h"
#include "mqtt_protocol.h"

static const char *TAG = "state_maint";

#define MAINT_DRAIN_SHUTDOWN_TICKS STAY_TICKS(600000)

#define VOICE_PLAY_ONCE(id) \
    do { \
        if (!voice_play_is_busy()) { \
            voice_manager_play(id); \
        } \
    } while (0)

static bool maint_voice_repeat(voice_id_t id,
                               uint32_t interval,
                               uint8_t *count,
                               uint8_t max)
{
    if (*count < max && !voice_play_is_busy()) {
        if (voice_manager_interval(id, interval)) {
            (*count)++;
        }
    }

    return (*count >= max && !voice_play_is_busy());
}

static bool maint_wait_delay(app_ctx_t *ctx,
                             uint32_t *tick,
                             uint32_t delay)
{
    return (ctx->timer.tick - *tick >= delay);
}

static bool maint_wait_clean_key(app_ctx_t *ctx)
{
    if (ctx->input.key_pressed & (1 << KEY_CLEAN)) {
        ctx->input.key_pressed &= ~(1 << KEY_CLEAN);
        return true;
    }

    return false;
}

static bool maint_wait_clean_finish(app_ctx_t *ctx,
                                    uint32_t *tick)
{
    return (!ctx->ms.drink_making_flg &&
            ctx->timer.tick - *tick >= STAY_TICKS_3S);
}

static voice_id_t maint_water_add_voice(const app_ctx_t *ctx)
{
    if (ctx && ctx->setting.water_in_mode == WATER_IN_MODE_BUCKET) {
        return VOICE_BUCKETMODEWATERADDREMINDER;
    }

    return VOICE_TANKMODEWATERADDREMINDER;
}

static void maint_set_resume(app_ctx_t *ctx,
                             maint_type_t resume_type,
                             uint8_t resume_stage)
{
    bool changed;

    if (!ctx) {
        return;
    }

    changed = !ctx->maint.resume_flag ||
              ctx->maint.type != resume_type ||
              ctx->maint.resume_stage != resume_stage;
    ctx->maint.resume_flag = true;
    ctx->maint.type = resume_type;
    ctx->maint.resume_stage = resume_stage;
    ctx->state_runtime.maint.resume_prompt_played = false;
    if (changed) {
        sp_pro_app_save_settings();
    }
}

static void maint_clear_resume(app_ctx_t *ctx)
{
    bool changed;

    if (!ctx) {
        return;
    }

    changed = ctx->maint.resume_flag ||
              ctx->maint.resume_stage != MAINT_PROGRESS_NONE ||
              ctx->maint.type != MAINT_TYPE_NONE;
    ctx->maint.resume_flag = false;
    ctx->maint.resume_stage = MAINT_PROGRESS_NONE;
    ctx->maint.type = MAINT_TYPE_NONE;
    ctx->state_runtime.maint.resume_prompt_played = false;
    if (changed) {
        sp_pro_app_save_settings();
    }
}

static bool maint_prompt_and_start_clean(app_ctx_t *ctx,
                                         uint32_t *tick,
                                         uint32_t delay_ticks,
                                         control_action_t action,
                                         uint8_t resume_step)
{
    if (!ctx || !tick) {
        return false;
    }

    if (delay_ticks > 0 && !maint_wait_delay(ctx, tick, delay_ticks)) {
        return false;
    }

    voice_manager_interval(VOICE_CLICKCLEANBUTTON, 2000);

    if (!maint_wait_clean_key(ctx)) {
        return false;
    }

    ctr_cmd_action(action, NULL);
    maint_set_resume(ctx, ctx->maint.type, resume_step);
    *tick = ctx->timer.tick;
    return true;
}

static bool maint_wait_clear_pipe_exit_combo(app_ctx_t *ctx)
{
    if (!ctx || ctx->input.key_combo != KEY_COMBO_CLEAR_PIPE) {
        return false;
    }

    ctx->input.key_combo = 0;
    return true;
}

static int maint_state_to_cleaning_mode(app_state_t state)
{
    switch (state) {
    case ST_CLEAN_BREW:
    case ST_MAINT_BREW:
        return CLEANING_MODE_BREW;
    case ST_MAINT_STEAM:
        return CLEANING_MODE_STEAM;
    case ST_MAINT_DES:
        return CLEANING_MODE_DESCALING;
    default:
        return -1;
    }
}

static app_state_t maint_finish_flow(app_ctx_t *ctx,
                                     bool *started,
                                     voice_id_t finish_voice,
                                     app_state_t maintain_state)
{
    int cleaning_mode = maint_state_to_cleaning_mode(maintain_state);

    VOICE_PLAY_ONCE(finish_voice);
    device_statistics_notify_maintain_success(maintain_state);
    if (cleaning_mode >= 0) {
        mqtt_notify_clean_result_complete(cleaning_mode);
    }

    if (ctx) {
        switch (maintain_state) {
        case ST_MAINT_BREW:
            event_record_publish_brew_cleaning((int)(ctx->setting.clean_v + 0.5f));
            break;

        case ST_MAINT_DES:
            event_record_publish_descaling_ticks(ctx->timer.tick - ctx->maint.start_tick);
            break;

        case ST_MAINT_STEAM:
            event_record_publish_steam_cleaning((int)(ctx->setting.clean_v + 0.5f));
            break;

        default:
            break;
        }
    }

    if (started) {
        *started = false;
    }

    maint_clear_resume(ctx);
    return ST_READY;
}

static app_state_t maint_shutdown_and_power_off(app_ctx_t *ctx)
{
    if (ctx) {
        ctx->setting.clear_step = CLEAR_STEP_NOW_SHUTDOWN;
    }

    return ST_OFF;
}

app_state_t state_handle_clean_brew(app_ctx_t *ctx)
{
    if (!ctx->state_runtime.maint.clean_brew_started) {
        ESP_LOGI(TAG, "Enter CLEAN_BREW");

        ctx->maint.start_tick = ctx->timer.tick;
        ctx->maint.clean_state = MAINT_CLEAN_SUB_RUN;
        ctr_cmd_action(CTRL_ACT_CLEAN_BREW, NULL);

        ctx->state_runtime.maint.clean_brew_started = true;
    }

    switch (ctx->maint.clean_state) {
    case MAINT_CLEAN_SUB_RUN:
        if (ctx->input.key_pressed & (1U << KEY_CLEAN)) {
            ctx->input.key_pressed &= ~(1U << KEY_CLEAN);
            ESP_LOGI(TAG, "Cancel CLEAN_BREW by clean key");
            ctr_cmd_action(CTRL_ACT_CANCEL, NULL);
            voice_manager_play_touch_tone();
            ctx->state_runtime.maint.clean_brew_started = false;
            ctx->maint.clean_state = MAINT_CLEAN_SUB_IDLE;
            ctx->core.state = ST_READY;
            break;
        }

        if (maint_wait_clean_finish(ctx, &ctx->maint.start_tick)) {
            ESP_LOGI(TAG, "End CLEAN_BREW");
            ctx->maint.clean_state = MAINT_CLEAN_SUB_FINISH;
        }
        break;

    case MAINT_CLEAN_SUB_FINISH:
        device_statistics_notify_maintain_success(ST_MAINT_BREW);
        mqtt_notify_clean_result_complete(CLEANING_MODE_BREW);
        event_record_publish_brew_cleaning((int)(ctx->setting.clean_v + 0.5f));
        voice_manager_play_touch_tone();
        ctx->state_runtime.maint.clean_brew_started = false;
        ctx->maint.clean_state = MAINT_CLEAN_SUB_IDLE;
        ctx->core.state = ST_READY;
        break;

    default:
        break;
    }

    if (ctx->core.state != ST_CLEAN_BREW) {
        ctx->state_runtime.maint.clean_brew_started = false;
        return ctx->core.state;
    }

    return ST_CLEAN_BREW;
}

app_state_t state_handle_maint_brew(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.maint.brew_started;
    maint_brew_sub_t *step = &ctx->state_runtime.maint.brew_step;
    uint32_t *step_tick = &ctx->state_runtime.maint.brew_step_tick;
    uint8_t *voice_count = &ctx->state_runtime.maint.brew_voice_count;
    uint8_t *tablet_voice_count = &ctx->state_runtime.maint.brew_tablet_voice_count;

    if (!*started) {
        *started = true;
        *tablet_voice_count = 0;
        *voice_count = 0;
        ctx->maint.start_tick = ctx->timer.tick;
        if (ctx->maint.resume_flag) {
            if (!ctx->state_runtime.maint.resume_prompt_played) {
                VOICE_PLAY_ONCE(VOICE_CONTINUECLEANAFTERPOWEROFF);
                ctx->state_runtime.maint.resume_prompt_played = true;
            }
            *step = (maint_brew_sub_t)ctx->maint.resume_stage;
        } else {
            maint_clear_resume(ctx);
            *step = BREW_STEP_ADD_WATER;
        }
        ctx->maint.type = MAINT_TYPE_BREW;

        VOICE_PLAY_ONCE(VOICE_CLEANFRONTBREWUNIT);
    }

    switch (*step) {
    case BREW_STEP_ADD_WATER:
        if (!ctx->ms.water_box_shortage_flag &&
            maint_voice_repeat(maint_water_add_voice(ctx),
                               2000,
                               tablet_voice_count,
                               2)) {
            *step = BREW_STEP_ADD_POWDER;
            *step_tick = ctx->timer.tick;
        }
        break;

    case BREW_STEP_ADD_POWDER:
        if (maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            if (maint_voice_repeat(VOICE_PUTCLEANINGPOWDER,
                                   2000,
                                   voice_count,
                                   2)) {
                *step = BREW_STEP_WAIT_HANDLE;
                *voice_count = 0;
                *step_tick = ctx->timer.tick;
            }
        }
        break;

    case BREW_STEP_WAIT_HANDLE:
        if (!ctx->ms.brew_handle_postion_flag) {
            maint_voice_repeat(VOICE_PROMPTHANDLEFITTING,
                               2000,
                               tablet_voice_count,
                               2);
        } else {
            *step = BREW_STEP_WAIT_CLICK1;
            *step_tick = ctx->timer.tick;
        }
        break;

    case BREW_STEP_WATER_LACK:
        maint_voice_repeat(VOICE_SYSTEMLACKSWATER,
                           2000,
                           tablet_voice_count,
                           2);

        if (!ctx->ms.water_box_shortage_flag) {
            *step = (maint_brew_sub_t)ctx->maint.resume_stage;
            *step_tick = ctx->timer.tick;
        }
        break;

    case BREW_STEP_WAIT_CLICK1:
        if (maint_prompt_and_start_clean(ctx,
                                         step_tick,
                                         STAY_TICKS_3S,
                                         CTRL_ACT_MAINT_BREW1,
                                         BREW_STEP_WAIT_CLEAN1)) {
            *step = BREW_STEP_WAIT_CLEAN1;
        }
        break;

    case BREW_STEP_WAIT_CLEAN1:
        if (ctx->ms.water_box_shortage_flag) {
            *step = BREW_STEP_WATER_LACK;
            *step_tick = ctx->timer.tick;
        } else if (maint_wait_clean_finish(ctx, step_tick)) {
            *step = BREW_STEP_REMOVE_HANDLE;
        }
        break;

    case BREW_STEP_REMOVE_HANDLE:
        VOICE_PLAY_ONCE(VOICE_TAKEOFFHANDLEANDWASH);
        *step = BREW_STEP_WAIT_CLICK2;
        *step_tick = ctx->timer.tick;
        break;

    case BREW_STEP_WAIT_CLICK2:
        if (maint_prompt_and_start_clean(ctx,
                                         step_tick,
                                         STAY_TICKS_3S,
                                         CTRL_ACT_MAINT_BREW2,
                                         BREW_STEP_WAIT_CLEAN2)) {
            *step = BREW_STEP_WAIT_CLEAN2;
        }
        break;

    case BREW_STEP_WAIT_CLEAN2:
        if (ctx->ms.water_box_shortage_flag) {
            *step = BREW_STEP_WATER_LACK;
            *step_tick = ctx->timer.tick;
        } else if (maint_wait_clean_finish(ctx, step_tick)) {
            *step = BREW_STEP_FINISH;
        }
        break;

    case BREW_STEP_FINISH:
        return maint_finish_flow(ctx, started, VOICE_FINISHANDREMINDTODUMP, ST_MAINT_BREW);
    }

    return ST_MAINT_BREW;
}

app_state_t state_handle_maint_steam(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.maint.steam_started;
    maint_steam_sub_t *step = &ctx->state_runtime.maint.steam_step;
    uint32_t *step_tick = &ctx->state_runtime.maint.steam_step_tick;
    uint8_t *voice_count = &ctx->state_runtime.maint.steam_voice_count;

    if (!*started) {
        *started = true;
        *voice_count = 0;
        ctx->maint.start_tick = ctx->timer.tick;
        if (ctx->maint.resume_flag) {
            if (!ctx->state_runtime.maint.resume_prompt_played) {
                VOICE_PLAY_ONCE(VOICE_CONTINUECLEANAFTERPOWEROFF);
                ctx->state_runtime.maint.resume_prompt_played = true;
            }
            *step = (maint_steam_sub_t)ctx->maint.resume_stage;
        } else {
            maint_clear_resume(ctx);
            *step = STEAM_STEP_ADD_WATER;
        }
        ctx->maint.type = MAINT_TYPE_STEAM;

        VOICE_PLAY_ONCE(VOICE_CLEANSTEAMWAND);
    }

    switch (*step) {
    case STEAM_STEP_ADD_WATER:
        if (!ctx->ms.water_box_shortage_flag &&
            maint_voice_repeat(VOICE_ADDWATERTOCLEANINGLINE,
                               2000,
                               voice_count,
                               2)) {
            *step = STEAM_STEP_ADD_POWDER;
            *step_tick = ctx->timer.tick;
            *voice_count = 0;
        }
        break;

    case STEAM_STEP_ADD_POWDER:
        if (maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            if (maint_voice_repeat(VOICE_ADDCLEANPOWDER,
                                   2000,
                                   voice_count,
                                   2)) {
                *step = STEAM_STEP_SOAK1;
                *voice_count = 0;
                *step_tick = ctx->timer.tick;
            }
        }
        break;

    case STEAM_STEP_SOAK1:
        if (maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            if (maint_voice_repeat(VOICE_SOAKSTEAMNOZZLE,
                                   2000,
                                   voice_count,
                                   2)) {
                *step = STEAM_STEP_WAIT_CLICK1;
                *voice_count = 0;
                *step_tick = ctx->timer.tick;
            }
        }
        break;

    case STEAM_STEP_WATER_LACK:
        maint_voice_repeat(VOICE_SYSTEMLACKSWATER,
                           2000,
                           voice_count,
                           2);

        if (!ctx->ms.water_box_shortage_flag &&
            *voice_count >= 2 &&
            !voice_play_is_busy()) {
            if (ctx->maint.resume_stage == STEAM_STEP_WAIT_CLEAN1) {
                *step = STEAM_STEP_WAIT_CLEAN1;
            } else {
                *step = STEAM_STEP_WAIT_CLEAN2;
            }

            *step_tick = ctx->timer.tick;
        }
        break;

    case STEAM_STEP_WAIT_CLICK1:
        if (maint_prompt_and_start_clean(ctx,
                                         step_tick,
                                         STEAM_SOAK1_TICKS,
                                         CTRL_ACT_MAINT_STEAM,
                                         STEAM_STEP_WAIT_CLEAN1)) {
            *step = STEAM_STEP_WAIT_CLEAN1;
        }
        break;

    case STEAM_STEP_WAIT_CLEAN1:
        if (ctx->ms.water_box_shortage_flag) {
            *step = STEAM_STEP_WATER_LACK;
            *step_tick = ctx->timer.tick;
        } else if (maint_wait_clean_finish(ctx, step_tick)) {
            *step = STEAM_STEP_WASH_PITCHER;
        }
        break;

    case STEAM_STEP_WASH_PITCHER:
        VOICE_PLAY_ONCE(VOICE_WASHMILKPITCHER);
        *step = STEAM_STEP_SOAK2;
        *step_tick = ctx->timer.tick;
        *voice_count = 0;
        break;

    case STEAM_STEP_SOAK2:
        if (maint_voice_repeat(VOICE_SECONDSOAKSTEAMNOZZLE,
                               2000,
                               voice_count,
                               2)) {
            *step = STEAM_STEP_WAIT_CLICK2;
            *voice_count = 0;
            *step_tick = ctx->timer.tick;
        }
        break;

    case STEAM_STEP_WAIT_CLICK2:
        if (maint_prompt_and_start_clean(ctx,
                                         step_tick,
                                         STEAM_SOAK2_TICKS,
                                         CTRL_ACT_MAINT_STEAM,
                                         STEAM_STEP_WAIT_CLEAN2)) {
            *step = STEAM_STEP_WAIT_CLEAN2;
        }
        break;

    case STEAM_STEP_WAIT_CLEAN2:
        if (ctx->ms.water_box_shortage_flag) {
            *step = STEAM_STEP_WATER_LACK;
            *step_tick = ctx->timer.tick;
        } else if (maint_wait_clean_finish(ctx, step_tick)) {
            *step = STEAM_STEP_FINISH;
        }
        break;

    case STEAM_STEP_FINISH:
        return maint_finish_flow(ctx, started, VOICE_CLEANFINISHED, ST_MAINT_STEAM);
    }

    return ST_MAINT_STEAM;
}

app_state_t state_handle_maint_des(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.maint.des_started;
    maint_des_sub_t *step = &ctx->state_runtime.maint.des_step;
    uint32_t *step_tick = &ctx->state_runtime.maint.des_step_tick;
    uint8_t *voice_count = &ctx->state_runtime.maint.des_voice_count;

    if (!*started) {
        *started = true;
        *voice_count = 0;
        ctx->maint.start_tick = ctx->timer.tick;
        if (ctx->maint.resume_flag) {
            if (!ctx->state_runtime.maint.resume_prompt_played) {
                VOICE_PLAY_ONCE(VOICE_CONTINUECLEANAFTERPOWEROFF);
                ctx->state_runtime.maint.resume_prompt_played = true;
            }
            *step = (maint_des_sub_t)ctx->maint.resume_stage;
        } else {
            maint_clear_resume(ctx);
            *step = DES_STEP_SWITCH_WATER_MODE;
        }
        ctx->maint.type = MAINT_TYPE_DES;

        VOICE_PLAY_ONCE(VOICE_DESCALING);
    }

    switch (*step) {
    case DES_STEP_SWITCH_WATER_MODE:
        if (maint_voice_repeat(VOICE_SWITCHWATERTANKMODEREMIND,
                               2000,
                               voice_count,
                               2)) {
            *step = DES_STEP_ADD_WATER;
            *voice_count = 0;
            *step_tick = ctx->timer.tick;
        }
        break;

    case DES_STEP_ADD_WATER:
        if (!ctx->ms.water_box_shortage_flag &&
            maint_voice_repeat(VOICE_ADDWATERTOCLEANINGLINE,
                               2000,
                               voice_count,
                               2)) {
            *step = DES_STEP_WAIT_TABLET;
            *step_tick = ctx->timer.tick;
            *voice_count = 0;
        }
        break;

    case DES_STEP_WAIT_TABLET:
        if (maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            if (maint_voice_repeat(VOICE_PROMPTPLACECONTAINER,
                                   2000,
                                   voice_count,
                                   2)) {
                *step = DES_STEP_ADD_POWDER;
                *voice_count = 0;
                *step_tick = ctx->timer.tick;
            }
        }
        break;

    case DES_STEP_ADD_POWDER:
        if (maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            if (maint_voice_repeat(VOICE_ADDWATERANDDESCALINGPOWDER,
                                   2000,
                                   voice_count,
                                   2)) {
                *step = DES_STEP_WAIT_CLICK1;
                *voice_count = 0;
                *step_tick = ctx->timer.tick;
            }
        }
        break;

    case DES_STEP_WAIT_CLICK1:
        if (maint_prompt_and_start_clean(ctx,
                                         step_tick,
                                         0,
                                         CTRL_ACT_MAINT_DES1,
                                         DES_STEP_WAIT_CLEAN1)) {
            *step = DES_STEP_WAIT_CLEAN1;
        }
        break;

    case DES_STEP_WAIT_CLEAN1:
        if (ctx->ms.water_box_shortage_flag) {
            *step = DES_STEP_WATER_LACK;
            *step_tick = ctx->timer.tick;
        } else if (maint_wait_clean_finish(ctx, step_tick)) {
            *step = DES_STEP_CHANGE_WATER;
        }
        break;

    case DES_STEP_CHANGE_WATER:
        VOICE_PLAY_ONCE(VOICE_CHANGEWATER);
        *step = DES_STEP_WAIT_CLICK2;
        *step_tick = ctx->timer.tick;
        break;

    case DES_STEP_WAIT_CLICK2:
        if (maint_prompt_and_start_clean(ctx,
                                         step_tick,
                                         0,
                                         CTRL_ACT_MAINT_DES2,
                                         DES_STEP_WAIT_CLEAN2)) {
            *step = DES_STEP_WAIT_CLEAN2;
        }
        break;

    case DES_STEP_WAIT_CLEAN2:
        if (ctx->ms.water_box_shortage_flag) {
            *step = DES_STEP_WATER_LACK;
            *step_tick = ctx->timer.tick;
        } else if (maint_wait_clean_finish(ctx, step_tick)) {
            *step = DES_STEP_FINISH;
        }
        break;

    case DES_STEP_WATER_LACK:
        maint_voice_repeat(VOICE_SYSTEMLACKSWATER,
                           2000,
                           voice_count,
                           2);

        if (!ctx->ms.water_box_shortage_flag) {
            *step = (maint_des_sub_t)ctx->maint.resume_stage;
            *step_tick = ctx->timer.tick;
        }
        break;

    case DES_STEP_FINISH:
        return maint_finish_flow(ctx, started, VOICE_FINISHANDREMINDDUMPING, ST_MAINT_DES);
    }

    return ST_MAINT_DES;
}

app_state_t state_handle_maint_drain(app_ctx_t *ctx)
{
    bool *started = &ctx->state_runtime.maint.drain_started;
    uint32_t *step_tick = &ctx->state_runtime.maint.drain_step_tick;
    uint8_t *voice_count = &ctx->state_runtime.maint.drain_voice_count;
    uint8_t *finish_voice_count = &ctx->state_runtime.maint.drain_finish_voice_count;
    uint8_t *prompt_voice_count = &ctx->state_runtime.maint.drain_prompt_voice_count;
    uint8_t *shutdown_voice_count = &ctx->state_runtime.maint.drain_shutdown_voice_count;

    if (!*started) {
        ESP_LOGI(TAG, "Enter MAINT_DRAIN");
        *started = true;
        *voice_count = 0;
        *finish_voice_count = 0;
        *prompt_voice_count = 0;
        *shutdown_voice_count = 0;
        ctx->setting.sub = SET_SUB_CLEAR_WATERWAY;
        ctx->setting.clear_step = CLEAR_STEP_CUTOFF_SUPPLY;
        ctx->setting.reset_step = RESET_STEP_NONE;
        *step_tick = ctx->timer.tick;
    }

    if (maint_wait_clear_pipe_exit_combo(ctx)) {
        ESP_LOGI(TAG, "Exit MAINT_DRAIN by clean+wifi combo");
        *started = false;
        ctx->setting.clear_step = CLEAR_STEP_NONE;
        ctx->setting.sub = SET_SUB_CLEAR_WATERWAY;
        return ST_READY;
    }

    switch (ctx->setting.clear_step) {
    case CLEAR_STEP_CUTOFF_SUPPLY:
        if (maint_voice_repeat(VOICE_TANKMODEALERT,
                               2000,
                               voice_count,
                               2)) {
            ctr_cmd_action(CTRL_ACT_DRAIN, NULL);
            ctx->setting.clear_step = CLEAR_STEP_CLEARING;
            *step_tick = ctx->timer.tick;
        }
        break;

    case CLEAR_STEP_CLEARING:
        if (maint_wait_clean_finish(ctx, step_tick)) {
            event_record_publish_empty_water((int)(ctx->setting.clean_v + 0.5f));
            ctx->setting.clear_step = CLEAR_STEP_CLEARING_DONE;
            *finish_voice_count = 0;
            *step_tick = ctx->timer.tick;
        }
        break;

    case CLEAR_STEP_CLEARING_DONE:
        if (maint_voice_repeat(VOICE_FINISHANDREMINDDUMPING,
                               2000,
                               finish_voice_count,
                               2)) {
            ctx->setting.clear_step = CLEAR_STEP_FACTORY_RESET;
            ctx->setting.reset_step = RESET_STEP_DOUBLE_CONFIRM;
            *prompt_voice_count = 0;
            *step_tick = ctx->timer.tick;
        }
        break;

    case CLEAR_STEP_FACTORY_RESET:
        if (!maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            break;
        }

        if (*prompt_voice_count == 0U && !voice_play_is_busy()) {
            voice_manager_play(VOICE_CONFIRMFACTORYRESET);
            *prompt_voice_count = 1U;
        }

        if (ctx->input.key_long & (1U << KEY_ESPRESSO)) {
            ctx->input.key_long &= ~(1U << KEY_ESPRESSO);
            mqtt_restore_local_defaults();
            mqtt_set_formula_force_update_pending(true);
            formula_store_set_force_update(true);
            ctx->setting.reset_step = RESET_STEP_DONE_AND_SHUTDOWN;
            ctx->setting.clear_step = CLEAR_STEP_WILL_SHUTDOWN;
            *shutdown_voice_count = 0;
            *step_tick = ctx->timer.tick;
            break;
        }

        if (maint_wait_delay(ctx, step_tick, STAY_TICKS_3S + MAINT_DRAIN_SHUTDOWN_TICKS)) {
            ctx->setting.clear_step = CLEAR_STEP_WILL_SHUTDOWN;
            *shutdown_voice_count = 0;
            *step_tick = ctx->timer.tick;
        }
        break;

    case CLEAR_STEP_WILL_SHUTDOWN:
        if (ctx->setting.reset_step == RESET_STEP_DONE_AND_SHUTDOWN) {
            if (*shutdown_voice_count == 0U && !voice_play_is_busy()) {
                voice_manager_play(VOICE_FACTORYRESETCOMPLETED);
                *shutdown_voice_count = 1U;
            }
        } else if (*shutdown_voice_count == 0U && !voice_play_is_busy()) {
            voice_manager_play(VOICE_AUTOPOWEROFF);
            *shutdown_voice_count = 1U;
        }

        if (*shutdown_voice_count > 0U &&
            !voice_play_is_busy() &&
            maint_wait_delay(ctx, step_tick, STAY_TICKS_3S)) {
            *started = false;
            ctx->setting.sub = SET_SUB_FACTORY_RESET;
            return maint_shutdown_and_power_off(ctx);
        }
        break;

    case CLEAR_STEP_NOW_SHUTDOWN:
        *started = false;
        return maint_shutdown_and_power_off(ctx);

    case CLEAR_STEP_NONE:
    default:
        break;
    }

    return ST_MAINT_DRAIN;
}


