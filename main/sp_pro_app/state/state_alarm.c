#include "sp_pro_app.h"
#include "sp_pro_build_flags.h"
#include <stddef.h>
#include "esp_log.h"
#include "device_statistics_store.h"

static const char *TAG = "state_alarm";
static const uint32_t WATER_PUMP_RETRY_TIMEOUT_TICKS = STAY_TICKS(5000);

typedef struct {
    uint32_t error_bit;   // Bitmask from MACHINE_STATUS.error_code.
    uint8_t major;        // Hxx major code.
    uint8_t sub;          // Sub-code, or 0 when not used.
} alarm_fault_map_t;

typedef struct {
    p_indicator_t indicator;
    warning_type_t warning;
    maint_type_t notice_type;
} alarm_notice_t;

static bool alarm_should_suppress_maint_notice(const app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    switch (ctx->core.state) {
    case ST_MAINT_BREW:
    case ST_MAINT_DES:
    case ST_MAINT_STEAM:
        return true;
    default:
        return false;
    }
}

static uint8_t alarm_maint_notice_mask(maint_type_t type)
{
    switch (type) {
    case MAINT_TYPE_STEAM:
        return (uint8_t)(1U << 0);
    case MAINT_TYPE_BREW:
        return (uint8_t)(1U << 1);
    case MAINT_TYPE_DES:
        return (uint8_t)(1U << 2);
    default:
        return 0U;
    }
}

static void alarm_refresh_dismissed_maint_notice(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if ((ctx->state_runtime.alarm.dismissed_maint_notice_mask & alarm_maint_notice_mask(MAINT_TYPE_STEAM)) != 0U &&
        !device_statistics_should_raise_notice(MAINT_TYPE_STEAM)) {
        ctx->state_runtime.alarm.dismissed_maint_notice_mask &= (uint8_t)~alarm_maint_notice_mask(MAINT_TYPE_STEAM);
    }

    if ((ctx->state_runtime.alarm.dismissed_maint_notice_mask & alarm_maint_notice_mask(MAINT_TYPE_BREW)) != 0U &&
        !device_statistics_should_raise_notice(MAINT_TYPE_BREW)) {
        ctx->state_runtime.alarm.dismissed_maint_notice_mask &= (uint8_t)~alarm_maint_notice_mask(MAINT_TYPE_BREW);
    }

    if ((ctx->state_runtime.alarm.dismissed_maint_notice_mask & alarm_maint_notice_mask(MAINT_TYPE_DES)) != 0U &&
        !device_statistics_should_raise_notice(MAINT_TYPE_DES)) {
        ctx->state_runtime.alarm.dismissed_maint_notice_mask &= (uint8_t)~alarm_maint_notice_mask(MAINT_TYPE_DES);
    }
}

static bool alarm_is_maint_notice_dismissed(const app_ctx_t *ctx, maint_type_t type)
{
    if (!ctx) {
        return false;
    }

    return (ctx->state_runtime.alarm.dismissed_maint_notice_mask & alarm_maint_notice_mask(type)) != 0U;
}

static const alarm_fault_map_t g_alarm_fault_map[] = {
    /* H05 water pump fault. */
    { WATER_PUMP_ERROR,                 5, 0 },

    /* H01 brew heater faults. */
    { BREW_HEAT_PLATE_ERROR,           1, 1 },
    { BREW_HEAT_PLATE_FAST_TEMP_ERROR, 1, 2 },
    { BREW_HEAT_PLATE_HIGH_TEMP_ERROR, 1, 3 },
    { E_FAST_ERROR,                    1, 7 },

    /* H01 steam heater faults. */
    { STEAM_HEAT_PLATE_ERROR,           1, 4 },
    { STEAM_HEAT_PLATE_FAST_TEMP_ERROR, 1, 5 },
    { STEAM_HEAT_PLATE_HIGH_TEMP_ERROR, 1, 6 },

    /* H02 low temperature. */
    { LOW_MACHINE_TEMP_ERROR,           2, 0 },

    /* H08 NTC faults. */
    { NTC_COFFEE_ERROR,                 8, 1 },
    { NTC_FOAM_ERROR,                   8, 2 },
    { NTC_BREW_ERROR,                   8, 3 },
    { NTC_STEAM_ERROR,                  8, 4 },
    { NTC_RELIEF_ERROR,                 8, 5 },

    /* H09 flowmeter fault. */
    { WATER_WAY_ERROR,                  9, 0 },

    /* H10 pressure faults. */
    { PRESSURE_SIGNAL_ERROR,           10, 1 },
    { PRESSURE_VALUE_ERROR,            10, 2 },

    /* H11 bean hopper fault. */
    { BEANBOX_ERROR,                   11, 0 },
};

static void alarm_reset_voice_state(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.alarm.active = false;
    ctx->state_runtime.alarm.is_notice = false;
    ctx->state_runtime.alarm.major = 0;
    ctx->state_runtime.alarm.sub = 0;
    ctx->state_runtime.alarm.notice_p = BF_STATUS_NO;
    ctx->state_runtime.alarm.warning = WARN_NONE;
    ctx->state_runtime.alarm.error_code = 0U;
    ctx->state_runtime.alarm.fault_enter_tick = 0U;
    ctx->state_runtime.alarm.second_fault_played = false;
    ctx->state_runtime.alarm.water_pump_retry_count = 0U;
    ctx->state_runtime.alarm.water_pump_replenishing = false;
    ctx->state_runtime.alarm.water_pump_retry_tick = 0U;
    ctx->state_runtime.alarm.fault_cancel_sent = false;
    ctx->state_runtime.alarm.shutdown_pending = false;
    ctx->state_runtime.alarm.shutdown_tick = 0U;
}

static uint32_t alarm_effective_error_code(const app_ctx_t *ctx)
{
    if (!ctx) {
        return 0U;
    }

    return ctx->ms.error_code & ~(ctx->state_runtime.alarm.suppressed_error_mask);
}

static bool alarm_is_water_pump_fault(const app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    return (alarm_effective_error_code(ctx) & WATER_PUMP_ERROR) != 0U;
}

static bool alarm_is_heater_fault(const app_ctx_t *ctx)
{
    uint32_t error_code;

    if (!ctx) {
        return false;
    }

    error_code = alarm_effective_error_code(ctx);
    return (error_code & (BREW_HEAT_PLATE_ERROR |
                          BREW_HEAT_PLATE_FAST_TEMP_ERROR |
                          BREW_HEAT_PLATE_HIGH_TEMP_ERROR |
                          STEAM_HEAT_PLATE_ERROR |
                          STEAM_HEAT_PLATE_FAST_TEMP_ERROR |
                          STEAM_HEAT_PLATE_HIGH_TEMP_ERROR |
                          E_FAST_ERROR)) != 0U;
}

static void alarm_clear(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->alarm.active = false;
    ctx->alarm.is_notice = false;
    ctx->alarm.major = 0;
    ctx->alarm.sub = 0;
    ctx->alarm.notice_p = BF_STATUS_NO;
    ctx->alarm.warning = WARN_NONE;
    ctx->alarm.notice_type = MAINT_TYPE_NONE;
    alarm_reset_voice_state(ctx);
}

static void alarm_set_notice(app_ctx_t *ctx,
                             p_indicator_t indicator,
                             warning_type_t warning,
                             maint_type_t notice_type)
{
    if (!ctx) {
        return;
    }

    ctx->alarm.active = true;
    ctx->alarm.is_notice = true;
    ctx->alarm.notice_p = indicator;
    ctx->alarm.warning = warning;
    ctx->alarm.notice_type = notice_type;
    ctx->alarm.major = 0;
    ctx->alarm.sub = 0;
}

static void alarm_set_fault(app_ctx_t *ctx,
                            const alarm_fault_map_t *fault)
{
    if (!ctx) {
        return;
    }

    ctx->alarm.active = true;
    ctx->alarm.is_notice = false;
    ctx->alarm.notice_p = BF_STATUS_NO;
    ctx->alarm.warning = WARN_NONE;
    ctx->alarm.notice_type = MAINT_TYPE_NONE;

    if (fault) {
        ctx->alarm.major = fault->major;
        ctx->alarm.sub = fault->sub;
    } else {
        ctx->alarm.major = 0;
        ctx->alarm.sub = 0;
    }
}

static bool alarm_find_notice(app_ctx_t *ctx, alarm_notice_t *notice)
{
    if (!ctx || !notice) {
        return false;
    }

    alarm_refresh_dismissed_maint_notice(ctx);

    if (ctx->core.state == ST_GRIND &&
        ctx->core.substate == BREW_SUB_PREPARE &&
        ctx->ms.grind_handle_postion_flag != 1U &&
        !ctx->state_runtime.drink.grind_allow_without_handle) {
        notice->indicator = BF_STATUS_NO;
        notice->warning = WARN_GRIND_HD_MISS;
        notice->notice_type = MAINT_TYPE_NONE;
        return true;
    }

    if (sp_pro_app_state_requires_brew_handle(ctx->core.state) &&
        ctx->core.substate == BREW_SUB_PREPARE &&
        !sp_pro_app_is_brew_handle_in_place()) {
        notice->indicator = BF_STATUS_P2;
        notice->warning = WARN_BREW_HD_MISS;
        notice->notice_type = MAINT_TYPE_NONE;
        return true;
    }

    if (ctx->core.state == ST_GRIND &&
        ctx->core.substate == BREW_SUB_RUNNING_1 &&
        ctx->ms.bean_detect_flag != 0U) {
        notice->indicator = BF_STATUS_P2;
        notice->warning = WARN_BEAN_EMPTY;
        notice->notice_type = MAINT_TYPE_NONE;
        return true;
    }

    if (ctx->core.state == ST_READY &&
        ctx->state_runtime.ready.steam_not_ready_notice_deadline != 0U &&
        (int32_t)(ctx->state_runtime.ready.steam_not_ready_notice_deadline - ctx->timer.tick) > 0) {
        notice->indicator = BF_STATUS_NO;
        notice->warning = WARN_STEAM_NOT_READY;
        notice->notice_type = MAINT_TYPE_NONE;
        return true;
    }

    if (ctx->core.state == ST_WATER &&
        ctx->ms.water_box_shortage_flag) {
        notice->indicator = BF_STATUS_P1;
        notice->warning = WARN_WATER_EMPTY;
        notice->notice_type = MAINT_TYPE_NONE;
        return true;
    }

    /* P1/P2: ready-like notices.
     * Also allow the final power-on loading phase so "no water tank / add water"
     * can be announced immediately after boot completes. */
    if (ctx->core.state == ST_READY ||
        (ctx->core.state == ST_ON &&
         ctx->core.substate == (brew_substate_t)POWER_ON_SUB_LOADING) ||
        ctx->core.state == ST_WIFI ||
        ctx->core.state == ST_STANDBY ||
        ctx->core.state == ST_LOCK ||
        ctx->core.state == ST_SETTING ||
        ctx->core.state == ST_DRINK_SET) {
        bool hopper_notice_active = (ctx->ms.beanbox_in_place == 0U) ||
                                    ctx->state_runtime.clear_bean.post_clean_notice_pending;

        if (ctx->ms.water_box_shortage_flag &&
            hopper_notice_active &&
            !clear_bean_should_suppress_hopper_notice(ctx)) {
            notice->indicator = BF_STATUS_P1;
            notice->warning = WARN_WATER_BEAN_MISS;
            notice->notice_type = MAINT_TYPE_NONE;
            return true;
        }

        if (ctx->ms.water_box_shortage_flag) {
            notice->indicator = BF_STATUS_P1;
            notice->warning = WARN_WATER_EMPTY;
            notice->notice_type = MAINT_TYPE_NONE;
            return true;
        }

        if (hopper_notice_active && !clear_bean_should_suppress_hopper_notice(ctx)) {
            notice->indicator = BF_STATUS_P2;
            notice->warning = WARN_BEAN_MISS;
            notice->notice_type = MAINT_TYPE_NONE;
            return true;
        }
    }

    /* Bean hopper missing in other scenes is a non-blocking popup. */
    if (((ctx->ms.beanbox_in_place == 0U) ||
         ctx->state_runtime.clear_bean.post_clean_notice_pending) &&
        !clear_bean_should_suppress_hopper_notice(ctx)) {
        notice->indicator = BF_STATUS_P2;
        notice->warning = WARN_BEAN_MISS;
        notice->notice_type = MAINT_TYPE_NONE;
        return true;
    }

    /* Maintenance reminders. */
    if (ctx->ms.water_box_shortage_flag) {
        /* Water shortage while active brew/steam/clean is deferred to the state machine or shown after exit. */
        return false;
    }

    if (!alarm_should_suppress_maint_notice(ctx) &&
        ctx->maint.resume_flag &&
        !alarm_is_maint_notice_dismissed(ctx, ctx->maint.type)) {
        switch (ctx->maint.type) {
        case MAINT_TYPE_STEAM:
            notice->indicator = BF_STATUS_P3;
            notice->warning = WARN_NONE;
            notice->notice_type = MAINT_TYPE_STEAM;
            return true;

        case MAINT_TYPE_BREW:
            notice->indicator = BF_STATUS_P4;
            notice->warning = WARN_NONE;
            notice->notice_type = MAINT_TYPE_BREW;
            return true;

        case MAINT_TYPE_DES:
            notice->indicator = BF_STATUS_P5;
            notice->warning = WARN_NONE;
            notice->notice_type = MAINT_TYPE_DES;
            return true;

        case MAINT_TYPE_NONE:
        default:
            break;
        }
    }

    /* Maintenance reminders. */
    if (!alarm_should_suppress_maint_notice(ctx) &&
        !alarm_is_maint_notice_dismissed(ctx, MAINT_TYPE_STEAM) &&
        device_statistics_should_raise_notice(MAINT_TYPE_STEAM)) {
        notice->indicator = BF_STATUS_P3;
        notice->warning = WARN_NONE;
        notice->notice_type = MAINT_TYPE_STEAM;
        return true;
    }

    if (!alarm_should_suppress_maint_notice(ctx) &&
        !alarm_is_maint_notice_dismissed(ctx, MAINT_TYPE_BREW) &&
        device_statistics_should_raise_notice(MAINT_TYPE_BREW)) {
        notice->indicator = BF_STATUS_P4;
        notice->warning = WARN_NONE;
        notice->notice_type = MAINT_TYPE_BREW;
        return true;
    }

    if (!alarm_should_suppress_maint_notice(ctx) &&
        !alarm_is_maint_notice_dismissed(ctx, MAINT_TYPE_DES) &&
        device_statistics_should_raise_notice(MAINT_TYPE_DES)) {
        notice->indicator = BF_STATUS_P5;
        notice->warning = WARN_NONE;
        notice->notice_type = MAINT_TYPE_DES;
        return true;
    }

    return false;
}

static const alarm_fault_map_t *alarm_parse_fault(uint32_t error_code)
{
    for (size_t i = 0; i < sizeof(g_alarm_fault_map) / sizeof(g_alarm_fault_map[0]); i++) {
        if (error_code & g_alarm_fault_map[i].error_bit) {
            return &g_alarm_fault_map[i];
        }
    }
    return NULL;
}

static voice_id_t alarm_notice_voice(const app_ctx_t *ctx)
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

    case WARN_BEAN_EMPTY:
        return VOICE_FILLCOFFEEBEANS;

    case WARN_BREW_HD_MISS:
        return VOICE_INSTALLHANDLE;

    case WARN_GRIND_HD_MISS:
        return VOICE_GRINDINGWITHOUTHANDLE;

    case WARN_STEAM_NOT_READY:
        return VOICE_STEAMNOTREADY;

    case WARN_NONE:
    case WARN_TRAY_MISS:
    case WARN_LIQUID_ABNM:
    default:
        break;
    }

    switch (ctx->alarm.notice_type) {
    case MAINT_TYPE_STEAM:
        if (ctx->maint.resume_flag &&
            ctx->maint.type == MAINT_TYPE_STEAM &&
            !ctx->state_runtime.maint.resume_prompt_played) {
            return VOICE_CONTINUECLEANAFTERPOWEROFF;
        }
        return VOICE_CLEANSTEAMWAND;

    case MAINT_TYPE_BREW:
        if (ctx->maint.resume_flag &&
            ctx->maint.type == MAINT_TYPE_BREW &&
            !ctx->state_runtime.maint.resume_prompt_played) {
            return VOICE_CONTINUECLEANAFTERPOWEROFF;
        }
        return VOICE_CLEANFRONTBREWUNIT;

    case MAINT_TYPE_DES:
        if (ctx->maint.resume_flag &&
            ctx->maint.type == MAINT_TYPE_DES &&
            !ctx->state_runtime.maint.resume_prompt_played) {
            return VOICE_CONTINUECLEANAFTERPOWEROFF;
        }
        return VOICE_DESCALING;

    case MAINT_TYPE_NONE:
    default:
        return VOICE_NONE;
    }
}

static bool alarm_is_generic_fault(uint32_t error_code)
{
    if (error_code == 0U) {
        return false;
    }

    if (error_code & WATER_PUMP_ERROR) {
        return false;
    }

    if (error_code & LOW_MACHINE_TEMP_ERROR) {
        return false;
    }

    if (error_code & (PRESSURE_SIGNAL_ERROR | PRESSURE_VALUE_ERROR)) {
        return false;
    }

    if (error_code & BEANBOX_ERROR) {
        return false;
    }

    return true;
}

static voice_id_t alarm_fault_voice(const app_ctx_t *ctx)
{
    uint32_t error_code;

    if (!ctx) {
        return VOICE_NONE;
    }

    error_code = alarm_effective_error_code(ctx);

    if (error_code & WATER_PUMP_ERROR) {
        if (ctx->state_runtime.alarm.water_pump_retry_count >= 3U) {
            return VOICE_WATERPUMPFAULT;
        }
        return VOICE_SYSTEMLACKSWATER;
    }

    if (error_code & LOW_MACHINE_TEMP_ERROR) {
        return VOICE_LOWTEMPERATUREERROR;
    }

    if (error_code & (PRESSURE_SIGNAL_ERROR | PRESSURE_VALUE_ERROR)) {
        return VOICE_PRESSURESENSORFAULTWARNING;
    }

    if (error_code & BEANBOX_ERROR) {
        return VOICE_BEANHOPPERWARNING;
    }

    return VOICE_FIRSTFAULTWARNING;
}

static voice_id_t alarm_current_voice(const app_ctx_t *ctx)
{
    if (!ctx || !ctx->alarm.active) {
        return VOICE_NONE;
    }

    return ctx->alarm.is_notice ? alarm_notice_voice(ctx) : alarm_fault_voice(ctx);
}

static void alarm_play_prompt_if_needed(app_ctx_t *ctx)
{
    voice_id_t voice = VOICE_NONE;
    bool changed;

    if (!ctx || !ctx->alarm.active) {
        return;
    }

    changed = (ctx->state_runtime.alarm.active != ctx->alarm.active) ||
              (ctx->state_runtime.alarm.is_notice != ctx->alarm.is_notice) ||
              (ctx->state_runtime.alarm.notice_p != ctx->alarm.notice_p) ||
              (ctx->state_runtime.alarm.warning != ctx->alarm.warning) ||
              (ctx->state_runtime.alarm.major != ctx->alarm.major) ||
              (ctx->state_runtime.alarm.sub != ctx->alarm.sub) ||
              (ctx->state_runtime.alarm.error_code != ctx->ms.error_code);

    if (changed) {
        ctx->state_runtime.alarm.active = ctx->alarm.active;
        ctx->state_runtime.alarm.is_notice = ctx->alarm.is_notice;
        ctx->state_runtime.alarm.notice_p = ctx->alarm.notice_p;
        ctx->state_runtime.alarm.warning = ctx->alarm.warning;
        ctx->state_runtime.alarm.major = ctx->alarm.major;
        ctx->state_runtime.alarm.sub = ctx->alarm.sub;
        ctx->state_runtime.alarm.error_code = ctx->ms.error_code;
        ctx->state_runtime.alarm.fault_enter_tick = ctx->timer.tick;
        ctx->state_runtime.alarm.second_fault_played = false;
        ctx->state_runtime.alarm.fault_cancel_sent = false;
        voice = ctx->alarm.is_notice ? alarm_notice_voice(ctx) : alarm_fault_voice(ctx);
        if (voice != VOICE_NONE) {
            if (voice == VOICE_CONTINUECLEANAFTERPOWEROFF) {
                ctx->state_runtime.maint.resume_prompt_played = true;
            }
            voice_manager_play_interrupt(voice);
        }
        return;
    }

    if (!ctx->alarm.is_notice &&
        alarm_is_generic_fault(ctx->ms.error_code) &&
        !ctx->state_runtime.alarm.second_fault_played &&
        (ctx->timer.tick - ctx->state_runtime.alarm.fault_enter_tick >= STAY_TICKS(10000))) {
        ctx->state_runtime.alarm.second_fault_played = true;
        voice_manager_play_interrupt(VOICE_SECONDFAULTWARNING);
    }
}

static app_state_t alarm_route_notice(app_ctx_t *ctx)
{
    if (ctx->input.key_pressed & (1U << KEY_CLEAN)) {
        ctx->input.key_pressed &= ~(1U << KEY_CLEAN);

        switch (ctx->alarm.notice_type) {
        case MAINT_TYPE_BREW:
            sp_pro_consume_notice(ctx);
            return ST_MAINT_BREW;

        case MAINT_TYPE_DES:
            sp_pro_consume_notice(ctx);
            return ST_MAINT_DES;

        case MAINT_TYPE_STEAM:
            sp_pro_consume_notice(ctx);
            return ST_MAINT_STEAM;

        default:
            break;
        }
    }

    if (ctx->input.key_long & (1U << KEY_CLEAN)) {
        ctx->input.key_long &= ~(1U << KEY_CLEAN);
        sp_pro_dismiss_notice(ctx);
        return ST_READY;
    }

    return ctx->core.prev_state;
}

static void alarm_handle_water_pump_retry(app_ctx_t *ctx)
{
    voice_id_t voice = VOICE_NONE;

    if (!ctx || !alarm_is_water_pump_fault(ctx)) {
        return;
    }

    if (!ctx->state_runtime.alarm.water_pump_replenishing) {
        return;
    }

    if ((ctx->timer.tick - ctx->state_runtime.alarm.water_pump_retry_tick) < WATER_PUMP_RETRY_TIMEOUT_TICKS) {
        return;
    }

    ctx->state_runtime.alarm.water_pump_replenishing = false;
    if (ctx->state_runtime.alarm.water_pump_retry_count < 0xFFU) {
        ctx->state_runtime.alarm.water_pump_retry_count++;
    }

    voice = (ctx->state_runtime.alarm.water_pump_retry_count >= 3U)
              ? VOICE_WATERPUMPFAULT
              : VOICE_SYSTEMLACKSWATER;
    ESP_LOGW(TAG, "H05 replenish failed, retry=%u", ctx->state_runtime.alarm.water_pump_retry_count);
    voice_manager_play_interrupt(voice);

}

static void alarm_handle_heater_fault(app_ctx_t *ctx)
{
    if (!ctx || !alarm_is_heater_fault(ctx)) {
        return;
    }

    if (ctx->state_runtime.alarm.fault_cancel_sent) {
        return;
    }

    ctx->state_runtime.alarm.fault_cancel_sent = true;
    if (!ctr_cmd_action(CTRL_ACT_CANCEL, NULL)) {
        ESP_LOGW(TAG, "H01 cancel action rejected");
        return;
    }

    ESP_LOGW(TAG, "H01 fault cancel current process to protect heater");
}

static void alarm_try_start_water_pump_replenish(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->state_runtime.alarm.water_pump_replenishing) {
        ESP_LOGI(TAG, "H05 replenish ignored: already running");
        return;
    }

    if (ctx->state_runtime.alarm.water_pump_retry_count >= 3U) {
        ESP_LOGW(TAG, "H05 replenish ignored: retry limit reached=%u",
                 (unsigned)ctx->state_runtime.alarm.water_pump_retry_count);
        return;
    }

    if (ctr_cmd_action(CTRL_ACT_POWER, NULL)) {
        ctx->state_runtime.alarm.water_pump_replenishing = true;
        ctx->state_runtime.alarm.water_pump_retry_tick = ctx->timer.tick;
        ESP_LOGI(TAG, "H05 start replenish attempt=%u",
                 (unsigned)(ctx->state_runtime.alarm.water_pump_retry_count + 1U));
    } else {
        ESP_LOGW(TAG, "H05 replenish command rejected");
    }
}

void alarm_check(app_ctx_t *ctx)
{
    alarm_notice_t notice;
    uint32_t effective_error_code;

    if (!ctx) {
        return;
    }

    /* Once CTR clears a suppressed fault bit, allow it to surface again next time. */
    ctx->state_runtime.alarm.suppressed_error_mask &= ctx->ms.error_code;

    effective_error_code = alarm_effective_error_code(ctx);
#if APP_TEST_DISABLE_LOCAL_ALARM
    effective_error_code = 0U;
#endif
    if (ctx->core.state == ST_DETECTION) {
        /* Detection mode still suppresses P-class reminders so the sequence
         * is not hijacked by intentionally exercised sensors, but H-class
         * faults must remain visible for validation. */
        if (effective_error_code != 0U) {
            alarm_set_fault(ctx, alarm_parse_fault(effective_error_code));
            alarm_play_prompt_if_needed(ctx);
            return;
        }

        alarm_clear(ctx);
        return;
    }

    if (effective_error_code != 0U) {
        alarm_set_fault(ctx, alarm_parse_fault(effective_error_code));
        alarm_play_prompt_if_needed(ctx);
        return;
    }

    if (alarm_find_notice(ctx, &notice)) {
        alarm_set_notice(ctx, notice.indicator, notice.warning, notice.notice_type);
        alarm_play_prompt_if_needed(ctx);
        return;
    }

    alarm_clear(ctx);
}

app_state_t state_handle_alarm(app_ctx_t *ctx)
{
    uint32_t effective_error_code;
    uint16_t replay_keys;
    voice_id_t replay_voice;

    if (!ctx) {
        return ST_READY;
    }

    ctx->state_runtime.alarm.suppressed_error_mask &= ctx->ms.error_code;
    effective_error_code = alarm_effective_error_code(ctx);
#if APP_TEST_DISABLE_LOCAL_ALARM
    effective_error_code = 0U;
#endif

    if (effective_error_code == 0U) {
        alarm_clear(ctx);
        return ST_READY;
    }

    alarm_check(ctx);
    alarm_handle_water_pump_retry(ctx);
    alarm_handle_heater_fault(ctx);

    if (ctx->alarm.active && ctx->alarm.is_notice) {
        return alarm_route_notice(ctx);
    }

    if (ctx->alarm.major == 5) {
        if ((ctx->input.key_pressed & (1U << KEY_CLEAN)) != 0U) {
            ctx->input.key_pressed &= ~(1U << KEY_CLEAN);
            alarm_try_start_water_pump_replenish(ctx);
        }

        if ((ctx->input.key_long & (1U << KEY_CLEAN)) != 0U) {
            ctx->input.key_long &= ~(1U << KEY_CLEAN);
            alarm_try_start_water_pump_replenish(ctx);
        }
        return ST_ALARM;
    }

    if (ctx->alarm.major == 10 &&
        ctx->alarm.sub == 2 &&
        (ctx->input.key_pressed & (1U << KEY_GRIND)) != 0U) {
        ctx->input.key_pressed &= ~(1U << KEY_GRIND);
        ctx->state_runtime.alarm.suppressed_error_mask |= PRESSURE_VALUE_ERROR;
        ctr_cmd_action(CTRL_ACT_CANCEL, NULL);
        alarm_clear(ctx);
        return ST_READY;
    }

    replay_keys = ctx->input.key_pressed;
    if (replay_keys != 0U) {
        replay_voice = alarm_current_voice(ctx);
        ctx->input.key_pressed = 0U;
        if (replay_voice != VOICE_NONE) {
            voice_manager_play_interrupt(replay_voice);
        }
    }

    if (effective_error_code != 0U) {
        return ST_ALARM;
    }

    return ST_READY;
}
