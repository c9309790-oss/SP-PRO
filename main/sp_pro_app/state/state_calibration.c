#include "sp_pro_app.h"
#include "esp_log.h"
#include "mqtt.h"
#include "mqtt_protocol.h"
#include "uart_ctr.h"
#include <stddef.h>

static const char *TAG = "state_cal";

#define DETECTION_CONFIRM_TICKS      STAY_TICKS_2S
#define DETECTION_CLEAR_BEAN_TICKS   STAY_TICKS(5000)
#define POWDER_CAL_ADC_DELTA_MIN     100
#define POWDER_CAL_WEIGHT_DELTA_MIN  0.5f
#define POWDER_CAL_WEIGHT_GRAMS      500.0f
#define POWDER_CAL_ADC_SCALE         256.0f
#define FLOW_CAL_PERCENT_MAX         20
#define FLOW_CAL_PERCENT_MIN        -20
#define FLOW_CAL_BASE_HOT_WATER      0.45f
/* TODO: confirm hot drink default flow coefficient with PRD/CTR side. */
#define FLOW_CAL_BASE_HOT_DRINK      0.30f

extern FLASH_FACTORY_DATA factory_data;

static bool detection_grind_handle_present(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.grind_handle_postion_flag == 1U);
}

static bool detection_brew_handle_present(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.brew_handle_postion_flag == 1U);
}

static bool detection_water_tank_missing(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.water_box_shortage_flag != 0U);
}

static bool detection_bean_hopper_installed(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.beanbox_in_place == 1U);
}

static bool detection_clear_bean_unlocked(const app_ctx_t *ctx)
{
    return ctx && (ctx->ms.beanbox_in_place == 2U);
}

static bool detection_arm_on_inactive(app_ctx_t *ctx, bool condition_met)
{
    if (!ctx) {
        return false;
    }

    if (!ctx->state_runtime.detection.transition_armed) {
        if (!condition_met) {
            ctx->state_runtime.detection.transition_armed = true;
        } else {
            ctx->state_runtime.detection.sensor_latched = false;
            ctx->detection.step_tick = ctx->timer.tick;
            return false;
        }
    }

    return true;
}

static detection_result_flag_t detection_result_flag_for_step(detection_step_t step)
{
    switch (step) {
    case DET_STEP_PLACE_PORTAFILTER:
    case DET_STEP_REMOVE_PORTAFILTER:
        return DET_RESULT_GRIND_HANDLE;

    case DET_STEP_REMOVE_WATER_TANK:
    case DET_STEP_PUTBACK_WATER_TANK:
        return DET_RESULT_WATER_TANK;

    case DET_STEP_REMOVE_BEAN_HOPPER:
    case DET_STEP_PUTBACK_BEAN_HOPPER:
        return DET_RESULT_BEAN_HOPPER;

    case DET_STEP_UNLOCK_HOPPER:
    case DET_STEP_CLEAR_BEAN_RUNNING:
    case DET_STEP_REMOVE_BEAN_HOPPER_AFTER_CLEAR:
        return DET_RESULT_CLEAR_BEAN;

    case DET_STEP_HANDLE_FIT:
    case DET_STEP_REMOVE_PORTAFILTER_FINAL:
        return DET_RESULT_BREW_HANDLE;

    default:
        return 0U;
    }
}

static void detection_mark_pass(app_ctx_t *ctx, detection_result_flag_t flag)
{
    if (!ctx || flag == 0U) {
        return;
    }

    ctx->detection.pass_mask |= (uint8_t)flag;
    ctx->detection.fail_mask &= (uint8_t)(~flag);
}

static void detection_mark_fail(app_ctx_t *ctx, detection_result_flag_t flag)
{
    if (!ctx || flag == 0U) {
        return;
    }

    ctx->detection.fail_mask |= (uint8_t)flag;
    ctx->detection.pass_mask &= (uint8_t)(~flag);
}

static void detection_stop_grinder(app_ctx_t *ctx)
{
    if (!ctx || !ctx->state_runtime.detection.grinder_started) {
        return;
    }

    ctx->ctrl.src = CTRL_SRC_UI;
    ctr_cmd_action(CTRL_ACT_GRIND_STOP, NULL);
    ctx->state_runtime.detection.grinder_started = false;
}

static void calibration_reset_runtime(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.calibration.entered = false;
    ctx->state_runtime.calibration.last_step = CAL_STEP_NONE;
    ctx->state_runtime.calibration.result_voice_played = false;
    ctx->state_runtime.calibration.powder_adc_baseline = 0;
    ctx->state_runtime.calibration.powder_adc_weight_point = 0;
    ctx->state_runtime.calibration.powder_weight_baseline = 0.0f;
    ctx->state_runtime.calibration.powder_signal_seen = false;
    ctx->state_runtime.calibration.flow_coeff_base = 0.0f;
    ctx->state_runtime.calibration.flow_adjust_percent = 0;
    ctx->calibration.mode = CAL_MODE_NONE;
    ctx->calibration.step = CAL_STEP_NONE;
    ctx->calibration.step_tick = 0U;
}

static void detection_reset_runtime(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    detection_stop_grinder(ctx);
    ctx->state_runtime.detection.entered = false;
    ctx->state_runtime.detection.last_step = DET_STEP_NONE;
    ctx->state_runtime.detection.result_voice_played = false;
    ctx->state_runtime.detection.sensor_latched = false;
    ctx->state_runtime.detection.transition_armed = false;
    ctx->state_runtime.detection.grinder_started = false;
    ctx->detection.step = DET_STEP_NONE;
    ctx->detection.step_tick = 0U;
    ctx->detection.pass_mask = 0U;
    ctx->detection.fail_mask = 0U;
}

static void calibration_set_step(app_ctx_t *ctx, calibration_step_t step)
{
    if (!ctx) {
        return;
    }

    ctx->calibration.step = step;
    ctx->calibration.step_tick = ctx->timer.tick;
}

static void detection_set_step(app_ctx_t *ctx, detection_step_t step)
{
    if (!ctx) {
        return;
    }

    ctx->detection.step = step;
    ctx->detection.step_tick = ctx->timer.tick;
    ctx->state_runtime.detection.sensor_latched = false;
    ctx->state_runtime.detection.transition_armed = false;
    ESP_LOGI(TAG, "detection step -> %d", step);
}

static bool calibration_try_play_result_voice(app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    switch (ctx->calibration.step) {
    case CAL_STEP_DONE:
        if (ctx->calibration.mode == CAL_MODE_POWDER) {
            return voice_manager_play_interrupt(VOICE_CALIBRATIONCOMPLETED);
        }
        if (ctx->calibration.mode == CAL_MODE_HOT_DRINK) {
            return voice_manager_play_interrupt(VOICE_HOTDRINKCALIBRATION);
        }
        if (ctx->calibration.mode == CAL_MODE_HOT_WATER) {
            return voice_manager_play_interrupt(VOICE_HOTWATERCALIBRATION);
        }
        break;

    case CAL_STEP_FAIL:
        return voice_manager_play_interrupt(VOICE_DETECTIONABNORMAL);

    default:
        break;
    }

    return false;
}

static void calibration_play_step_voice(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->state_runtime.calibration.last_step == ctx->calibration.step) {
        return;
    }

    ctx->state_runtime.calibration.last_step = ctx->calibration.step;

    switch (ctx->calibration.step) {
    case CAL_STEP_POWDER_PLACE_CUP:
        voice_manager_play_interrupt(VOICE_PLACEGROUNDCUP);
        break;

    case CAL_STEP_POWDER_WAIT_START:
        voice_manager_play_interrupt(VOICE_PLACECALIBRATIONWEIGHT);
        break;

    case CAL_STEP_DONE:
        ctx->state_runtime.calibration.result_voice_played = calibration_try_play_result_voice(ctx);
        break;

    case CAL_STEP_FAIL:
        ctx->state_runtime.calibration.result_voice_played = calibration_try_play_result_voice(ctx);
        break;

    default:
        break;
    }
}

static bool powder_calibration_signal_seen(app_ctx_t *ctx)
{
    int32_t adc_delta;

    if (!ctx) {
        return false;
    }

    adc_delta = ctx->ms.powder_adc - ctx->state_runtime.calibration.powder_adc_baseline;
    if (adc_delta < 0) {
        adc_delta = -adc_delta;
    }

    return adc_delta >= POWDER_CAL_ADC_DELTA_MIN;
}

static bool powder_calibration_apply_result(app_ctx_t *ctx)
{
    float powder_b;
    float powder_k;
    int32_t adc_delta;

    if (!ctx) {
        return false;
    }

    adc_delta = ctx->state_runtime.calibration.powder_adc_weight_point -
                ctx->state_runtime.calibration.powder_adc_baseline;
    if (adc_delta == 0) {
        ESP_LOGW(TAG,
                 "powder calibration invalid delta baseline=%ld adc500=%ld",
                 (long)ctx->state_runtime.calibration.powder_adc_baseline,
                 (long)ctx->state_runtime.calibration.powder_adc_weight_point);
        return false;
    }

    powder_b = (float)ctx->state_runtime.calibration.powder_adc_baseline;
    powder_k = ((float)ctx->state_runtime.calibration.powder_adc_weight_point - powder_b) /
               (POWDER_CAL_WEIGHT_GRAMS * POWDER_CAL_ADC_SCALE);

    factory_data.powder_b_value = powder_b;
    factory_data.powder_k_value = powder_k;

    ESP_LOGI(TAG,
             "powder calibration result adc0=%ld adc500=%ld powder_b=%.8f powder_k=%.8f",
             (long)ctx->state_runtime.calibration.powder_adc_baseline,
             (long)ctx->state_runtime.calibration.powder_adc_weight_point,
             factory_data.powder_b_value,
             factory_data.powder_k_value);

    ctr_factory_data_persist();

    if (!ctr_cmd_action(CTRL_ACT_FACTORY_WRITE, NULL)) {
        ESP_LOGE(TAG, "powder calibration failed to write factory data to CTR");
        return false;
    }

    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_CALIBRATION,
                                                "powder_calibration_done",
                                                0U);
    return true;
}

static float flow_calibration_current_coeff(const app_ctx_t *ctx)
{
    if (!ctx) {
        return 0.0f;
    }

    switch (ctx->calibration.mode) {
    case CAL_MODE_HOT_DRINK:
        if (factory_data.liquid_k_value > 0.0f) {
            return factory_data.liquid_k_value;
        }
        return FLOW_CAL_BASE_HOT_DRINK;

    case CAL_MODE_HOT_WATER:
        if (factory_data.flowmeter_coff > 0.0f) {
            return factory_data.flowmeter_coff;
        }
        return FLOW_CAL_BASE_HOT_WATER;

    case CAL_MODE_NONE:
    case CAL_MODE_POWDER:
    default:
        return 0.0f;
    }
}

static void flow_calibration_set_coeff(const app_ctx_t *ctx, float coeff)
{
    if (!ctx) {
        return;
    }

    switch (ctx->calibration.mode) {
    case CAL_MODE_HOT_DRINK:
        factory_data.liquid_k_value = coeff;
        break;

    case CAL_MODE_HOT_WATER:
        factory_data.flowmeter_coff = coeff;
        break;

    case CAL_MODE_NONE:
    case CAL_MODE_POWDER:
    default:
        break;
    }
}

static bool flow_calibration_commit(app_ctx_t *ctx)
{
    float coeff;

    if (!ctx) {
        return false;
    }

    coeff = ctx->state_runtime.calibration.flow_coeff_base *
            (1.0f + ((float)ctx->state_runtime.calibration.flow_adjust_percent / 100.0f));
    flow_calibration_set_coeff(ctx, coeff);

    ESP_LOGI(TAG,
             "flow calibration commit mode=%d base=%.8f adjust=%d%% result=%.8f",
             ctx->calibration.mode,
             (double)ctx->state_runtime.calibration.flow_coeff_base,
             (int)ctx->state_runtime.calibration.flow_adjust_percent,
             (double)coeff);

    if (!ctr_cmd_action(CTRL_ACT_FACTORY_WRITE, NULL)) {
        ESP_LOGE(TAG, "flow calibration failed to write factory data to CTR");
        return false;
    }

    ctr_factory_data_persist();

    mqtt_schedule_device_status_sections_report(MQTT_DEVICE_STATUS_SECTION_CALIBRATION,
                                                "flow_calibration_done",
                                                0U);
    return true;
}

static void flow_calibration_enter_mode(app_ctx_t *ctx, calibration_mode_t mode)
{
    if (!ctx) {
        return;
    }

    ctx->calibration.mode = mode;
    ctx->state_runtime.calibration.flow_coeff_base = flow_calibration_current_coeff(ctx);
    ctx->state_runtime.calibration.flow_adjust_percent = 0U;
    ESP_LOGI(TAG,
             "enter flow calibration mode=%d coeff_base=%.8f",
             mode,
             (double)ctx->state_runtime.calibration.flow_coeff_base);
    calibration_set_step(ctx, CAL_STEP_RUNNING);
}

static bool detection_try_play_result_voice(app_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }

    switch (ctx->detection.step) {
    case DET_STEP_PASS:
        return voice_manager_play_interrupt(VOICE_DETECTIONPASSED);

    case DET_STEP_FAIL:
        return voice_manager_play_interrupt(VOICE_DETECTIONABNORMAL);

    default:
        break;
    }

    return false;
}

static void detection_play_step_voice(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->state_runtime.detection.last_step == ctx->detection.step) {
        return;
    }

    ctx->state_runtime.detection.last_step = ctx->detection.step;

    switch (ctx->detection.step) {
    case DET_STEP_PLACE_PORTAFILTER:
        voice_manager_play_interrupt(VOICE_PLACEPORTAFILTERONSTAND);
        break;
    case DET_STEP_REMOVE_PORTAFILTER:
        voice_manager_play_interrupt(VOICE_REMOVEPORTAFILTER);
        break;
    case DET_STEP_REMOVE_WATER_TANK:
        voice_manager_play_interrupt(VOICE_REMOVEWATERTANK);
        break;
    case DET_STEP_PUTBACK_WATER_TANK:
        voice_manager_play_interrupt(VOICE_PUTBACKWATERTANK);
        break;
    case DET_STEP_REMOVE_BEAN_HOPPER:
        voice_manager_play_interrupt(VOICE_REMOVEBEANHOPPER);
        break;
    case DET_STEP_PUTBACK_BEAN_HOPPER:
        voice_manager_play_interrupt(VOICE_PUTBACKBEANHOPPER);
        break;
    case DET_STEP_UNLOCK_HOPPER:
        voice_manager_play_interrupt(VOICE_UNLOCKHOPPER);
        break;
    case DET_STEP_REMOVE_BEAN_HOPPER_AFTER_CLEAR:
        voice_manager_play_interrupt(VOICE_REMOVEBEANHOPPER);
        break;
    case DET_STEP_HANDLE_FIT:
        voice_manager_play_interrupt(VOICE_PROMPTHANDLEFITTING);
        break;
    case DET_STEP_REMOVE_PORTAFILTER_FINAL:
        voice_manager_play_interrupt(VOICE_REMOVEPORTAFILTER);
        break;
    case DET_STEP_PASS:
        ctx->state_runtime.detection.result_voice_played = detection_try_play_result_voice(ctx);
        break;
    case DET_STEP_FAIL:
        ctx->state_runtime.detection.result_voice_played = detection_try_play_result_voice(ctx);
        break;
    default:
        break;
    }
}

static app_state_t calibration_finish_or_fail(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    calibration_play_step_voice(ctx);

    if (!ctx->state_runtime.calibration.result_voice_played) {
        ctx->state_runtime.calibration.result_voice_played = calibration_try_play_result_voice(ctx);
        return ST_CALIBRATION;
    }

    if (ctx->timer.tick - ctx->calibration.step_tick < STAY_TICKS_2S) {
        return ST_CALIBRATION;
    }

    if (voice_play_is_busy()) {
        return ST_CALIBRATION;
    }

    calibration_reset_runtime(ctx);
    return ST_READY;
}

static app_state_t detection_finish_or_fail(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    detection_play_step_voice(ctx);

    if (!ctx->state_runtime.detection.result_voice_played) {
        ctx->state_runtime.detection.result_voice_played = detection_try_play_result_voice(ctx);
        return ST_DETECTION;
    }

    if (ctx->timer.tick - ctx->detection.step_tick < STAY_TICKS_2S) {
        return ST_DETECTION;
    }

    if (voice_play_is_busy()) {
        return ST_DETECTION;
    }

    detection_reset_runtime(ctx);
    return ST_READY;
}

static app_state_t state_handle_powder_calibration(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    if (ctx->ms.error_code != 0U && ctx->calibration.step != CAL_STEP_FAIL) {
        ESP_LOGW(TAG, "powder calibration failed by error_code=0x%08lx", ctx->ms.error_code);
        calibration_set_step(ctx, CAL_STEP_FAIL);
    }

    calibration_play_step_voice(ctx);

    if (ctx->input.key_pressed & (1U << KEY_GRIND)) {
        ctx->input.key_pressed &= ~(1U << KEY_GRIND);
        calibration_reset_runtime(ctx);
        return ST_READY;
    }

    switch (ctx->calibration.step) {
    case CAL_STEP_POWDER_PLACE_CUP:
        if (ctx->input.key_pressed & (1U << KEY_ESPRESSO)) {
            ctx->input.key_pressed &= ~(1U << KEY_ESPRESSO);

            if (ctx->timer.tick - ctx->calibration.step_tick < STAY_TICKS_3S) {
                ESP_LOGI(TAG, "powder calibration ignore confirm before cup settle");
                break;
            }

            ctx->state_runtime.calibration.powder_adc_baseline = ctx->ms.powder_adc;
            ctx->state_runtime.calibration.powder_weight_baseline = ctx->ms.powder_weight;
            ESP_LOGI(TAG,
                     "powder calibration captured adc0=%ld weight0=%.3f after confirm, prompt 500g placement",
                     (long)ctx->state_runtime.calibration.powder_adc_baseline,
                     ctx->state_runtime.calibration.powder_weight_baseline);
            calibration_set_step(ctx, CAL_STEP_POWDER_WAIT_START);
        }
        break;

    case CAL_STEP_POWDER_WAIT_WEIGHT:
        break;

    case CAL_STEP_POWDER_WAIT_START:
        if (ctx->input.key_pressed & (1U << KEY_ESPRESSO)) {
            ctx->input.key_pressed &= ~(1U << KEY_ESPRESSO);
            ctx->state_runtime.calibration.powder_signal_seen = false;
            ctx->state_runtime.calibration.powder_adc_weight_point = 0;
            ESP_LOGI(TAG,
                     "powder calibration waiting for 500g point baseline=%ld current_adc=%ld",
                     (long)ctx->state_runtime.calibration.powder_adc_baseline,
                     (long)ctx->ms.powder_adc);
            calibration_set_step(ctx, CAL_STEP_RUNNING);
        }
        break;

    case CAL_STEP_RUNNING:
        if (!ctx->state_runtime.calibration.powder_signal_seen &&
            powder_calibration_signal_seen(ctx)) {
            ctx->state_runtime.calibration.powder_signal_seen = true;
            ctx->calibration.step_tick = ctx->timer.tick;
            ESP_LOGI(TAG,
                     "powder calibration detected 500g signal adc=%ld delta=%ld",
                     (long)ctx->ms.powder_adc,
                     (long)(ctx->ms.powder_adc - ctx->state_runtime.calibration.powder_adc_baseline));
        }

        if (ctx->state_runtime.calibration.powder_signal_seen &&
            ctx->timer.tick - ctx->calibration.step_tick >= STAY_TICKS_2S) {
            ctx->state_runtime.calibration.powder_adc_weight_point = ctx->ms.powder_adc;
            if (powder_calibration_apply_result(ctx)) {
                calibration_set_step(ctx, CAL_STEP_DONE);
            } else {
                calibration_set_step(ctx, CAL_STEP_FAIL);
            }
        }
        break;

    case CAL_STEP_DONE:
    case CAL_STEP_FAIL:
        return calibration_finish_or_fail(ctx);

    default:
        calibration_set_step(ctx, CAL_STEP_POWDER_PLACE_CUP);
        break;
    }

    return ST_CALIBRATION;
}

static app_state_t state_handle_flow_calibration(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    if (ctx->ms.error_code != 0U && ctx->calibration.step != CAL_STEP_FAIL) {
        ESP_LOGW(TAG, "flow calibration failed by error_code=0x%08lx", ctx->ms.error_code);
        calibration_set_step(ctx, CAL_STEP_FAIL);
    }

    calibration_play_step_voice(ctx);

    if (ctx->input.key_pressed & (1U << KEY_CLEAN)) {
        ctx->input.key_pressed &= ~(1U << KEY_CLEAN);
        calibration_reset_runtime(ctx);
        return ST_READY;
    }

    switch (ctx->calibration.step) {
    case CAL_STEP_FLOW_SELECT:
        if (ctx->input.key_long & (1U << KEY_ESPRESSO)) {
            ctx->input.key_long &= ~(1U << KEY_ESPRESSO);
            flow_calibration_enter_mode(ctx, CAL_MODE_HOT_DRINK);
        } else if (ctx->input.key_long & (1U << KEY_AMERICANO)) {
            ctx->input.key_long &= ~(1U << KEY_AMERICANO);
            flow_calibration_enter_mode(ctx, CAL_MODE_HOT_WATER);
        }
        break;

    case CAL_STEP_RUNNING:
        if (ctx->input.key_pressed & (1U << KEY_GRIND)) {
            ctx->input.key_pressed &= ~(1U << KEY_GRIND);
            if (ctx->state_runtime.calibration.flow_adjust_percent < FLOW_CAL_PERCENT_MAX) {
                ctx->state_runtime.calibration.flow_adjust_percent++;
            }
            ESP_LOGI(TAG,
                     "flow calibration adjust mode=%d percent=%d coeff=%.8f",
                     ctx->calibration.mode,
                     (int)ctx->state_runtime.calibration.flow_adjust_percent,
                     (double)(ctx->state_runtime.calibration.flow_coeff_base *
                              (1.0f + ((float)ctx->state_runtime.calibration.flow_adjust_percent / 100.0f))));
        }

        if (ctx->input.key_pressed & (1U << KEY_TEMP)) {
            ctx->input.key_pressed &= ~(1U << KEY_TEMP);
            if (ctx->state_runtime.calibration.flow_adjust_percent > FLOW_CAL_PERCENT_MIN) {
                ctx->state_runtime.calibration.flow_adjust_percent--;
            }
            ESP_LOGI(TAG,
                     "flow calibration adjust mode=%d percent=%d coeff=%.8f",
                     ctx->calibration.mode,
                     (int)ctx->state_runtime.calibration.flow_adjust_percent,
                     (double)(ctx->state_runtime.calibration.flow_coeff_base *
                              (1.0f + ((float)ctx->state_runtime.calibration.flow_adjust_percent / 100.0f))));
        }

        if (ctx->input.key_long & (1U << KEY_STEAM)) {
            ctx->input.key_long &= ~(1U << KEY_STEAM);
            if (flow_calibration_commit(ctx)) {
                calibration_set_step(ctx, CAL_STEP_DONE);
            } else {
                calibration_set_step(ctx, CAL_STEP_FAIL);
            }
        }
        break;

    case CAL_STEP_DONE:
    case CAL_STEP_FAIL:
        return calibration_finish_or_fail(ctx);

    default:
        calibration_set_step(ctx, CAL_STEP_FLOW_SELECT);
        break;
    }

    return ST_CALIBRATION;
}

app_state_t state_handle_calibration(app_ctx_t *ctx)
{
    if (!ctx) {
        return ST_READY;
    }

    if (!ctx->state_runtime.calibration.entered) {
        ctx->state_runtime.calibration.entered = true;
        ctx->state_runtime.calibration.last_step = CAL_STEP_NONE;
        ctx->state_runtime.calibration.result_voice_played = false;
        ctx->state_runtime.calibration.powder_adc_baseline = 0;
        ctx->state_runtime.calibration.powder_adc_weight_point = 0;
        ctx->state_runtime.calibration.powder_weight_baseline = 0.0f;
        ctx->state_runtime.calibration.powder_signal_seen = false;
        ctx->state_runtime.calibration.flow_coeff_base = 0.0f;
        ctx->state_runtime.calibration.flow_adjust_percent = 0U;
        ctx->calibration.step_tick = ctx->timer.tick;

        if (ctx->calibration.mode == CAL_MODE_POWDER) {
            calibration_set_step(ctx, CAL_STEP_POWDER_PLACE_CUP);
        } else {
            ctx->calibration.mode = CAL_MODE_NONE;
            calibration_set_step(ctx, CAL_STEP_FLOW_SELECT);
        }
        ESP_LOGI(TAG, "enter calibration mode=%d", ctx->calibration.mode);
    }

    if (ctx->calibration.mode == CAL_MODE_POWDER) {
        return state_handle_powder_calibration(ctx);
    }

    return state_handle_flow_calibration(ctx);
}

app_state_t state_handle_detection(app_ctx_t *ctx)
{
    bool condition_met;

    if (!ctx) {
        return ST_READY;
    }

    if (!ctx->state_runtime.detection.entered) {
        ctx->state_runtime.detection.entered = true;
        ctx->state_runtime.detection.last_step = DET_STEP_NONE;
        ctx->state_runtime.detection.result_voice_played = false;
        ctx->state_runtime.detection.sensor_latched = false;
        ctx->state_runtime.detection.transition_armed = false;
        ctx->state_runtime.detection.grinder_started = false;
        ctx->detection.pass_mask = 0U;
        ctx->detection.fail_mask = 0U;
        detection_set_step(ctx, DET_STEP_PLACE_PORTAFILTER);
        ESP_LOGI(TAG, "enter detection");
    }

    if (ctx->ms.error_code != 0U && ctx->detection.step != DET_STEP_FAIL) {
        ESP_LOGW(TAG, "detection failed by error_code=0x%08lx", ctx->ms.error_code);
        detection_mark_fail(ctx, detection_result_flag_for_step(ctx->detection.step));
        detection_stop_grinder(ctx);
        detection_set_step(ctx, DET_STEP_FAIL);
    }

    detection_play_step_voice(ctx);

    if (ctx->input.key_pressed & (1U << KEY_CLEAN)) {
        ctx->input.key_pressed &= ~(1U << KEY_CLEAN);
        detection_reset_runtime(ctx);
        return ST_READY;
    }

    switch (ctx->detection.step) {
    case DET_STEP_PLACE_PORTAFILTER:
        condition_met = detection_grind_handle_present(ctx);
        if (!detection_arm_on_inactive(ctx, condition_met)) {
            break;
        }
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_set_step(ctx, DET_STEP_REMOVE_PORTAFILTER);
        }
        break;

    case DET_STEP_REMOVE_PORTAFILTER:
        condition_met = !detection_grind_handle_present(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_mark_pass(ctx, DET_RESULT_GRIND_HANDLE);
            detection_set_step(ctx, DET_STEP_REMOVE_WATER_TANK);
        }
        break;

    case DET_STEP_REMOVE_WATER_TANK:
        condition_met = detection_water_tank_missing(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_set_step(ctx, DET_STEP_PUTBACK_WATER_TANK);
        }
        break;

    case DET_STEP_PUTBACK_WATER_TANK:
        condition_met = !detection_water_tank_missing(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_mark_pass(ctx, DET_RESULT_WATER_TANK);
            detection_set_step(ctx, DET_STEP_REMOVE_BEAN_HOPPER);
        }
        break;

    case DET_STEP_REMOVE_BEAN_HOPPER:
        condition_met = !detection_bean_hopper_installed(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_set_step(ctx, DET_STEP_PUTBACK_BEAN_HOPPER);
        }
        break;

    case DET_STEP_PUTBACK_BEAN_HOPPER:
        condition_met = detection_bean_hopper_installed(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_mark_pass(ctx, DET_RESULT_BEAN_HOPPER);
            detection_set_step(ctx, DET_STEP_UNLOCK_HOPPER);
        }
        break;

    case DET_STEP_UNLOCK_HOPPER:
        condition_met = detection_clear_bean_unlocked(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            ctx->ctrl.src = CTRL_SRC_UI;
            if (ctr_cmd_action(CTRL_ACT_GRIND_START, NULL)) {
                ctx->state_runtime.detection.grinder_started = true;
                detection_set_step(ctx, DET_STEP_CLEAR_BEAN_RUNNING);
            } else {
                detection_mark_fail(ctx, DET_RESULT_CLEAR_BEAN);
                detection_set_step(ctx, DET_STEP_FAIL);
            }
        }
        break;

    case DET_STEP_CLEAR_BEAN_RUNNING:
        if (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CLEAR_BEAN_TICKS) {
            detection_stop_grinder(ctx);
            detection_mark_pass(ctx, DET_RESULT_CLEAR_BEAN);
            detection_set_step(ctx, DET_STEP_REMOVE_BEAN_HOPPER_AFTER_CLEAR);
        }
        break;

    case DET_STEP_REMOVE_BEAN_HOPPER_AFTER_CLEAR:
        condition_met = !detection_clear_bean_unlocked(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_set_step(ctx, DET_STEP_HANDLE_FIT);
        }
        break;

    case DET_STEP_HANDLE_FIT:
        condition_met = detection_brew_handle_present(ctx);
        if (!detection_arm_on_inactive(ctx, condition_met)) {
            break;
        }
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_set_step(ctx, DET_STEP_REMOVE_PORTAFILTER_FINAL);
        }
        break;

    case DET_STEP_REMOVE_PORTAFILTER_FINAL:
        condition_met = !detection_brew_handle_present(ctx);
        if (condition_met && !ctx->state_runtime.detection.sensor_latched) {
            ctx->state_runtime.detection.sensor_latched = true;
            ctx->detection.step_tick = ctx->timer.tick;
        } else if (!condition_met) {
            ctx->state_runtime.detection.sensor_latched = false;
        }

        if (ctx->state_runtime.detection.sensor_latched &&
            (ctx->timer.tick - ctx->detection.step_tick >= DETECTION_CONFIRM_TICKS)) {
            detection_mark_pass(ctx, DET_RESULT_BREW_HANDLE);
            detection_set_step(ctx, DET_STEP_PASS);
        }
        break;

    case DET_STEP_PASS:
    case DET_STEP_FAIL:
        return detection_finish_or_fail(ctx);

    case DET_STEP_NONE:
    default:
        detection_set_step(ctx, DET_STEP_PLACE_PORTAFILTER);
        break;
    }

    return ST_DETECTION;
}
