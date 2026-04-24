#include "sp_pro_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "board_config.h"
#include "ctr_scheduler.h"
#include "led_service.h"
#include "device_statistics_store.h"
#include "drink_record_service.h"
#include "ble.h"
#include "mqtt.h"
#include "mqtt_protocol.h"
#include "ble_pairing.h"
#include "system_runtime.h"
#include "ota_ctr.h"
#include "formula_store.h"
#include "nvs.h"
#include "esp_err.h"
#include <stddef.h>

static const char *TAG = "sp_pro_app";
#define APP_TEST_DISABLE_LOCAL_ALARM 1
#define SP_PRO_LOGIC_TASK_STACK_SIZE_DEFAULT 12288
#define SP_PRO_LOGIC_TASK_STACK_SIZE_TEST    8192
#define SP_PRO_LOGIC_TASK_STACK_SIZE         SP_PRO_LOGIC_TASK_STACK_SIZE_TEST
#define DISPLAY_CLEAR_REINFORCE_FRAMES      8U
#define POWER_KEY_ACTIVE_LEVEL     0
#define POWER_KEY_DEBOUNCE_TICKS   STAY_TICKS(50)
#define POWER_KEY_LONG_PRESS_TICKS STAY_TICKS(2000)

/* Initial bench values for local liquid compensation.
 * TODO: move these into a dedicated calibration/settings entry once test-side
 * tuning is ready. */
/* Default to zero compensation until each drink is bench-calibrated.
 * Non-zero placeholders distort the user-facing display immediately. */
#define LIQ_COMP_ESPRESSO_DISPLAY_OFFSET_ML    0.0f
#define LIQ_COMP_ESPRESSO_STOP_AHEAD_ML        0.0f
#define LIQ_COMP_AME_BREW_DISPLAY_OFFSET_ML    0.0f
#define LIQ_COMP_AME_BREW_STOP_AHEAD_ML        0.0f
#define LIQ_COMP_COLD_BREW_DISPLAY_OFFSET_ML   0.0f
#define LIQ_COMP_COLD_BREW_STOP_AHEAD_ML       0.0f
#define LIQ_COMP_AME_WATER_DISPLAY_OFFSET_ML   0.0f
#define LIQ_COMP_AME_WATER_STOP_AHEAD_ML       0.0f
#define LIQ_COMP_HOT_WATER_DISPLAY_OFFSET_ML   0.0f
#define LIQ_COMP_HOT_WATER_STOP_AHEAD_ML       0.0f

static int s_last_auto_power_off_time = -1;
static int s_last_auto_stand_by_time = -1;
static uint32_t s_last_encoder_activity_seq = 0U;
static uint8_t s_display_clear_reinforce_frames = 0U;

#define SP_PRO_SETTINGS_NVS_NAMESPACE "sp_pro_app"
#define SP_PRO_SETTINGS_NVS_KEY       "settings_v1"
#define SP_PRO_SETTINGS_MAGIC         0x53505031UL
#define SP_PRO_SETTINGS_VERSION       1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    float esp_brew_w;
    float esp_brew_t;
    float ame_brew_w;
    float ame_brew_t;
    float ame_water_w;
    float ame_water_t;
    float cold_brew_w;
    float hot_water_w;
    float hot_water_t;
    float grind_w;
    float clean_v;
    float steam_level;
    uint8_t water_in_mode;
    uint8_t hardness;
    uint8_t maint_resume_flag;
    uint8_t maint_resume_type;
    uint8_t maint_resume_stage;
    uint8_t reserved2;
} sp_pro_persisted_settings_t;

typedef struct {
    bool initialized;
    bool raw_down;
    bool stable_down;
    bool long_fired;
    uint8_t debounce_ticks;
    uint16_t hold_ticks;
    bool short_event;
    bool long_event;
} power_key_state_t;

#if SUPPLIER_HMI_DEMO_MODE
typedef struct {
    uint16_t key_down_mask;
    uint8_t selected_key;
    uint8_t display_number;
    TickType_t blink_until_tick;
    bool has_selection;
} supplier_hmi_demo_state_t;
#endif

app_ctx_t g_ctx;
static disp_model_t g_disp_model;
static power_key_state_t s_power_key;
#if SUPPLIER_HMI_DEMO_MODE
static supplier_hmi_demo_state_t s_supplier_hmi_demo;
#endif
static void sp_pro_build_command_view_from_ctx(const app_ctx_t *ctx, app_command_view_t *view);
static void sp_pro_build_beverage_settings_from_ctx(const app_ctx_t *ctx, app_beverage_settings_t *settings);
static void sp_pro_apply_default_settings(app_ctx_t *ctx);
static void sp_pro_sync_beverage_settings_from_formula_store(app_ctx_t *ctx);
static void sp_pro_export_persisted_settings(const app_ctx_t *ctx, sp_pro_persisted_settings_t *persisted);
static void sp_pro_import_persisted_settings(app_ctx_t *ctx,
                                             const sp_pro_persisted_settings_t *persisted,
                                             size_t persisted_size);
static bool sp_pro_load_settings_from_nvs(app_ctx_t *ctx);
static void sp_pro_reset_clear_bean_runtime(app_ctx_t *ctx);
static void sp_pro_prepare_setting_leave(app_ctx_t *ctx, app_state_t cur);
static void sp_pro_clear_idle_deadlines(void);
static void sp_pro_refresh_idle_deadlines(void);
static void sp_pro_arm_standby_poweroff_deadline(void);
static void sp_pro_sync_idle_timeout_config(void);
static void sp_pro_capture_local_activity(void);
static app_state_t sp_pro_apply_power_timeout(app_state_t cur);
static void sp_pro_prepare_power_on_entry(void);
static void sp_pro_power_key_init(void);
static void sp_pro_power_key_tick(void);
static app_state_t sp_pro_apply_power_key(app_state_t cur);
static control_action_t sp_pro_shutdown_stop_action(const app_ctx_t *ctx, app_state_t cur);
static void sp_pro_stop_running_parts_for_off(app_ctx_t *ctx, app_state_t cur);
#if SUPPLIER_HMI_DEMO_MODE
static bool supplier_hmi_demo_blink_active(TickType_t now);
static void supplier_hmi_demo_apply_key_selection(uint8_t key_idx);
static void supplier_hmi_demo_render(disp_element_t *disp);
#endif
static void sp_pro_clear_local_alarm_state(app_ctx_t *ctx);
static void sp_pro_apply_state_side_effects(app_state_t cur, app_state_t next);
static void sp_pro_sync_led_effect(void);
static app_state_t sp_pro_apply_ota_ui(app_state_t cur);

static bool s_device_sensor_snapshot_valid = false;
static uint8_t s_last_brew_handle_postion_flag = 0;
static uint8_t s_last_grind_handle_postion_flag = 0;
static uint8_t s_last_beanbox_in_place = 0;
static uint8_t s_last_water_box_shortage_flag = 0;
static uint32_t s_last_error_code = 0;

static const drink_liquid_compensation_t s_espresso_liquid_comp = {
    LIQ_COMP_ESPRESSO_DISPLAY_OFFSET_ML,
    LIQ_COMP_ESPRESSO_STOP_AHEAD_ML,
};
static const drink_liquid_compensation_t s_americano_brew_liquid_comp = {
    LIQ_COMP_AME_BREW_DISPLAY_OFFSET_ML,
    LIQ_COMP_AME_BREW_STOP_AHEAD_ML,
};
static const drink_liquid_compensation_t s_cold_brew_liquid_comp = {
    LIQ_COMP_COLD_BREW_DISPLAY_OFFSET_ML,
    LIQ_COMP_COLD_BREW_STOP_AHEAD_ML,
};
static const drink_liquid_compensation_t s_americano_water_liquid_comp = {
    LIQ_COMP_AME_WATER_DISPLAY_OFFSET_ML,
    LIQ_COMP_AME_WATER_STOP_AHEAD_ML,
};
static const drink_liquid_compensation_t s_hot_water_liquid_comp = {
    LIQ_COMP_HOT_WATER_DISPLAY_OFFSET_ML,
    LIQ_COMP_HOT_WATER_STOP_AHEAD_ML,
};

static void sp_pro_check_device_sensor_changed(const MACHINE_STATUS *status)
{
    if (!status) {
        return;
    }

    if (!s_device_sensor_snapshot_valid) {
        s_last_brew_handle_postion_flag = status->brew_handle_postion_flag;
        s_last_grind_handle_postion_flag = status->grind_handle_postion_flag;
        s_last_beanbox_in_place = status->beanbox_in_place;
        s_last_water_box_shortage_flag = status->water_box_shortage_flag;
        s_last_error_code = status->error_code;
        s_device_sensor_snapshot_valid = true;
        return;
    }

    bool changed = false;

    if (s_last_brew_handle_postion_flag != status->brew_handle_postion_flag) {
        changed = true;
        s_last_brew_handle_postion_flag = status->brew_handle_postion_flag;
    }

    if (s_last_grind_handle_postion_flag != status->grind_handle_postion_flag) {
        changed = true;
        s_last_grind_handle_postion_flag = status->grind_handle_postion_flag;
    }

    if (s_last_beanbox_in_place != status->beanbox_in_place) {
        changed = true;
        s_last_beanbox_in_place = status->beanbox_in_place;
    }

    if (s_last_water_box_shortage_flag != status->water_box_shortage_flag) {
        changed = true;
        s_last_water_box_shortage_flag = status->water_box_shortage_flag;
    }

    if (s_last_error_code != status->error_code) {
        changed = true;
        s_last_error_code = status->error_code;
    }

    if (changed) {
        mqtt_report_device_status_sections(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                           "device_sensor_changed");
    }
}

drink_liquid_compensation_t sp_pro_get_active_liquid_compensation(const app_ctx_t *ctx)
{
    if (!ctx) {
        return (drink_liquid_compensation_t){0};
    }

    switch (ctx->core.state) {
    case ST_ESPRESSO:
    case ST_MASTER:
        return s_espresso_liquid_comp;

    case ST_AMERICANO:
        if (ctx->core.substate == BREW_SUB_RUNNING_2 ||
            ctx->core.substate == BREW_SUB_FINISH ||
            ctx->drink.target_ml == (uint16_t)(ctx->setting.ame_water_w + 0.5f)) {
            return s_americano_water_liquid_comp;
        }
        return s_americano_brew_liquid_comp;

    case ST_COLD_BREW:
        return s_cold_brew_liquid_comp;

    case ST_WATER:
        return s_hot_water_liquid_comp;

    default:
        return (drink_liquid_compensation_t){0};
    }
}

float sp_pro_get_raw_liquid_progress_ml(const app_ctx_t *ctx)
{
    float raw_progress;

    if (!ctx) {
        return 0.0f;
    }

    raw_progress = ctx->ms.liquid_weight - ctx->state_runtime.drink.liquid_session_base_ml;
    if (raw_progress < 0.0f) {
        raw_progress = 0.0f;
    }
    return raw_progress;
}

float sp_pro_get_display_liquid_ml(const app_ctx_t *ctx)
{
    drink_liquid_compensation_t comp;
    float display_ml;

    if (!ctx) {
        return 0.0f;
    }

    if (ctx->core.state == ST_WATER ||
        (ctx->core.state == ST_AMERICANO &&
         (ctx->core.substate == BREW_SUB_RUNNING_2 ||
          ctx->core.substate == BREW_SUB_FINISH))) {
        display_ml = ctx->state_runtime.drink.display_liquid_ml;
    } else {
        display_ml = sp_pro_get_raw_liquid_progress_ml(ctx);
    }

    comp = sp_pro_get_active_liquid_compensation(ctx);
    display_ml -= comp.display_offset_ml;
    if (display_ml < 0.0f) {
        display_ml = 0.0f;
    }

    return display_ml;
}

static led_sys_state_t sp_pro_led_state_for_drink(const app_ctx_t *ctx)
{
    if (!ctx) {
        return LED_EFFECT_OFF;
    }

    switch (ctx->core.state) {
    case ST_COLD_BREW:
        return (ctx->core.substate == BREW_SUB_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_BLUE_BREATH;

    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_AMERICANO:
    case ST_WATER:
        return (ctx->core.substate == BREW_SUB_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_ORANGE_BREATH;

    case ST_GRIND:
        return (ctx->core.substate == BREW_SUB_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_WHITE_BREATH;

    case ST_STEAM:
        if (ctx->core.substate == BREW_SUB_TAIL_SPRAY_COUNTDOWN ||
            ctx->core.substate == BREW_SUB_TAIL_SPRAY_RUNNING) {
            return LED_EFFECT_RED_BREATH;
        }

        return (ctx->core.substate == BREW_SUB_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_ORANGE_BREATH;

    default:
        return LED_EFFECT_OFF;
    }
}

static led_sys_state_t sp_pro_led_state_for_maint(const app_ctx_t *ctx)
{
    if (!ctx) {
        return LED_EFFECT_OFF;
    }

    switch (ctx->core.state) {
    case ST_CLEAN_BREW:
        return (ctx->maint.clean_state == MAINT_CLEAN_SUB_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_WHITE_BREATH;

    case ST_MAINT_BREW:
        return (ctx->state_runtime.maint.brew_step == BREW_STEP_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_WHITE_BREATH;

    case ST_MAINT_DES:
        return (ctx->state_runtime.maint.des_step == DES_STEP_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_WHITE_BREATH;

    case ST_MAINT_STEAM:
        return (ctx->state_runtime.maint.steam_step == STEAM_STEP_FINISH) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_WHITE_BREATH;

    case ST_MAINT_DRAIN:
        switch (ctx->setting.clear_step) {
        case CLEAR_STEP_CUTOFF_SUPPLY:
        case CLEAR_STEP_CLEARING:
            return LED_EFFECT_WHITE_BREATH;

        case CLEAR_STEP_CLEARING_DONE:
        case CLEAR_STEP_FACTORY_RESET:
            return LED_EFFECT_WHITE_SOLID;

        case CLEAR_STEP_WILL_SHUTDOWN:
        case CLEAR_STEP_NOW_SHUTDOWN:
            return LED_EFFECT_OFF;

        case CLEAR_STEP_NONE:
        default:
            return LED_EFFECT_WHITE_BREATH;
        }

    default:
        return LED_EFFECT_OFF;
    }
}

static led_sys_state_t sp_pro_led_state_for_ctx(const app_ctx_t *ctx)
{
    if (!ctx) {
        return LED_EFFECT_OFF;
    }

    if (ctx->alarm.active) {
        return LED_EFFECT_RED_BREATH;
    }

    switch (ctx->core.state) {
    case ST_OFF:
        return LED_EFFECT_OFF;

    case ST_ON:
        switch ((power_on_substate_t)ctx->core.substate) {
        case POWER_ON_SUB_FLASH:
            return LED_EFFECT_WHITE_HIGHLIGHT;
        case POWER_ON_SUB_REPLENISH:
            return LED_EFFECT_WHITE_SOLID;
        case POWER_ON_SUB_LOADING:
            return LED_EFFECT_WHITE_BREATH;
        default:
            return LED_EFFECT_WHITE_HIGHLIGHT;
        }

    case ST_READY:
        return LED_EFFECT_WHITE_HIGHLIGHT;

    case ST_WIFI:
        return (sys_pra.wifi_state == WIFI_CONNECTED) ?
               LED_EFFECT_WHITE_HIGHLIGHT :
               LED_EFFECT_WHITE_BREATH;

    case ST_OTA:
        switch ((ota_ui_substate_t)ctx->core.substate) {
        case OTA_UI_REMINDER:
            return LED_EFFECT_RED_BREATH;
        case OTA_UI_UPGRADING:
            return LED_EFFECT_WHITE_BREATH;
        case OTA_UI_SUCCESS:
        case OTA_UI_FAIL:
            return LED_EFFECT_WHITE_SOLID;
        case OTA_UI_NONE:
        default:
            return LED_EFFECT_WHITE_SOLID;
        }

    case ST_STANDBY:
    case ST_LOCK:
        return LED_EFFECT_WHITE_HALF;

    case ST_DRINK_SET:
    case ST_SETTING:
        return LED_EFFECT_WHITE_HIGHLIGHT;

    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_AMERICANO:
    case ST_COLD_BREW:
    case ST_WATER:
    case ST_STEAM:
    case ST_GRIND:
        return sp_pro_led_state_for_drink(ctx);

    case ST_CLEAR_BEAN:
        return LED_EFFECT_WHITE_SOLID;

    case ST_CLEAN_BREW:
    case ST_MAINT_BREW:
    case ST_MAINT_DES:
    case ST_MAINT_STEAM:
    case ST_MAINT_DRAIN:
        return sp_pro_led_state_for_maint(ctx);

    case ST_CALIBRATION:
        return (ctx->calibration.step == CAL_STEP_FAIL) ?
               LED_EFFECT_RED_SOLID :
               LED_EFFECT_WHITE_SOLID;

    case ST_DETECTION:
        return (ctx->detection.step == DET_STEP_FAIL) ?
               LED_EFFECT_RED_SOLID :
               LED_EFFECT_WHITE_SOLID;

    case ST_AUTO_TEST:
        return LED_EFFECT_BLUE_BREATH;

    case ST_ALARM:
        return LED_EFFECT_RED_BREATH;

    default:
        return LED_EFFECT_OFF;
    }
}

static void sp_pro_sync_led_effect(void)
{
    led_service_set_state(sp_pro_led_state_for_ctx(&g_ctx));
}

static void sp_pro_force_visuals_off(void)
{
    disp_clear(&g_disp_model.disp);
    g_disp_model.disp.frame_id = (uint8_t)(g_disp_model.disp.frame_id + 1U);
    g_disp_model.dirty = true;
    led_service_set_state(LED_EFFECT_OFF);
}

#if SUPPLIER_HMI_DEMO_MODE
static bool supplier_hmi_demo_blink_active(TickType_t now)
{
    return s_supplier_hmi_demo.has_selection &&
           ((int32_t)(s_supplier_hmi_demo.blink_until_tick - now) > 0);
}

static void supplier_hmi_demo_apply_key_selection(uint8_t key_idx)
{
    if (key_idx >= BF_KEY_COUNT) {
        return;
    }

    s_supplier_hmi_demo.selected_key = key_idx;
    s_supplier_hmi_demo.display_number = (uint8_t)(key_idx + 1U);
    s_supplier_hmi_demo.blink_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    s_supplier_hmi_demo.has_selection = true;
    g_disp_model.dirty = true;
}

static void supplier_hmi_demo_render(disp_element_t *disp)
{
    TickType_t now;
    bool blink_active;

    if (!disp) {
        return;
    }

    now = xTaskGetTickCount();
    blink_active = supplier_hmi_demo_blink_active(now);

    disp_clear(disp);

    for (uint8_t key_idx = 0U; key_idx < BF_KEY_COUNT; key_idx++) {
        disp_set_key_icon(disp, key_idx, WHITE_LIGHT_HALF);
        disp_set_key_text(disp, key_idx, WHITE_LIGHT_HALF);
        disp_set_key_blink(disp, key_idx, false);
    }

    if (!s_supplier_hmi_demo.has_selection) {
        disp_show_number(disp, SM_POS_1, 88U, 2);
        return;
    }

    if (s_supplier_hmi_demo.display_number >= 10U) {
        disp_show_number(disp, SM_POS_1, s_supplier_hmi_demo.display_number, 2);
    } else {
        disp_show_number(disp, SM_POS_2, s_supplier_hmi_demo.display_number, 1);
    }

    disp_set_key_icon(disp, s_supplier_hmi_demo.selected_key, WHITE_LIGHT_FULL);
    disp_set_key_text(disp, s_supplier_hmi_demo.selected_key, WHITE_LIGHT_FULL);
    disp_set_key_blink(disp, s_supplier_hmi_demo.selected_key, blink_active);
}
#endif

static bool sp_pro_power_key_raw_is_down(void)
{
    return gpio_get_level(PIN_POWER_KEY) == POWER_KEY_ACTIVE_LEVEL;
}

static void sp_pro_power_key_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_POWER_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);

    memset(&s_power_key, 0, sizeof(s_power_key));
    s_power_key.initialized = true;
    s_power_key.raw_down = sp_pro_power_key_raw_is_down();
    s_power_key.stable_down = s_power_key.raw_down;
}

static void sp_pro_power_key_tick(void)
{
    bool raw_down;

    if (!s_power_key.initialized) {
        return;
    }

    raw_down = sp_pro_power_key_raw_is_down();
    if (raw_down != s_power_key.raw_down) {
        s_power_key.raw_down = raw_down;
        s_power_key.debounce_ticks = POWER_KEY_DEBOUNCE_TICKS;
    } else if (s_power_key.debounce_ticks > 0U) {
        s_power_key.debounce_ticks--;
        if (s_power_key.debounce_ticks == 0U && s_power_key.stable_down != raw_down) {
            s_power_key.stable_down = raw_down;
            if (raw_down) {
                s_power_key.hold_ticks = 0U;
                s_power_key.long_fired = false;
            } else {
                if (!s_power_key.long_fired && s_power_key.hold_ticks > 0U) {
                    s_power_key.short_event = true;
                }
                s_power_key.hold_ticks = 0U;
                s_power_key.long_fired = false;
            }
        }
    }

    if (s_power_key.stable_down) {
        if (s_power_key.hold_ticks < 0xFFFFU) {
            s_power_key.hold_ticks++;
        }

        if (!s_power_key.long_fired && s_power_key.hold_ticks >= POWER_KEY_LONG_PRESS_TICKS) {
            s_power_key.long_event = true;
            s_power_key.long_fired = true;
        }
    }
}

static void sp_pro_apply_state_side_effects(app_state_t cur, app_state_t next)
{
    if ((next == ST_OFF || next == ST_STANDBY) &&
        (cur != next)) {
        s_display_clear_reinforce_frames = DISPLAY_CLEAR_REINFORCE_FRAMES;
    } else if (next != ST_OFF && next != ST_STANDBY) {
        s_display_clear_reinforce_frames = 0U;
    }

    if (next != ST_READY) {
        g_ctx.state_runtime.ready.steam_click_count = 0U;
        g_ctx.state_runtime.ready.steam_click_deadline = 0U;
    }

    if (cur == ST_OTA && next != ST_OTA) {
        g_ctx.state_runtime.ota.entered = false;
        g_ctx.state_runtime.ota.last_phase = OTA_UI_NONE;
        g_ctx.state_runtime.ota.phase_voice_played = false;
    }

    if (cur == ST_CLEAR_BEAN && next != ST_CLEAR_BEAN) {
        sp_pro_reset_clear_bean_runtime(&g_ctx);
    }

    if ((cur == ST_DRINK_SET || cur == ST_SETTING) &&
        next != ST_DRINK_SET &&
        next != ST_SETTING) {
        sp_pro_prepare_setting_leave(&g_ctx, cur);
    }

    if (next == ST_OFF) {
        if (cur != ST_OFF) {
            sp_pro_stop_running_parts_for_off(&g_ctx, cur);
            sp_pro_clear_local_alarm_state(&g_ctx);
            sp_pro_force_visuals_off();
            voice_manager_play(VOICE_POWEROFF);
            mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                        "local_power_off",
                                                        0U);
        }
        sys_pra.power_off_flag = 1;
        sp_pro_clear_idle_deadlines();
    } else {
        if (cur == ST_OFF) {
            sys_pra.power_off_flag = 0;
            mqtt_notify_power_on_reupload_needed();
            mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                        "local_power_on",
                                                        0U);
        }

        if (next == ST_ON) {
            if (cur != ST_ON) {
                voice_manager_play(VOICE_POWERON);
            }
            sp_pro_prepare_power_on_entry();
            sp_pro_refresh_idle_deadlines();
        } else if (next == ST_READY) {
            if (cur == ST_STANDBY) {
                ctr_cmd_action(CTRL_ACT_STEAM_SET_NORMAL, NULL);
            }
            sp_pro_refresh_idle_deadlines();
            if (cur == ST_ON || cur == ST_OFF) {
                if (!ctr_cmd_action(CTRL_ACT_FACTORY_READ, NULL)) {
                    ESP_LOGW(TAG, "factory read request rejected during READY entry");
                }
                mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_SENSORS,
                                                            "local_power_ready",
                                                            0U);
            }
        } else if (next == ST_STANDBY) {
            g_ctx.timer.standby_timer = 0U;
            ctr_cmd_action(CTRL_ACT_STANDBY, NULL);
            sp_pro_arm_standby_poweroff_deadline();
        }
    }

    if (cur != next) {
        app_beverage_settings_t beverage_settings = {
            .esp_brew_w = g_ctx.setting.esp_brew_w,
            .esp_brew_t = g_ctx.setting.esp_brew_t,
            .ame_brew_w = g_ctx.setting.ame_brew_w,
            .ame_brew_t = g_ctx.setting.ame_brew_t,
            .ame_water_w = g_ctx.setting.ame_water_w,
            .ame_water_t = g_ctx.setting.ame_water_t,
            .cold_brew_w = g_ctx.setting.cold_brew_w,
            .grind_w = g_ctx.setting.grind_w,
        };

        device_statistics_notify_local_state_start(next);
        drink_record_notify_local_state_start(next, &beverage_settings);
    }

    sp_pro_sync_led_effect();
}

static control_action_t sp_pro_shutdown_stop_action(const app_ctx_t *ctx, app_state_t cur)
{
    if (!ctx) {
        return CTRL_ACT_NONE;
    }

    switch (cur) {
    case ST_STEAM:
        return CTRL_ACT_STEAM_STOP;

    case ST_GRIND:
    case ST_CLEAR_BEAN:
        return CTRL_ACT_GRIND_STOP;

    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_AMERICANO:
    case ST_COLD_BREW:
    case ST_WATER:
    case ST_CLEAN_BREW:
    case ST_MAINT_BREW:
    case ST_MAINT_DES:
    case ST_MAINT_STEAM:
    case ST_MAINT_DRAIN:
        return CTRL_ACT_CANCEL;

    default:
        break;
    }

    if (ctx->ms.steam_flag == STEAM_RUNNING) {
        return CTRL_ACT_STEAM_STOP;
    }

    if (ctx->ms.grind_run_flg != 0U) {
        return CTRL_ACT_GRIND_STOP;
    }

    if (ctx->ms.hot_water_flg != 0U ||
        ctx->ms.drink_making_flg != DRINK_MAKER_NONE) {
        return CTRL_ACT_CANCEL;
    }

    return CTRL_ACT_NONE;
}

static void sp_pro_stop_running_parts_for_off(app_ctx_t *ctx, app_state_t cur)
{
    control_action_t action;

    action = sp_pro_shutdown_stop_action(ctx, cur);
    if (action == CTRL_ACT_NONE) {
        return;
    }

    ESP_LOGI(TAG, "Prepare ST_OFF: send stop action=%d from state=%d",
             action,
             cur);
    if (!ctr_cmd_action(action, NULL)) {
        ESP_LOGW(TAG, "Prepare ST_OFF: stop action %d rejected", action);
    }
}

static void sp_pro_clear_local_alarm_state(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->alarm.active = false;
    ctx->alarm.is_notice = false;
    ctx->alarm.major = 0U;
    ctx->alarm.sub = 0U;
    ctx->alarm.notice_p = BF_STATUS_NO;
    ctx->alarm.warning = WARN_NONE;
    ctx->alarm.notice_type = MAINT_TYPE_NONE;

    ctx->state_runtime.alarm.active = false;
    ctx->state_runtime.alarm.is_notice = false;
    ctx->state_runtime.alarm.major = 0U;
    ctx->state_runtime.alarm.sub = 0U;
    ctx->state_runtime.alarm.notice_p = BF_STATUS_NO;
    ctx->state_runtime.alarm.warning = WARN_NONE;
    ctx->state_runtime.alarm.error_code = 0U;
    ctx->state_runtime.alarm.suppressed_error_mask = 0U;
    ctx->state_runtime.alarm.fault_enter_tick = 0U;
    ctx->state_runtime.alarm.second_fault_played = false;
    ctx->state_runtime.alarm.water_pump_retry_count = 0U;
    ctx->state_runtime.alarm.water_pump_replenishing = false;
    ctx->state_runtime.alarm.water_pump_retry_tick = 0U;
    ctx->state_runtime.alarm.fault_cancel_sent = false;
    ctx->state_runtime.alarm.shutdown_pending = false;
    ctx->state_runtime.alarm.shutdown_tick = 0U;
}

static void sp_pro_reset_clear_bean_runtime(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->clear_bean.step = CLEAR_BEAN_STEP_NONE;
    ctx->clear_bean.step_tick = 0U;
    ctx->state_runtime.clear_bean.entered = false;
    ctx->state_runtime.clear_bean.grinder_started = false;
}

static void sp_pro_prepare_setting_leave(app_ctx_t *ctx, app_state_t cur)
{
    if (!ctx) {
        return;
    }

    if (cur == ST_DRINK_SET) {
        if (ctx->setting.formula_dirty) {
            if (!formula_store_sync_local_setting(ctx->setting.drink_type)) {
                ESP_LOGW(TAG, "Skip formulaOverall sync for drink_type=%u", ctx->setting.drink_type);
            } else {
                mqtt_report_device_status();
            }
        }
        sp_pro_app_save_settings();
        ctx->setting.active = false;
        ctx->setting.formula_dirty = false;
        return;
    }

    if (cur == ST_SETTING) {
        sp_pro_app_save_settings();
        ctx->state_runtime.setting.water_in_entered = false;
        ctx->state_runtime.setting.water_hardness_entered = false;
        ctx->state_runtime.setting.factory_reset_entered = false;
        ctx->state_runtime.setting.factory_reset_step_tick = 0U;
        ctx->state_runtime.setting.factory_reset_prompt_voice_count = 0U;
        ctx->state_runtime.setting.factory_reset_done_voice_count = 0U;
    }
}

static void sp_pro_prepare_power_on_entry(void)
{
    memset(&g_ctx.state_runtime.power_on, 0, sizeof(g_ctx.state_runtime.power_on));
    g_ctx.core.substate = (brew_substate_t)POWER_ON_SUB_FLASH;
    g_ctx.anim.active = false;
    g_ctx.anim.step = 0U;
    g_ctx.anim.tick = 0U;
    g_ctx.anim.interval = 0U;
    g_ctx.state_runtime.power_on.phase_tick = g_ctx.timer.tick;
}

static ota_ui_substate_t sp_pro_current_ota_ui_phase(void)
{
    if (g_ota_info.ota_auto_up[0] == '1') {
        return OTA_UI_NONE;
    }

    switch (g_ota_info.ota_sta) {
    case IOT_SIMPLE_OTA_WAIT_CONFIRM:
        return (g_ota_info.ota_auto_up[0] == '9') ?
               OTA_UI_UPGRADING :
               OTA_UI_REMINDER;

    case IOT_SIMPLE_OTA_YMODEM:
        return (g_ota_info.ota_auto_up[0] == '9') ?
               OTA_UI_UPGRADING :
               OTA_UI_NONE;

    case IOT_SIMPLE_OTA_SUCCESS:
        return (g_ota_info.ota_auto_up[0] == '9') ?
               OTA_UI_SUCCESS :
               OTA_UI_NONE;

    case IOT_SIMPLE_OTA_FAIL:
        return (g_ota_info.ota_auto_up[0] == '9') ?
               OTA_UI_FAIL :
               OTA_UI_NONE;

    default:
        return OTA_UI_NONE;
    }
}

static bool sp_pro_ota_prompt_dismissed(void)
{
    return g_ota_info.ota_tkid[0] != '\0' &&
           strcmp(g_ctx.state_runtime.ota.dismissed_task_id, g_ota_info.ota_tkid) == 0;
}

static app_state_t sp_pro_apply_ota_ui(app_state_t cur)
{
    ota_ui_substate_t phase = sp_pro_current_ota_ui_phase();

    if (cur == ST_OTA) {
        if (phase == OTA_UI_NONE || (phase == OTA_UI_REMINDER && sp_pro_ota_prompt_dismissed())) {
            return ST_READY;
        }

        g_ctx.core.substate = (brew_substate_t)phase;
        return ST_OTA;
    }

    if (cur == ST_READY &&
        phase != OTA_UI_NONE &&
        !(phase == OTA_UI_REMINDER && sp_pro_ota_prompt_dismissed())) {
        g_ctx.core.substate = (brew_substate_t)phase;
        return ST_OTA;
    }

    return cur;
}

static app_state_t sp_pro_apply_power_key(app_state_t cur)
{
    bool short_event = s_power_key.short_event;
    bool long_event = s_power_key.long_event;

    s_power_key.short_event = false;
    s_power_key.long_event = false;

    if (!short_event && !long_event) {
        return cur;
    }

    switch (cur) {
    case ST_OFF:
        if (long_event) {
            ESP_LOGI(TAG, "Power key long press -> ST_ON");
            return ST_ON;
        }
        break;

    case ST_STANDBY:
        if (long_event) {
            ESP_LOGI(TAG, "Power key long press -> ST_OFF");
            voice_manager_play_touch_tone();
            return ST_OFF;
        }
        if (short_event) {
            ESP_LOGI(TAG, "Power key short press -> ST_READY");
            return ST_READY;
        }
        break;

    case ST_READY:
        if (long_event) {
            ESP_LOGI(TAG, "Power key long press -> ST_OFF");
            voice_manager_play_touch_tone();
            return ST_OFF;
        }
        if (short_event) {
            ESP_LOGI(TAG, "Power key short press -> ST_STANDBY");
            return ST_STANDBY;
        }
        break;

    case ST_DRINK_SET:
    case ST_SETTING:
        if (long_event) {
            ESP_LOGI(TAG, "Power key long press from state %d -> ST_OFF", cur);
            voice_manager_play_touch_tone();
            return ST_OFF;
        }
        if (short_event) {
            ESP_LOGI(TAG, "Power key short press from state %d -> ST_STANDBY", cur);
            return ST_STANDBY;
        }
        break;

    default:
        if (long_event) {
            ESP_LOGI(TAG, "Power key long press from state %d -> ST_OFF", cur);
            voice_manager_play_touch_tone();
            return ST_OFF;
        }
        break;
    }

    return cur;
}

void sp_pro_app_set_ctrl_src(ctrl_src_t src)
{
    g_ctx.ctrl.src = src;
}

void sp_pro_app_get_command_view(app_command_view_t *view)
{
    sp_pro_build_command_view_from_ctx(&g_ctx, view);
}

void sp_pro_app_get_beverage_settings(app_beverage_settings_t *settings)
{
    sp_pro_build_beverage_settings_from_ctx(&g_ctx, settings);
}

static const formula_info_t *sp_pro_find_formula_by_drink_id(const formula_overall_t *overall, uint8_t drink_id)
{
    if (!overall || !overall->formula_intel_list) {
        return NULL;
    }

    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        if (overall->formula_intel_list[i].drink_id == drink_id) {
            return &overall->formula_intel_list[i];
        }
    }

    return NULL;
}

static void sp_pro_sync_beverage_settings_from_formula_store(app_ctx_t *ctx)
{
    formula_overall_t overall = {0};
    const formula_info_t *espresso;
    const formula_info_t *americano;
    const formula_info_t *cold_brew;
    const formula_info_t *hot_water;

    if (!ctx) {
        return;
    }

    if (!formula_store_ensure_local_defaults()) {
        ESP_LOGW(TAG, "Skip beverage cache sync: formula store defaults unavailable");
        return;
    }

    if (!formula_store_get_overall_snapshot(&overall)) {
        ESP_LOGW(TAG, "Skip beverage cache sync: formula snapshot unavailable");
        return;
    }

    espresso = sp_pro_find_formula_by_drink_id(&overall, DRINK_ID_ESPRESSO);
    americano = sp_pro_find_formula_by_drink_id(&overall, DRINK_ID_AMERICAN);
    cold_brew = sp_pro_find_formula_by_drink_id(&overall, DRINK_ID_COLDBREW);
    hot_water = sp_pro_find_formula_by_drink_id(&overall, DRINK_ID_WATER);

    if (espresso) {
        ctx->setting.esp_brew_w = (float)espresso->preset_liquid_weight;
        ctx->setting.esp_brew_t = (float)espresso->preset_temperature;
        if (espresso->grind_weight > 0U) {
            ctx->setting.grind_w = (float)espresso->grind_weight;
        }
    }

    if (americano) {
        ctx->setting.ame_brew_w = (float)americano->preset_liquid_weight;
        ctx->setting.ame_brew_t = (float)americano->preset_temperature;
        ctx->setting.ame_water_w = (float)americano->water_weight;
        ctx->setting.ame_water_t = (float)americano->water_temperature;
        if (!espresso && americano->grind_weight > 0U) {
            ctx->setting.grind_w = (float)americano->grind_weight;
        }
    }

    if (cold_brew) {
        ctx->setting.cold_brew_w = (float)((cold_brew->preset_liquid_weight > 0U)
                                               ? cold_brew->preset_liquid_weight
                                               : cold_brew->water_weight);
        if (!espresso && !americano && cold_brew->grind_weight > 0U) {
            ctx->setting.grind_w = (float)cold_brew->grind_weight;
        }
    }

    if (hot_water) {
        ctx->setting.hot_water_w = (float)((hot_water->water_weight > 0U)
                                               ? hot_water->water_weight
                                               : hot_water->preset_liquid_weight);
        ctx->setting.hot_water_t = (float)((hot_water->water_temperature > 0U)
                                               ? hot_water->water_temperature
                                               : hot_water->preset_temperature);
    }

    ESP_LOGI(TAG,
             "Beverage cache synced from formula store: esp=%.1f/%.1f ame=%.1f/%.1f water=%.1f/%.1f cold=%.1f grind=%.1f",
             ctx->setting.esp_brew_w,
             ctx->setting.esp_brew_t,
             ctx->setting.ame_brew_w,
             ctx->setting.ame_brew_t,
             ctx->setting.hot_water_w,
             ctx->setting.hot_water_t,
             ctx->setting.cold_brew_w,
             ctx->setting.grind_w);
}

bool sp_pro_app_save_settings(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    sp_pro_persisted_settings_t persisted = {0};

    sp_pro_export_persisted_settings(&g_ctx, &persisted);

    err = nvs_open(SP_PRO_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(nvs_handle, SP_PRO_SETTINGS_NVS_KEY, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "settings saved to NVS");
    return true;
}

void sp_pro_app_set_clean_volume(float clean_v)
{
    g_ctx.setting.clean_v = clean_v;
}

float sp_pro_app_get_clean_volume(void)
{
    return g_ctx.setting.clean_v;
}

void sp_pro_app_set_water_in_mode(setting_water_in_t mode)
{
    g_ctx.setting.water_in_mode = mode;
}

static void sp_pro_apply_default_settings(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    /* Beverage params are hydrated from formula_store and only cached here. */
    ctx->setting.esp_brew_w = 0.0f;
    ctx->setting.esp_brew_t = 0.0f;
    ctx->setting.ame_brew_w = 0.0f;
    ctx->setting.ame_brew_t = 0.0f;
    ctx->setting.ame_water_w = 0.0f;
    ctx->setting.ame_water_t = 0.0f;
    ctx->setting.cold_brew_w = 0.0f;
    ctx->setting.hot_water_w = 0.0f;
    ctx->setting.hot_water_t = 0.0f;
    ctx->setting.grind_w = 0.0f;
    ctx->setting.clean_v = 30.0f;
    ctx->setting.steam_level = 2.0f;
    ctx->setting.water_in_mode = WATER_IN_MODE_BUCKET;
    ctx->setting.hardness = HARDNESS_LEVEL_B;
    ctx->child_lock.enabled = 0;
}

static void sp_pro_export_persisted_settings(const app_ctx_t *ctx, sp_pro_persisted_settings_t *persisted)
{
    if (!ctx || !persisted) {
        return;
    }

    memset(persisted, 0, sizeof(*persisted));
    persisted->magic = SP_PRO_SETTINGS_MAGIC;
    persisted->version = SP_PRO_SETTINGS_VERSION;
    persisted->clean_v = ctx->setting.clean_v;
    persisted->steam_level = ctx->setting.steam_level;
    persisted->water_in_mode = (uint8_t)ctx->setting.water_in_mode;
    persisted->hardness = (uint8_t)ctx->setting.hardness;
    persisted->maint_resume_flag = ctx->maint.resume_flag ? 1U : 0U;
    persisted->maint_resume_type = (uint8_t)ctx->maint.type;
    persisted->maint_resume_stage = ctx->maint.resume_stage;
}

static bool sp_pro_persisted_field_available(size_t persisted_size,
                                             size_t field_offset,
                                             size_t field_size)
{
    return persisted_size >= (field_offset + field_size);
}

static void sp_pro_import_persisted_settings(app_ctx_t *ctx,
                                             const sp_pro_persisted_settings_t *persisted,
                                             size_t persisted_size)
{
    if (!ctx || !persisted) {
        return;
    }

    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, clean_v), sizeof(persisted->clean_v))) {
        ctx->setting.clean_v = persisted->clean_v;
    }
    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, steam_level), sizeof(persisted->steam_level))) {
        ctx->setting.steam_level = persisted->steam_level;
    }
    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, water_in_mode), sizeof(persisted->water_in_mode))) {
        ctx->setting.water_in_mode = (setting_water_in_t)persisted->water_in_mode;
    }
    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, hardness), sizeof(persisted->hardness))) {
        ctx->setting.hardness = (setting_water_hardness_t)persisted->hardness;
    }
    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, maint_resume_flag), sizeof(persisted->maint_resume_flag))) {
        ctx->maint.resume_flag = (persisted->maint_resume_flag != 0U);
    }
    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, maint_resume_type), sizeof(persisted->maint_resume_type))) {
        ctx->maint.type = (maint_type_t)persisted->maint_resume_type;
    }
    if (sp_pro_persisted_field_available(persisted_size, offsetof(sp_pro_persisted_settings_t, maint_resume_stage), sizeof(persisted->maint_resume_stage))) {
        ctx->maint.resume_stage = persisted->maint_resume_stage;
    }

    if (ctx->maint.type != MAINT_TYPE_STEAM &&
        ctx->maint.type != MAINT_TYPE_BREW &&
        ctx->maint.type != MAINT_TYPE_DES) {
        ctx->maint.resume_flag = false;
        ctx->maint.type = MAINT_TYPE_NONE;
        ctx->maint.resume_stage = MAINT_PROGRESS_NONE;
    }
    if (!ctx->maint.resume_flag) {
        ctx->maint.type = MAINT_TYPE_NONE;
        ctx->maint.resume_stage = MAINT_PROGRESS_NONE;
    }
}

static bool sp_pro_load_settings_from_nvs(app_ctx_t *ctx)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    sp_pro_persisted_settings_t persisted = {0};
    size_t size = sizeof(persisted);

    if (!ctx) {
        return false;
    }

    err = nvs_open(SP_PRO_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings nvs_open(read) failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_blob(nvs_handle, SP_PRO_SETTINGS_NVS_KEY, &persisted, &size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "settings load failed: %s", esp_err_to_name(err));
        }
        return false;
    }

    if (size < offsetof(sp_pro_persisted_settings_t, esp_brew_w) + sizeof(persisted.esp_brew_w) ||
        size > sizeof(persisted) ||
        persisted.magic != SP_PRO_SETTINGS_MAGIC ||
        persisted.version != SP_PRO_SETTINGS_VERSION) {
        ESP_LOGW(TAG, "settings blob invalid, ignore (size=%u magic=0x%08lX version=%u)",
                 (unsigned)size,
                 (unsigned long)persisted.magic,
                 (unsigned)persisted.version);
        return false;
    }

    sp_pro_import_persisted_settings(ctx, &persisted, size);
    ESP_LOGI(TAG, "settings restored from NVS (size=%u)", (unsigned)size);
    return true;
}

void sp_pro_app_restore_default_settings(void)
{
    sp_pro_apply_default_settings(&g_ctx);
    sp_pro_sync_beverage_settings_from_formula_store(&g_ctx);
    sp_pro_app_save_settings();
}

void sp_pro_app_reload_beverage_settings_from_formula_store(void)
{
    sp_pro_sync_beverage_settings_from_formula_store(&g_ctx);
}

bool sp_pro_app_is_off(void)
{
    return g_ctx.core.state == ST_OFF;
}

void sp_pro_app_enter_on(void)
{
    app_state_t cur = g_ctx.core.state;

    g_ctx.core.state = ST_ON;
    g_ctx.core.substate = 0;
    sp_pro_apply_state_side_effects(cur, ST_ON);
}

void sp_pro_app_enter_ready(void)
{
    app_state_t cur = g_ctx.core.state;

    g_ctx.core.state = ST_READY;
    g_ctx.core.substate = 0;
    sp_pro_apply_state_side_effects(cur, ST_READY);
}

void sp_pro_app_enter_off(void)
{
    app_state_t cur = g_ctx.core.state;

    g_ctx.core.state = ST_OFF;
    g_ctx.core.substate = 0;
    sp_pro_apply_state_side_effects(cur, ST_OFF);
    sp_pro_force_visuals_off();
}

bool sp_pro_app_toggle_child_lock(void)
{
    g_ctx.child_lock.enabled = g_ctx.child_lock.enabled ? 0 : 1;
    g_ctx.child_lock.press_tick = 0;
    g_ctx.child_lock.ui_hint = DISP_NO_LOCK;
    if (g_ctx.child_lock.enabled) {
        g_ctx.core.state = ST_LOCK;
    } else if (g_ctx.core.state == ST_LOCK) {
        g_ctx.core.state = ST_READY;
    }
    g_ctx.core.substate = 0;
    return g_ctx.child_lock.enabled != 0;
}

bool sp_pro_app_is_child_lock_enabled(void)
{
    return g_ctx.child_lock.enabled != 0;
}

app_state_t sp_pro_app_get_state(void)
{
    return g_ctx.core.state;
}

int sp_pro_app_get_maintain_notice_status(maint_type_t type)
{
    return (g_ctx.alarm.active && g_ctx.alarm.is_notice && g_ctx.alarm.notice_type == type) ? 1 : 0;
}

static void sp_pro_logic_task(void *pv_parameters);
static void sp_pro_build_drink_view(const app_ctx_t *ctx, app_drink_view_t *view);
static void sp_pro_build_clear_bean_view(const app_ctx_t *ctx, app_clear_bean_view_t *view);
static void sp_pro_build_setting_view(const app_ctx_t *ctx, app_setting_view_t *view);
static void sp_pro_build_calibration_view(const app_ctx_t *ctx, app_calibration_view_t *view);
static void sp_pro_build_detection_view(const app_ctx_t *ctx, app_detection_view_t *view);
static void sp_pro_build_alarm_view(const app_ctx_t *ctx, app_alarm_view_t *view);
static void sp_pro_build_child_lock_view(const app_ctx_t *ctx, app_child_lock_view_t *view);
static void sp_pro_build_display_view(const app_ctx_t *ctx, app_display_view_t *view);
static void sp_pro_sync_machine_status(app_ctx_t *ctx, const MACHINE_STATUS *status);
static void sp_pro_render_state_page(const app_display_view_t *view, disp_element_t *disp);
static void sp_pro_apply_alarm_overlay(const app_display_view_t *view, disp_element_t *disp);
static void sp_pro_commit_display_frame(disp_model_t *model,
                                        const disp_element_t *prev_disp,
                                        disp_element_t *next_disp);
static bool sp_pro_defer_fault_preemption(app_state_t state);
static bool sp_pro_map_remote_formula(const formula_info_t *formula,
                                      app_state_t *state,
                                      drink_type_t *drink);
static void sp_pro_apply_remote_formula_targets(app_ctx_t *ctx,
                                                const formula_info_t *formula,
                                                bool water_stage);

static uint8_t sp_pro_maint_notice_mask(maint_type_t type)
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

void sp_pro_consume_notice(app_ctx_t *ctx)
{
    if (!ctx || !ctx->alarm.active || !ctx->alarm.is_notice) {
        return;
    }

    ctx->alarm.active = false;
    ctx->alarm.is_notice = false;
    ctx->alarm.major = 0U;
    ctx->alarm.sub = 0U;
    ctx->alarm.notice_p = BF_STATUS_NO;
    ctx->alarm.warning = WARN_NONE;
    ctx->alarm.notice_type = MAINT_TYPE_NONE;

    ctx->state_runtime.alarm.active = false;
    ctx->state_runtime.alarm.is_notice = false;
    ctx->state_runtime.alarm.major = 0U;
    ctx->state_runtime.alarm.sub = 0U;
    ctx->state_runtime.alarm.notice_p = BF_STATUS_NO;
    ctx->state_runtime.alarm.warning = WARN_NONE;
    ctx->state_runtime.alarm.error_code = 0U;
    ctx->state_runtime.alarm.fault_enter_tick = 0U;
    ctx->state_runtime.alarm.second_fault_played = false;
    ctx->state_runtime.alarm.fault_cancel_sent = false;
}

void sp_pro_dismiss_notice(app_ctx_t *ctx)
{
    uint8_t notice_mask;

    if (!ctx || !ctx->alarm.active || !ctx->alarm.is_notice) {
        return;
    }

    notice_mask = sp_pro_maint_notice_mask(ctx->alarm.notice_type);
    if (notice_mask != 0U) {
        ctx->state_runtime.alarm.dismissed_maint_notice_mask |= notice_mask;
    }

    sp_pro_consume_notice(ctx);
}

static const state_handler_t state_handlers[] = {
    [ST_OFF]         = state_handle_off,
    [ST_ON]          = state_handle_on,
    [ST_READY]       = state_handle_ready,
    [ST_STANDBY]     = state_handle_standby,
    [ST_ESPRESSO]    = state_handle_espresso,
    [ST_MASTER]      = state_handle_master,
    [ST_AMERICANO]   = state_handle_americano,
    [ST_COLD_BREW]   = state_handle_cold_brew,
    [ST_WATER]       = state_handle_water,
    [ST_STEAM]       = state_handle_steam,
    [ST_GRIND]       = state_handle_grind,
    [ST_CLEAR_BEAN]  = state_handle_clear_bean,
    [ST_LOCK]        = state_handle_child_lock,
    [ST_CLEAN_BREW]  = state_handle_clean_brew,
    [ST_MAINT_BREW]  = state_handle_maint_brew,
    [ST_MAINT_DES]   = state_handle_maint_des,
    [ST_MAINT_STEAM] = state_handle_maint_steam,
    [ST_MAINT_DRAIN] = state_handle_maint_drain,
    [ST_DRINK_SET]   = state_handle_drink_set,
    [ST_SETTING]     = state_handle_setting,
    [ST_CALIBRATION] = state_handle_calibration,
    [ST_DETECTION]   = state_handle_detection,
    [ST_ALARM]       = state_handle_alarm,
    [ST_WIFI]        = state_handle_wifi,
    [ST_OTA]         = state_handle_ota,
    [ST_AUTO_TEST]   = state_handle_auto_test,
};

static void sp_pro_build_drink_view(const app_ctx_t *ctx, app_drink_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->elapsed_tick = ctx->drink.elapsed_tick;
    view->target_ml = ctx->drink.target_ml;
    view->target_drink = ctx->drink.target_drink;
    view->target_temp = ctx->drink.target_temp;
    view->display_liquid_ml = sp_pro_get_display_liquid_ml(ctx);
    view->remote_active = (ctx->ctrl.src == CTRL_SRC_MQTT) && (ctx->ctrl.formula != NULL);

    if (view->remote_active &&
        ctx->drink.target_drink == DRINK_AMERICANO &&
        ctx->ctrl.formula != NULL) {
        view->secondary_target_ml = ctx->ctrl.formula->water_weight;
        view->secondary_target_temp = (float)ctx->ctrl.formula->water_temperature;
    }

    if (view->remote_active && ctx->core.substate == BREW_SUB_REMOTE_COUNTDOWN) {
        uint32_t elapsed = ctx->timer.tick - ctx->drink.target_time;
        uint32_t remain = (elapsed >= STAY_TICKS_3S) ? 0U : (STAY_TICKS_3S - elapsed);
        view->countdown_seconds = (uint8_t)((remain + STAY_TICKS_1S - 1U) / STAY_TICKS_1S);
        if (view->countdown_seconds == 0U) {
            view->countdown_seconds = 1U;
        }
    }

    if (ctx->core.state == ST_STEAM) {
        if (ctx->core.substate == BREW_SUB_TAIL_SPRAY_COUNTDOWN) {
            uint32_t elapsed = ctx->timer.tick - ctx->drink.target_time;
            uint32_t remain = (elapsed >= STAY_TICKS_3S) ? 0U : (STAY_TICKS_3S - elapsed);
            view->countdown_seconds = (uint8_t)((remain + STAY_TICKS_1S - 1U) / STAY_TICKS_1S);
            if (view->countdown_seconds == 0U) {
                view->countdown_seconds = 1U;
            }
            view->tail_spray_pending = true;
        } else if (ctx->core.substate == BREW_SUB_WAIT_TAIL_SPRAY) {
            view->tail_spray_pending = true;
        } else if (ctx->core.substate == BREW_SUB_TAIL_SPRAY_RUNNING) {
            view->tail_spray_running = true;
        }
    }
}

static bool sp_pro_map_remote_formula(const formula_info_t *formula,
                                      app_state_t *state,
                                      drink_type_t *drink)
{
    if (!formula || !state || !drink) {
        return false;
    }

    switch (formula->drink_id) {
    case DRINK_ID_MASTER:
        *state = ST_MASTER;
        *drink = DRINK_MASTER;
        return true;

    case DRINK_ID_ESPRESSO:
        *state = ST_ESPRESSO;
        *drink = DRINK_ESPRESSO;
        return true;

    case DRINK_ID_AMERICAN:
        *state = ST_AMERICANO;
        *drink = DRINK_AMERICANO;
        return true;

    case DRINK_ID_COLDBREW:
        *state = ST_COLD_BREW;
        *drink = DRINK_COLD_BREW;
        return true;

    case DRINK_ID_WATER:
        *state = ST_WATER;
        *drink = DRINK_WATER;
        return true;

    default:
        return false;
    }
}

bool sp_pro_app_state_requires_brew_handle(app_state_t state)
{
    switch (state) {
    case ST_ESPRESSO:
    case ST_AMERICANO:
    case ST_COLD_BREW:
    case ST_MASTER:
        return true;

    default:
        return false;
    }
}

static bool sp_pro_app_drink_requires_brew_handle(drink_type_t drink)
{
    switch (drink) {
    case DRINK_ESPRESSO:
    case DRINK_AMERICANO:
    case DRINK_COLD_BREW:
    case DRINK_MASTER:
        return true;

    default:
        return false;
    }
}

bool sp_pro_app_formula_requires_brew_handle(const formula_info_t *formula)
{
    app_state_t state;
    drink_type_t drink;

    if (!formula) {
        return false;
    }

    if (!sp_pro_map_remote_formula(formula, &state, &drink)) {
        return false;
    }

    return sp_pro_app_drink_requires_brew_handle(drink) ||
           sp_pro_app_state_requires_brew_handle(state);
}

bool sp_pro_app_is_brew_handle_in_place(void)
{
    return g_ctx.ms.brew_handle_postion_flag == 1U;
}

static void sp_pro_apply_remote_formula_targets(app_ctx_t *ctx,
                                                const formula_info_t *formula,
                                                bool water_stage)
{
    if (!ctx || !formula) {
        return;
    }

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

static control_action_t sp_pro_remote_formula_to_stats_action(const formula_info_t *formula, app_state_t state)
{
    if (state == ST_WATER) {
        return CTRL_ACT_HOT_WATER;
    }

    if (!formula) {
        return CTRL_ACT_NONE;
    }

    switch (formula->drink_id) {
    case DRINK_ID_ESPRESSO:
        return CTRL_ACT_ESPRESSO;
    case DRINK_ID_AMERICAN:
        return CTRL_ACT_AMERICANO_BREW;
    case DRINK_ID_COLDBREW:
        return CTRL_ACT_COLD_BREW;
    case DRINK_ID_WATER:
        return CTRL_ACT_HOT_WATER;
    case DRINK_ID_MASTER:
    default:
        return CTRL_ACT_ESPRESSO;
    }
}

bool sp_pro_app_start_remote_drink(const formula_info_t *formula)
{
    app_state_t next_state;
    drink_type_t next_drink;

    if (!sp_pro_map_remote_formula(formula, &next_state, &next_drink)) {
        return false;
    }

    if (g_ctx.core.state != ST_READY) {
        return false;
    }

    if (sp_pro_app_formula_requires_brew_handle(formula) &&
        !sp_pro_app_is_brew_handle_in_place()) {
        return false;
    }

    memset(&g_ctx.ctrl.remote_formula, 0, sizeof(g_ctx.ctrl.remote_formula));
    g_ctx.ctrl.remote_formula = *formula;
    g_ctx.ctrl.formula = &g_ctx.ctrl.remote_formula;
    g_ctx.ctrl.src = CTRL_SRC_MQTT;
    g_ctx.ctrl.request_drink = next_drink;
    g_ctx.ctrl.busy = true;

    g_ctx.drink.target_drink = next_drink;
    sp_pro_apply_remote_formula_targets(&g_ctx, &g_ctx.ctrl.remote_formula, false);
    g_ctx.drink.start_tick = g_ctx.timer.tick;
    g_ctx.drink.finish_tick = 0U;
    g_ctx.drink.elapsed_tick = 0U;
    g_ctx.drink.target_time = g_ctx.timer.tick;
    g_ctx.drink.progress = 0U;

    g_ctx.state_runtime.drink.exit_reason = DRINK_EXIT_NONE;
    g_ctx.state_runtime.drink.remote_action_started = false;
    g_ctx.core.substate = BREW_SUB_REMOTE_COUNTDOWN;

    control_action_t stats_action = sp_pro_remote_formula_to_stats_action(formula, next_state);
    device_statistics_notify_remote_action_start(stats_action, formula);
    drink_record_notify_remote_action_start(stats_action, formula);

    g_ctx.core.state = next_state;
    return true;
}

static void sp_pro_build_clear_bean_view(const app_ctx_t *ctx, app_clear_bean_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->step = ctx->clear_bean.step;
}

static void sp_pro_build_setting_view(const app_ctx_t *ctx, app_setting_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->esp_brew_w = ctx->setting.esp_brew_w;
    view->esp_brew_t = ctx->setting.esp_brew_t;
    view->ame_brew_w = ctx->setting.ame_brew_w;
    view->ame_brew_t = ctx->setting.ame_brew_t;
    view->ame_water_w = ctx->setting.ame_water_w;
    view->ame_water_t = ctx->setting.ame_water_t;
    view->cold_brew_w = ctx->setting.cold_brew_w;
    view->hot_water_w = ctx->setting.hot_water_w;
    view->hot_water_t = ctx->setting.hot_water_t;
    view->grind_w = ctx->setting.grind_w;
    view->clean_v = ctx->setting.clean_v;
    view->steam_level = ctx->setting.steam_level;
    view->active = ctx->setting.active;
    view->drink_type = ctx->setting.drink_type;
    view->current_val = ctx->setting.current_val;
    view->target_val = ctx->setting.target_val;
    view->current_mode = ctx->setting.current_mode;
    view->ui_phase = ctx->setting.ui_phase;
    view->sub = ctx->setting.sub;
    view->water_in_mode = ctx->setting.water_in_mode;
    view->clear_step = ctx->setting.clear_step;
    view->reset_step = ctx->setting.reset_step;
    view->hardness = ctx->setting.hardness;
}

static void sp_pro_build_beverage_settings_from_ctx(const app_ctx_t *ctx, app_beverage_settings_t *settings)
{
    if (!ctx || !settings) {
        return;
    }

    memset(settings, 0, sizeof(*settings));
    settings->esp_brew_w = ctx->setting.esp_brew_w;
    settings->esp_brew_t = ctx->setting.esp_brew_t;
    settings->ame_brew_w = ctx->setting.ame_brew_w;
    settings->ame_brew_t = ctx->setting.ame_brew_t;
    settings->ame_water_w = ctx->setting.ame_water_w;
    settings->ame_water_t = ctx->setting.ame_water_t;
    settings->cold_brew_w = ctx->setting.cold_brew_w;
    settings->grind_w = ctx->setting.grind_w;
}

static void sp_pro_build_alarm_view(const app_ctx_t *ctx, app_alarm_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->active = ctx->alarm.active;
    view->is_notice = ctx->alarm.is_notice;
    view->major = ctx->alarm.major;
    view->sub = ctx->alarm.sub;
    view->notice_p = ctx->alarm.notice_p;
    view->warning = ctx->alarm.warning;
    view->retry_count = ctx->state_runtime.alarm.water_pump_retry_count;
}

static void sp_pro_build_calibration_view(const app_ctx_t *ctx, app_calibration_view_t *view)
{
    float current_coeff;

    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->mode = ctx->calibration.mode;
    view->step = ctx->calibration.step;
    view->flow_coeff_base = ctx->state_runtime.calibration.flow_coeff_base;
    view->flow_adjust_percent = ctx->state_runtime.calibration.flow_adjust_percent;
    current_coeff = view->flow_coeff_base *
                    (1.0f + ((float)view->flow_adjust_percent / 100.0f));
    view->flow_coeff_current = current_coeff;
}

static void sp_pro_build_detection_view(const app_ctx_t *ctx, app_detection_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->step = ctx->detection.step;
    view->pass_mask = ctx->detection.pass_mask;
    view->fail_mask = ctx->detection.fail_mask;
}

static void sp_pro_build_maint_view(const app_ctx_t *ctx, app_maint_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->clean_state = ctx->maint.clean_state;
    view->brew_step = ctx->state_runtime.maint.brew_step;
    view->des_step = ctx->state_runtime.maint.des_step;
    view->steam_step = ctx->state_runtime.maint.steam_step;
}

static void sp_pro_build_child_lock_view(const app_ctx_t *ctx, app_child_lock_view_t *view)
{
    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->enabled = ctx->child_lock.enabled;
    view->ui_hint = ctx->child_lock.ui_hint;
}

static void sp_pro_build_display_view(const app_ctx_t *ctx, app_display_view_t *view)
{
    bool ota_prompt_dismissed;
    bool provisioning_ui_active;

    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));

    view->tick = ctx->timer.tick;
    view->anim_step = ctx->anim.step;
    view->state = ctx->core.state;
    view->substate = ctx->core.substate;
    ota_prompt_dismissed = sp_pro_ota_prompt_dismissed();
    provisioning_ui_active = (ctx->core.state == ST_WIFI) ||
                             ((sys_pra.app_mode == APP_MODE_BT_PAIRING) &&
                              ble_app_is_active());
    view->network_connected = (sys_pra.wifi_state == WIFI_CONNECTED) &&
                              mqtt_is_ui_connected();
    view->network_connecting = !view->network_connected &&
                               provisioning_ui_active;
    view->ota_upgrade_prompt = (g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM) &&
                               (g_ota_info.ota_auto_up[0] == '0') &&
                               !ota_prompt_dismissed;
    view->ota_upgrading = (g_ota_info.ota_auto_up[0] == '9') &&
                          (g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM ||
                           g_ota_info.ota_sta == IOT_SIMPLE_OTA_YMODEM);
    view->ota_prompt_dismissed = ota_prompt_dismissed &&
                                 (g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM) &&
                                 (g_ota_info.ota_auto_up[0] == '0');
    view->ms = ctx->ms;

    sp_pro_build_drink_view(ctx, &view->drink);
    sp_pro_build_clear_bean_view(ctx, &view->clear_bean);
    sp_pro_build_setting_view(ctx, &view->setting);
    sp_pro_build_calibration_view(ctx, &view->calibration);
    sp_pro_build_detection_view(ctx, &view->detection);
    sp_pro_build_maint_view(ctx, &view->maint);
    sp_pro_build_alarm_view(ctx, &view->alarm);
    sp_pro_build_child_lock_view(ctx, &view->child_lock);
}

static void sp_pro_build_command_view_from_ctx(const app_ctx_t *ctx, app_command_view_t *view)
{
    app_beverage_settings_t beverage_settings = {0};

    if (!ctx || !view) {
        return;
    }

    memset(view, 0, sizeof(*view));
    view->src = ctx->ctrl.src;
    sp_pro_build_beverage_settings_from_ctx(ctx, &beverage_settings);
    view->esp_brew_w = beverage_settings.esp_brew_w;
    view->esp_brew_t = beverage_settings.esp_brew_t;
    view->ame_brew_w = beverage_settings.ame_brew_w;
    view->ame_brew_t = beverage_settings.ame_brew_t;
    view->ame_water_w = beverage_settings.ame_water_w;
    view->ame_water_t = beverage_settings.ame_water_t;
    view->cold_brew_w = beverage_settings.cold_brew_w;
    view->grind_w = beverage_settings.grind_w;
    view->hot_water_w = ctx->setting.hot_water_w;
    view->hot_water_t = ctx->setting.hot_water_t;
    view->clean_v = ctx->setting.clean_v;
    view->steam_level = ctx->setting.steam_level;
}

static void sp_pro_sync_machine_status(app_ctx_t *ctx, const MACHINE_STATUS *status)
{
    if (!ctx || !status) {
        return;
    }

    ctx->ms = *status;
    ctx->error_code = status->error_code;
}

static void sp_pro_render_state_page(const app_display_view_t *view, disp_element_t *disp)
{
    if (!view || !disp) {
        return;
    }

    switch (view->state) {
    case ST_OFF:
        disp_clear(disp);
        break;

    case ST_ON:
        disp_build_power_on_page(view, disp);
        break;

    case ST_READY:
    case ST_WIFI:
        disp_build_ready_page(view, disp);
        break;

    case ST_OTA:
        disp_build_ota_page(view, disp);
        break;

    case ST_STANDBY:
        disp_clear(disp);
        break;

    case ST_ESPRESSO:
    case ST_MASTER:
        if (view->substate == BREW_SUB_PREPARE || view->substate == BREW_SUB_REMOTE_COUNTDOWN) {
            disp_build_preheat_page(view, disp);
        } else {
            disp_build_espresso_brew_page(view, disp, BF_KEY_K1);
        }
        break;

    case ST_AMERICANO:
        if (view->substate == BREW_SUB_PREPARE || view->substate == BREW_SUB_REMOTE_COUNTDOWN) {
            disp_build_preheat_page(view, disp);
        } else if (view->substate == BREW_SUB_RUNNING_1) {
            disp_build_espresso_brew_page(view, disp, BF_KEY_K2);
        } else {
            disp_build_water_page(view, disp, BF_KEY_K2);
        }
        break;

    case ST_COLD_BREW:
        if (view->substate == BREW_SUB_PREPARE || view->substate == BREW_SUB_REMOTE_COUNTDOWN) {
            disp_build_cold_brew_prepare_page(view, disp);
        } else {
            disp_build_cold_brew_page(view, disp);
        }
        break;

    case ST_WATER:
        if (view->substate == BREW_SUB_PREPARE || view->substate == BREW_SUB_REMOTE_COUNTDOWN) {
            disp_build_water_prepare_page(view, disp);
        } else {
            disp_build_water_page(view, disp, BF_KEY_K4);
        }
        break;

    case ST_GRIND:
        if (view->substate == BREW_SUB_PREPARE) {
            disp_build_grind_prepare_page(view, disp);
        } else {
            disp_build_grind_page(view, disp);
        }
        break;

    case ST_CLEAR_BEAN:
        disp_build_clear_bean_page(view, disp);
        break;

    case ST_LOCK:
        disp_build_lock_page(view, disp);
        break;

    case ST_STEAM:
        disp_build_steam_page(view, disp);
        break;

    case ST_CLEAN_BREW:
    case ST_MAINT_BREW:
    case ST_MAINT_DES:
    case ST_MAINT_STEAM:
    case ST_MAINT_DRAIN:
        disp_build_clean_page(view, disp);
        break;

    case ST_DRINK_SET:
        disp_build_drink_set_page(view, disp);
        break;

    case ST_SETTING:
        switch (view->setting.sub) {
        case SET_SUB_WATER_IN:
            disp_build_water_in_page(view, disp);
            break;
        case SET_SUB_WATER_HARDNESS:
            disp_build_hardness_page(view, disp);
            break;
        case SET_SUB_FACTORY_RESET:
            disp_build_factory_reset_page(view, disp);
            break;
        case SET_SUB_CLEAR_WATERWAY:
        default:
            disp_build_ready_page(view, disp);
            break;
        }
        break;

    case ST_CALIBRATION:
        disp_build_calibration_page(view, disp);
        break;

    case ST_DETECTION:
        disp_build_detection_page(view, disp);
        break;

    case ST_ALARM:
        if (view->alarm.active && !view->alarm.is_notice) {
            disp_build_fault_page(view, disp);
        } else {
            disp_build_ready_page(view, disp);
        }
        break;

    default:
        disp_build_ready_page(view, disp);
        break;
    }
}

static void sp_pro_apply_alarm_overlay(const app_display_view_t *view, disp_element_t *disp)
{
    if (!view || !disp) {
        return;
    }

    if (view->alarm.active && view->alarm.is_notice) {
        disp_build_alarm_page(view, disp);
    }
}

static void sp_pro_commit_display_frame(disp_model_t *model,
                                        const disp_element_t *prev_disp,
                                        disp_element_t *next_disp)
{
    if (!model || !prev_disp || !next_disp) {
        return;
    }

    if (memcmp(prev_disp, next_disp, sizeof(*next_disp)) != 0) {
        next_disp->frame_id = (uint8_t)(prev_disp->frame_id + 1U);
        model->disp = *next_disp;
        model->dirty = true;
    }
}

static bool sp_pro_defer_fault_preemption(app_state_t state)
{
    switch (state) {
    case ST_ESPRESSO:
    case ST_MASTER:
    case ST_AMERICANO:
    case ST_COLD_BREW:
    case ST_WATER:
    case ST_STEAM:
    case ST_GRIND:
    case ST_CLEAR_BEAN:
    case ST_CLEAN_BREW:
    case ST_MAINT_BREW:
    case ST_MAINT_DES:
    case ST_MAINT_STEAM:
    case ST_MAINT_DRAIN:
    case ST_CALIBRATION:
    case ST_DETECTION:
    case ST_AUTO_TEST:
        return true;

    default:
        return false;
    }
}

void sp_pro_build_command_view(app_command_view_t *view)
{
    sp_pro_app_get_command_view(view);
}

void sp_pro_handle_key_event(const bf7613_key_event_t *event)
{
    uint16_t pressed_edges;

#if SUPPLIER_HMI_DEMO_MODE
    if (event) {
        pressed_edges = event->key_mask & (uint16_t)(~s_supplier_hmi_demo.key_down_mask);
        s_supplier_hmi_demo.key_down_mask = event->key_mask;
        if (pressed_edges != 0U) {
            if (g_ctx.core.state != ST_OFF) {
                sp_pro_refresh_idle_deadlines();
            }
            for (uint8_t key_idx = 0U; key_idx < BF_KEY_COUNT; key_idx++) {
                if ((pressed_edges & (uint16_t)(1U << key_idx)) != 0U) {
                    supplier_hmi_demo_apply_key_selection(key_idx);
                    break;
                }
            }
        }
    }
    return;
#endif

    if (event) {
        pressed_edges = event->key_mask & (uint16_t)(~g_ctx.input.key_down);
        if (pressed_edges != 0U && g_ctx.core.state != ST_OFF) {
            sp_pro_refresh_idle_deadlines();
        }
    }

    key_event_handle(&g_ctx, event);
}

disp_model_t *sp_pro_get_disp_model(void)
{
    return &g_disp_model;
}

void sp_pro_update_machine_status(const MACHINE_STATUS *status)
{
    sp_pro_sync_machine_status(&g_ctx, status);
    sp_pro_check_device_sensor_changed(status);
    device_statistics_handle_machine_status(status);
    drink_record_handle_machine_status(status);
}

/* Compatibility wrapper kept for existing callers. */
void handle_machine_status(const MACHINE_STATUS *status)
{
    sp_pro_update_machine_status(status);
}

/* ================= Display model builder ================= */

void sp_pro_disp_model(disp_model_t *model)
{
    disp_element_t disp = {0};
    disp_element_t prev_disp;
    app_display_view_t view;
    bool force_refresh = false;

    if (!model) {
        return;
    }

#if SUPPLIER_HMI_DEMO_MODE
    prev_disp = model->disp;
    supplier_hmi_demo_render(&disp);
    disp.frame_id = prev_disp.frame_id;
    sp_pro_commit_display_frame(model, &prev_disp, &disp);
    led_service_set_state(LED_EFFECT_OFF);
    return;
#endif

    sp_pro_build_display_view(&g_ctx, &view);
    prev_disp = model->disp;

    sp_pro_render_state_page(&view, &disp);
    sp_pro_apply_alarm_overlay(&view, &disp);

    /* Builders usually clear the whole display object, so restore the
     * previous frame id after rendering and before change detection. */
    disp.frame_id = prev_disp.frame_id;
    sp_pro_commit_display_frame(model, &prev_disp, &disp);

    if ((g_ctx.core.state == ST_OFF || g_ctx.core.state == ST_STANDBY) &&
        s_display_clear_reinforce_frames > 0U) {
        force_refresh = true;
        s_display_clear_reinforce_frames--;
    }

    if (force_refresh && !model->dirty) {
        disp.frame_id = (uint8_t)(prev_disp.frame_id + 1U);
        model->disp = disp;
        model->dirty = true;
    }
}

static inline void sp_pro_tick_update(void)
{
    g_ctx.timer.tick++;
}

static inline void sp_pro_key_update(void)
{
#if SUPPLIER_HMI_DEMO_MODE
    sp_pro_power_key_tick();
    return;
#endif

    key_scan_ticks(&g_ctx);
    key_combo_tick(&g_ctx);
    sp_pro_power_key_tick();
}

static inline void sp_pro_alarm_check(void)
{
    if (g_ctx.core.state == ST_OFF) {
        sp_pro_clear_local_alarm_state(&g_ctx);
        return;
    }

#if APP_TEST_DISABLE_LOCAL_ALARM
    uint32_t saved_error_code = g_ctx.ms.error_code;

    /* Test builds keep lightweight notices, but fault alarms must be fully muted. */
    g_ctx.ms.error_code = 0U;
    alarm_check(&g_ctx);
    g_ctx.ms.error_code = saved_error_code;
#else
    alarm_check(&g_ctx);
#endif
}

static void sp_pro_state_dispatch(void)
{
    app_state_t cur = g_ctx.core.state;

    if (cur >= (int)(sizeof(state_handlers) / sizeof(state_handlers[0]))) {
        return;
    }

    state_handler_t handler = state_handlers[cur];
    app_state_t next;

    if (!handler) {
        ESP_LOGW(TAG, "No state handler for state %d", cur);
        return;
    }

    next = sp_pro_apply_power_timeout(cur);
    if (next != cur) {
        ESP_LOGI(TAG, "[APP][STATE] %d -> %d", cur, next);
        g_ctx.core.prev_state = cur;
        g_ctx.core.state = next;
        g_ctx.core.substate = 0;
        g_ctx.input.key_pressed = 0;
        g_ctx.input.key_long = 0;
        g_ctx.drink.marquee_tick = 0;
        g_ctx.drink.marquee_step = 0;
        sp_pro_apply_state_side_effects(cur, next);
        return;
    }

    next = sp_pro_apply_power_key(cur);
    if (next != cur) {
        ESP_LOGI(TAG, "[APP][STATE] %d -> %d", cur, next);
        g_ctx.core.prev_state = cur;
        g_ctx.core.state = next;
        g_ctx.core.substate = 0;
        g_ctx.input.key_pressed = 0;
        g_ctx.input.key_long = 0;
        g_ctx.drink.marquee_tick = 0;
        g_ctx.drink.marquee_step = 0;
        sp_pro_apply_state_side_effects(cur, next);
        return;
    }

#if !APP_TEST_DISABLE_LOCAL_ALARM
    if (g_ctx.alarm.active &&
        !g_ctx.alarm.is_notice &&
        cur != ST_ALARM &&
        cur != ST_OFF &&
        !sp_pro_defer_fault_preemption(cur)) {
        ESP_LOGW(TAG,
                 "Enter ALARM from %d, h=%u-%u err=0x%08lX",
                 cur,
                 g_ctx.alarm.major,
                 g_ctx.alarm.sub,
                 (unsigned long)g_ctx.ms.error_code);
        g_ctx.core.prev_state = cur;
        g_ctx.core.state = ST_ALARM;
        g_ctx.core.substate = 0;
        g_ctx.input.key_pressed = 0;
        g_ctx.input.key_long = 0;
        return;
    }
#endif

    next = sp_pro_apply_ota_ui(cur);
    if (next != cur) {
        ESP_LOGI(TAG, "[APP][STATE] %d -> %d", cur, next);
        g_ctx.core.prev_state = cur;
        g_ctx.core.state = next;
        if (next != ST_OTA) {
            g_ctx.core.substate = 0;
        }
        g_ctx.input.key_pressed = 0;
        g_ctx.input.key_long = 0;
        g_ctx.drink.marquee_tick = 0;
        g_ctx.drink.marquee_step = 0;
        sp_pro_apply_state_side_effects(cur, next);
        return;
    }

    next = handler(&g_ctx);
    if (next == cur) {
        return;
    }

    ESP_LOGI(TAG, "[APP][STATE] %d -> %d", cur, next);

    g_ctx.core.prev_state = cur;
    g_ctx.core.state = next;
    g_ctx.core.substate = 0;

    g_ctx.input.key_pressed = 0;
    g_ctx.input.key_long = 0;

    g_ctx.drink.marquee_tick = 0;
    g_ctx.drink.marquee_step = 0;
    sp_pro_apply_state_side_effects(cur, next);
}

void sp_pro_ui_anim_tick(void)
{
    if (!g_ctx.anim.active) {
        return;
    }

    g_ctx.anim.tick++;
    if (g_ctx.anim.tick >= g_ctx.anim.interval) {
        g_ctx.anim.tick = 0;
        g_ctx.anim.step++;
    }
}

void sp_pro_state_machine(void)
{
#if SUPPLIER_HMI_DEMO_MODE
    sp_pro_tick_update();
    led_service_set_state(LED_EFFECT_OFF);
    led_service_tick();
    return;
#endif

    sp_pro_tick_update();
    sp_pro_sync_idle_timeout_config();
    sp_pro_key_update();
    sp_pro_capture_local_activity();
    sp_pro_alarm_check();
    sp_pro_state_dispatch();
    sp_pro_ui_anim_tick();
    sp_pro_sync_led_effect();
    led_service_tick();
}

void sp_pro_app_init(void)
{
    led_service_init();
    ctr_scheduler_init();
    sp_pro_power_key_init();
    memset(&g_ctx, 0, sizeof(g_ctx));
    drink_record_service_init();

    /* Boot through ST_ON and wait until the controller reports ctr_status=1. */
    g_ctx.core.state = ST_ON;
    sp_pro_prepare_power_on_entry();

    sp_pro_apply_default_settings(&g_ctx);
    sp_pro_load_settings_from_nvs(&g_ctx);
    sp_pro_sync_beverage_settings_from_formula_store(&g_ctx);
    s_last_auto_power_off_time = -1;
    s_last_auto_stand_by_time = -1;
    s_last_encoder_activity_seq = 0U;
    sp_pro_refresh_idle_deadlines();

    if (xTaskCreate(sp_pro_logic_task, "sp_pro_logic_task", SP_PRO_LOGIC_TASK_STACK_SIZE, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sp_pro_logic_task");
        return;
    }
    ESP_LOGI(TAG, "HMI App initialized");
}

static void sp_pro_logic_task(void *pv_parameters)
{
    (void)pv_parameters;
    memset(&g_disp_model, 0, sizeof(g_disp_model));

    while (1) {
        sp_pro_state_machine();
        sp_pro_disp_model(&g_disp_model);
        ctr_scheduler_poll();
        mqtt_poll();
        BlePairing_Poll();
        vTaskDelay(pdMS_TO_TICKS(LOGIC_TASK_MS));
    }
}
static const setting_info_t *sp_pro_runtime_setting(void)
{
    return mqtt_get_runtime_setting();
}

static uint32_t sp_pro_timeout_ticks_from_seconds(int seconds)
{
    uint64_t ticks;

    if (seconds <= 0) {
        return 0U;
    }

    ticks = (((uint64_t)seconds * 1000ULL) + (LOGIC_TASK_MS - 1U)) / LOGIC_TASK_MS;
    if (ticks > 0xFFFFFFFFULL) {
        return 0xFFFFFFFFU;
    }

    return (uint32_t)ticks;
}

static bool sp_pro_deadline_expired(uint32_t deadline)
{
    return deadline != 0U && (int32_t)(g_ctx.timer.tick - deadline) >= 0;
}

static void sp_pro_clear_idle_deadlines(void)
{
    g_ctx.timer.operation_timer = 0U;
    g_ctx.timer.standby_timer = 0U;
}

static void sp_pro_arm_standby_poweroff_deadline(void)
{
    const setting_info_t *setting = sp_pro_runtime_setting();
    uint32_t off_ticks = sp_pro_timeout_ticks_from_seconds(setting ? setting->auto_power_off_time : 0);

    g_ctx.timer.operation_timer = off_ticks > 0U ? (g_ctx.timer.tick + off_ticks) : 0U;
}

static void sp_pro_refresh_idle_deadlines(void)
{
    const setting_info_t *setting = sp_pro_runtime_setting();
    uint32_t standby_ticks = sp_pro_timeout_ticks_from_seconds(setting ? setting->auto_stand_by_time : 0);
    uint32_t off_ticks = sp_pro_timeout_ticks_from_seconds(setting ? setting->auto_power_off_time : 0);

    g_ctx.timer.standby_timer = standby_ticks > 0U ? (g_ctx.timer.tick + standby_ticks) : 0U;
    g_ctx.timer.operation_timer = (standby_ticks == 0U && off_ticks > 0U) ?
                                  (g_ctx.timer.tick + off_ticks) : 0U;
}

static void sp_pro_sync_idle_timeout_config(void)
{
    const setting_info_t *setting = sp_pro_runtime_setting();
    int auto_power_off_time = setting ? setting->auto_power_off_time : 0;
    int auto_stand_by_time = setting ? setting->auto_stand_by_time : 0;

    if (auto_power_off_time == s_last_auto_power_off_time &&
        auto_stand_by_time == s_last_auto_stand_by_time) {
        return;
    }

    s_last_auto_power_off_time = auto_power_off_time;
    s_last_auto_stand_by_time = auto_stand_by_time;

    if (g_ctx.core.state == ST_OFF) {
        sp_pro_clear_idle_deadlines();
    } else if (g_ctx.core.state == ST_STANDBY) {
        sp_pro_arm_standby_poweroff_deadline();
    } else {
        sp_pro_refresh_idle_deadlines();
    }
}

static void sp_pro_capture_local_activity(void)
{
    if (g_ctx.core.state == ST_OFF) {
        return;
    }

    if (g_ctx.ms.encoder.evt_seq != s_last_encoder_activity_seq) {
        s_last_encoder_activity_seq = g_ctx.ms.encoder.evt_seq;
        if (g_ctx.ms.encoder.evt_type == ENC_EVT_CLICK) {
            voice_manager_play_touch_tone();
        }
        sp_pro_refresh_idle_deadlines();
    }
}

static app_state_t sp_pro_apply_power_timeout(app_state_t cur)
{
    if (cur == ST_READY) {
        if (sp_pro_deadline_expired(g_ctx.timer.standby_timer)) {
            ESP_LOGI(TAG, "Auto standby timeout");
            g_ctx.timer.standby_timer = 0U;
            sp_pro_arm_standby_poweroff_deadline();
            return ST_STANDBY;
        }

        if (sp_pro_deadline_expired(g_ctx.timer.operation_timer)) {
            ESP_LOGI(TAG, "Auto power-off timeout");
            sp_pro_clear_idle_deadlines();
            voice_manager_play_touch_tone();
            return ST_OFF;
        }
    }

    if (cur == ST_STANDBY && sp_pro_deadline_expired(g_ctx.timer.operation_timer)) {
        ESP_LOGI(TAG, "Auto power-off timeout in standby");
        sp_pro_clear_idle_deadlines();
        voice_manager_play_touch_tone();
        return ST_OFF;
    }

    return cur;
}
