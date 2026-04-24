#include "sp_pro_app_types.h"
#include "sp_pro_app_state.h"

static bool disp_use_scale_unit(const app_display_view_t *view);
static void disp_show_weight_progress_stable(disp_element_t *d,
                                             pos_indicator_t pos,
                                             float value,
                                             float target,
                                             bool use_gram_unit);

static void disp_set_main_indicators(disp_element_t *d,
                                     white_light_level_t l1,
                                     white_light_level_t l2,
                                     white_light_level_t l3,
                                     white_light_level_t l4)
{
    disp_set_l_indicator(d, BF_INDICATOR_L1, l1);
    disp_set_l_indicator(d, BF_INDICATOR_L2, l2);
    disp_set_l_indicator(d, BF_INDICATOR_L3, l3);
    disp_set_l_indicator(d, BF_INDICATOR_L4, l4);
}

static void disp_set_all_segments(disp_element_t *d, uint8_t value)
{
    if (!d) {
        return;
    }

    for (int i = 0; i < SM_POS_COUNT; i++) {
        d->segment.digit[i] = value;
    }
}

static void disp_set_all_status_icons(disp_element_t *d, bool on)
{
    if (!d) {
        return;
    }

    disp_set_p_indicator(d, BF_STATUS_P1, on, false);
    disp_set_p_indicator(d, BF_STATUS_P2, on, false);
    disp_set_p_indicator(d, BF_STATUS_P3, on, false);
    disp_set_p_indicator(d, BF_STATUS_P4, on, false);
    disp_set_p_indicator(d, BF_STATUS_P5, on, false);
}

static void disp_show_weight_progress_stable(disp_element_t *d,
                                             pos_indicator_t pos,
                                             float value,
                                             float target,
                                             bool use_gram_unit)
{
    if (!d) {
        return;
    }

    if (value >= 100.0f || target >= 100.0f) {
        disp_show_brew_yield(d, pos, value, target, use_gram_unit);
        return;
    }

    disp_show_weight_with_unit(d, pos, value, target, use_gram_unit);
}

static void disp_set_all_unit_icons(disp_element_t *d, bool on)
{
    if (!d) {
        return;
    }

    disp_set_s_indicator(d, BF_UNIT_S1, on, false);
    disp_set_s_indicator(d, BF_UNIT_S2, on, false);
    disp_set_s_indicator(d, BF_UNIT_S3, on, false);
    disp_set_s_indicator(d, BF_UNIT_S4, on, false);
    disp_set_s_indicator(d, BF_UNIT_S5, on, false);
    disp_set_s_indicator(d, BF_UNIT_S6, on, false);
    disp_set_s_indicator(d, BF_UNIT_S7, on, false);
    disp_set_s_indicator(d, BF_UNIT_S8, on, false);
    disp_set_s_indicator(d, BF_UNIT_S9, on, false);
    disp_set_s_indicator(d, BF_UNIT_S10, on, false);
    disp_set_s_indicator(d, BF_UNIT_S11, on, false);
}

static void disp_apply_wifi_key_display(const app_display_view_t *view,
                                        disp_element_t *d,
                                        bool remote_blink)
{
    if (!view || !d) {
        return;
    }

    disp_set_key_blink(d, BF_KEY_K10, false);
    d->keys.byte17.k10_blue = WHITE_LIGHT_OFF;

    if (view->ota_upgrade_prompt) {
        d->keys.byte17.k10_white = WHITE_LIGHT_FULL;
        disp_set_key_blink(d, BF_KEY_K10, true);
        return;
    }

    if (view->ota_upgrading) {
        d->keys.byte17.k10_white = WHITE_LIGHT_FULL;
        return;
    }

    if (view->ota_prompt_dismissed) {
        if (view->network_connected) {
            d->keys.byte17.k10_white = WHITE_LIGHT_OFF;
            d->keys.byte17.k10_blue = WHITE_LIGHT_FULL;
        } else {
            d->keys.byte17.k10_white = WHITE_LIGHT_HALF;
        }
        return;
    }

    if (remote_blink || view->network_connecting) {
        d->keys.byte17.k10_white = WHITE_LIGHT_OFF;
        d->keys.byte17.k10_blue = WHITE_LIGHT_FULL;
        disp_set_key_blink(d, BF_KEY_K10, true);
        return;
    }

    if (view->network_connected) {
        d->keys.byte17.k10_white = WHITE_LIGHT_OFF;
        d->keys.byte17.k10_blue = WHITE_LIGHT_FULL;
        return;
    }

    d->keys.byte17.k10_white = WHITE_LIGHT_HALF;
}

static void disp_apply_loading_marquee(const app_display_view_t *view, disp_element_t *d)
{
    uint8_t idx;

    if (!view || !d) {
        return;
    }

    idx = view->anim_step % 5U;

    for (int i = 0; i < BF_KEY_COUNT; i++) {
        disp_set_key_icon(d, i, WHITE_LIGHT_HALF);
        disp_set_key_blink(d, i, false);
    }

    for (int i = BF_KEY_K1; i <= BF_KEY_K5; i++) {
        disp_set_key_text(d, i, WHITE_LIGHT_HALF);
    }

    d->keys.byte17.k10_blue = WHITE_LIGHT_OFF;
    disp_set_key_icon(d, idx, WHITE_LIGHT_FULL);
    disp_set_key_text(d, idx, WHITE_LIGHT_FULL);
}

static void disp_set_ota_wifi_white(disp_element_t *d, bool blink)
{
    if (!d) {
        return;
    }

    d->keys.byte17.k10_blue = WHITE_LIGHT_OFF;
    d->keys.byte17.k10_white = WHITE_LIGHT_FULL;
    disp_set_key_blink(d, BF_KEY_K10, blink);
}

static key_id_t disp_alarm_target_key(const app_display_view_t *view)
{
    if (!view) {
        return BF_KEY_COUNT;
    }

    switch (view->state) {
    case ST_ESPRESSO:
    case ST_MASTER:
        return BF_KEY_K1;
    case ST_AMERICANO:
        return BF_KEY_K2;
    case ST_COLD_BREW:
        return BF_KEY_K3;
    case ST_WATER:
        return BF_KEY_K4;
    case ST_STEAM:
        return BF_KEY_K5;
    case ST_GRIND:
        return BF_KEY_K6;
    default:
        return BF_KEY_COUNT;
    }
}

static bool disp_alarm_ready_like_state(app_state_t state)
{
    switch (state) {
    case ST_READY:
    case ST_WIFI:
    case ST_STANDBY:
    case ST_LOCK:
    case ST_SETTING:
    case ST_DRINK_SET:
        return true;
    default:
        return false;
    }
}

static void disp_build_setting_header(const app_display_view_t *view,
                                      disp_element_t *d,
                                      key_id_t key,
                                      bool show_l2)
{
    disp_clear(d);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE, (uint16_t)(1U << key), 0);
    if (view && mode_is_temp(view->setting.current_mode)) {
        disp_set_key_icon(d, BF_KEY_K7, WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K7, WHITE_LIGHT_FULL);
    }
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        show_l2 ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_FULL);
}

static void disp_build_combo_select_page(const app_display_view_t *view,
                                         disp_element_t *d,
                                         uint16_t selected_mask,
                                         uint16_t blink_mask)
{
    disp_clear(d);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_FULL);
    disp_apply_key_mode(view, d, KEY_MODE_COMBO_BLINK, selected_mask, blink_mask);
}

static void disp_render_weight_param(const app_display_view_t *view,
                                     disp_element_t *d,
                                     const disp_param_desc_t *param_desc,
                                     float dynamic_value,
                                     disp_render_mode_t render_mode)
{
    if (render_mode == DISP_RENDER_BLINK && !render_visible(view)) {
        if (param_desc->clear_fn) {
            param_desc->clear_fn(d, param_desc->pos);
        }
        return;
    }

    switch (param_desc->mode) {
    case ENCODER_MODE_ESP_BREW_WEIGHT:
    case ENCODER_MODE_AME_BREW_WEIGHT:
    case ENCODER_MODE_AME_WATER_WEIGHT:
    case ENCODER_MODE_COLD_BREW_WEIGHT:
    case ENCODER_MODE_HOT_WATER_WEIGHT:
        disp_show_weight_with_unit(d,
                                   param_desc->pos,
                                   dynamic_value,
                                   disp_get_setting_value(view, param_desc->mode),
                                   disp_use_scale_unit(view));
        break;

    case ENCODER_MODE_GRIND_WEIGHT:
        disp_show_weight_with_unit(d,
                                   param_desc->pos,
                                   dynamic_value,
                                   disp_get_setting_value(view, param_desc->mode),
                                   true);
        break;

    case ENCODER_MODE_CLEAN_VOLUME:
    default:
        disp_show_weight(d, param_desc->pos, dynamic_value, disp_get_setting_value(view, param_desc->mode));
        break;
    }
}

static void disp_render_temp_param(const app_display_view_t *view,
                                   disp_element_t *d,
                                   const disp_param_desc_t *param_desc,
                                   float dynamic_value,
                                   disp_render_mode_t render_mode)
{
    if (render_mode == DISP_RENDER_BLINK && !render_visible(view)) {
        if (param_desc->clear_fn) {
            param_desc->clear_fn(d, param_desc->pos);
        }
        return;
    }

    (void)view;
    disp_show_temp(d, param_desc->pos, (uint8_t)(dynamic_value + 0.5f));
}

static void disp_render_number_param(const app_display_view_t *view,
                                     disp_element_t *d,
                                     const disp_param_desc_t *param_desc,
                                     float dynamic_value,
                                     disp_render_mode_t render_mode)
{
    if (render_mode == DISP_RENDER_BLINK && !render_visible(view)) {
        if (param_desc->clear_fn) {
            param_desc->clear_fn(d, param_desc->pos);
        }
        return;
    }

    disp_show_number(d, param_desc->pos, (uint16_t)(dynamic_value + 0.5f), param_desc->digits);
}

void disp_render_current_param(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    const disp_param_desc_t *param_desc = disp_param_of(view->setting.current_mode);
    if (!param_desc) {
        return;
    }

    float dynamic_value =
        (view->setting.ui_phase == SET_UI_PHASE_HINT)
            ? view->setting.target_val
            : view->setting.current_val;

    disp_render_mode_t render_mode =
        (ui_render_state(view) == SET_UI_PHASE_HINT)
            ? DISP_RENDER_BLINK
            : DISP_RENDER_STATIC;

    switch (param_desc->kind) {
    case DISP_KIND_WEIGHT:
        disp_render_weight_param(view, d, param_desc, dynamic_value, render_mode);
        break;
    case DISP_KIND_TEMP:
        disp_render_temp_param(view, d, param_desc, dynamic_value, render_mode);
        break;
    case DISP_KIND_LEVEL:
        disp_render_number_param(view, d, param_desc, dynamic_value, render_mode);
        break;
    }
}

/* ============================================================
 * Page builders
 * ============================================================ */

void disp_build_ready_page(const app_display_view_t *view, disp_element_t *d)
{
    bool steam_preheating = false;
    bool maint_notice = false;

    if (view) {
        steam_preheating = (view->ms.steam_flag == STEAM_UNREADY);
        maint_notice = view->alarm.active &&
                       view->alarm.is_notice &&
                       ((view->alarm.notice_p == BF_STATUS_P3) ||
                        (view->alarm.notice_p == BF_STATUS_P4) ||
                        (view->alarm.notice_p == BF_STATUS_P5));
    }

    disp_clear(d);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_HALF);
    disp_apply_key_mode(view, d, KEY_MODE_READY, 0, 0);
    disp_apply_wifi_key_display(view, d, false);

    if (steam_preheating) {
        disp_set_key_blink(d, BF_KEY_K5, true);
    }

    if (maint_notice) {
        disp_set_key_icon(d, BF_KEY_K9, WHITE_LIGHT_FULL);
        disp_set_key_blink(d, BF_KEY_K9, true);
    }
}

void disp_build_power_on_page(const app_display_view_t *view, disp_element_t *d)
{
    bool lights_on;

    if (!d) {
        return;
    }

    if (view && view->substate == (brew_substate_t)POWER_ON_SUB_LOADING) {
        disp_clear(d);
        disp_apply_loading_marquee(view, d);
        disp_set_main_indicators(
            d,
            WHITE_LIGHT_OFF,
            WHITE_LIGHT_OFF,
            WHITE_LIGHT_HALF,
            WHITE_LIGHT_HALF);
        return;
    }

    lights_on = view && ((view->anim_step & 0x01U) == 0U);
    disp_clear(d);

    for (int i = 0; i < BF_KEY_COUNT; i++) {
        disp_set_key_icon(d, i, lights_on ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF);
        disp_set_key_text(d, i, lights_on ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF);
        disp_set_key_blink(d, i, false);
    }

    disp_set_main_indicators(
        d,
        lights_on ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF,
        lights_on ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF,
        lights_on ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF,
        lights_on ? WHITE_LIGHT_FULL : WHITE_LIGHT_OFF);

    if (!lights_on) {
        return;
    }

    /* Match the screen protocol's "full bright" example frame:
     * - all 12 segment bytes = 0xFF
     * - all key channels = full
     * - all P/S indicators on
     * - all L indicators full
     * - both gauges lit
     */
    disp_set_all_segments(d, 0xFF);
    d->dots.dot[DISP_DOT_LEFT_WEIGHT] = true;
    d->dots.dot[DISP_DOT_RIGHT_WEIGHT] = true;
    d->keys.byte17.k10_blue = WHITE_LIGHT_FULL;
    disp_set_all_status_icons(d, true);
    disp_set_all_unit_icons(d, true);
    d->q1_gauge = 0x17;
    d->q2_gauge = 0x19;
}

void disp_build_preheat_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    if (view->drink.remote_active) {
        switch (view->drink.target_drink) {
        case DRINK_ESPRESSO:
        case DRINK_MASTER:
            disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0U, (uint16_t)(1U << BF_KEY_K1));
            break;
        case DRINK_AMERICANO:
            disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0U, (uint16_t)(1U << BF_KEY_K2));
            break;
        default:
            break;
        }
    }
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);

    switch (view->drink.target_drink) {
    case DRINK_ESPRESSO:
    case DRINK_MASTER:
        if (view->drink.remote_active) {
            disp_show_weight_with_unit(d,
                                       BF_POS_LEFT,
                                       view->drink.target_ml,
                                       view->drink.target_ml,
                                       disp_use_scale_unit(view));
            disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->drink.target_temp + 0.5f));
        } else {
            disp_show_weight(d, BF_POS_LEFT, view->setting.esp_brew_w, view->setting.esp_brew_w);
            disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->setting.esp_brew_t + 0.5f));
        }
        disp_set_gauge_q1(d, Q1_GAUGE_MAX_VAL);
        break;

    case DRINK_AMERICANO:
        disp_set_l_indicator(d, BF_INDICATOR_L2, WHITE_LIGHT_FULL);
        if (view->drink.remote_active) {
            disp_show_weight_with_unit(d,
                                       BF_POS_LEFT,
                                       view->drink.target_ml,
                                       view->drink.target_ml,
                                       disp_use_scale_unit(view));
            disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->drink.target_temp + 0.5f));
        } else {
            disp_show_weight(d, BF_POS_LEFT, view->setting.ame_brew_w, view->setting.ame_brew_w);
            disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->setting.ame_brew_t + 0.5f));
        }
        disp_set_gauge_q1(d, Q1_GAUGE_MAX_VAL);
        if (view->drink.remote_active) {
            disp_show_weight_with_unit(d,
                                       BF_POS_RIGHT,
                                       view->drink.secondary_target_ml,
                                       view->drink.secondary_target_ml,
                                       disp_use_scale_unit(view));
            disp_show_temp(d, BF_POS_RIGHT, (uint8_t)(view->drink.secondary_target_temp + 0.5f));
        } else {
            disp_show_weight(d, BF_POS_RIGHT, view->setting.ame_water_w, view->setting.ame_water_w);
            disp_show_temp(d, BF_POS_RIGHT, (uint8_t)(view->setting.ame_water_t + 0.5f));
        }
        disp_set_gauge_q2(d, Q2_GAUGE_MAX_VAL);
        break;

    default:
        disp_show_temp(d, BF_POS_LEFT, view->ms.hot_current_temp);
        break;
    }

    if (view->drink.remote_active && view->drink.countdown_seconds > 0U) {
        disp_show_time_s(d, view->drink.countdown_seconds);
    } else {
        disp_preheat_marquee(d, view->anim_step);
    }

    for (int i = BF_KEY_K6; i < BF_KEY_COUNT; i++) {
        disp_set_key_icon(d, i, WHITE_LIGHT_HALF);
        disp_set_key_text(d, i, WHITE_LIGHT_HALF);
        disp_set_key_blink(d, i, false);
    }

    disp_apply_wifi_key_display(view, d, view->drink.remote_active);
}

static bool disp_use_scale_unit(const app_display_view_t *view)
{
    (void)view;
    return false;
}

void disp_build_espresso_brew_page(const app_display_view_t *view, disp_element_t *d, key_id_t drink_key)
{
    uint8_t display_temp;

    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0, (uint16_t)(1U << drink_key));
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);

    display_temp = (view->drink.remote_active && view->drink.target_temp > 0.0f) ?
                   (uint8_t)(view->drink.target_temp + 0.5f) :
                   view->ms.brew_current_temp;

    disp_show_time_s(d, (uint16_t)view->drink.elapsed_tick);
    disp_show_pressure_bar(d, view->ms.pressure, PRESSURE_MAX_VAL);
    disp_show_temp(d, BF_POS_LEFT, display_temp);
    disp_show_brew_yield(d,
                         BF_POS_LEFT,
                         view->drink.display_liquid_ml,
                         view->drink.target_ml,
                         disp_use_scale_unit(view));
}

void disp_build_cold_brew_prepare_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d,
                        view->drink.remote_active ? KEY_MODE_SINGLE_BLINK : KEY_MODE_SINGLE,
                        view->drink.remote_active ? 0U : (uint16_t)(1U << BF_KEY_K3),
                        view->drink.remote_active ? (uint16_t)(1U << BF_KEY_K3) : 0U);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);
    disp_show_weight(d, BF_POS_LEFT, view->drink.target_ml, view->drink.target_ml);
    if (view->drink.remote_active && view->drink.countdown_seconds > 0U) {
        disp_show_time_s(d, view->drink.countdown_seconds);
    }

    disp_apply_wifi_key_display(view, d, view->drink.remote_active);
}

void disp_build_cold_brew_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0, (uint16_t)(1U << BF_KEY_K3));
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);

    disp_show_time_s(d, (uint16_t)view->drink.elapsed_tick);
    disp_show_pressure_bar(d, view->ms.pressure, PRESSURE_MAX_VAL);
    disp_show_brew_yield(d,
                         BF_POS_LEFT,
                         view->drink.display_liquid_ml,
                         view->drink.target_ml,
                         disp_use_scale_unit(view));
}

void disp_build_water_prepare_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d,
                        view->drink.remote_active ? KEY_MODE_SINGLE_BLINK : KEY_MODE_SINGLE,
                        view->drink.remote_active ? 0U : (uint16_t)(1U << BF_KEY_K4),
                        view->drink.remote_active ? (uint16_t)(1U << BF_KEY_K4) : 0U);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);
    disp_set_s_indicator(d, BF_UNIT_S1, true, false);

    disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->drink.target_temp + 0.5f));
    disp_show_weight(d, BF_POS_LEFT, view->drink.target_ml, view->drink.target_ml);
    if (view->drink.remote_active && view->drink.countdown_seconds > 0U) {
        disp_show_time_s(d, view->drink.countdown_seconds);
    }

    disp_apply_wifi_key_display(view, d, view->drink.remote_active);
}

void disp_build_water_page(const app_display_view_t *view, disp_element_t *d, key_id_t drink_key)
{
    uint8_t display_temp;

    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0, (uint16_t)(1U << drink_key));
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);
    disp_set_s_indicator(d, BF_UNIT_S1, true, false);

    if (drink_key == BF_KEY_K2) {
        disp_show_time_s(d, (uint16_t)view->drink.elapsed_tick);
    }

    if (drink_key == BF_KEY_K4) {
        display_temp = (uint8_t)(view->drink.target_temp + 0.5f);
    } else {
        display_temp = (view->drink.remote_active && view->drink.target_temp > 0.0f) ?
                       (uint8_t)(view->drink.target_temp + 0.5f) :
                       view->ms.hot_current_temp;
    }
    disp_show_temp(d, BF_POS_LEFT, display_temp);
    disp_show_weight_progress_stable(d,
                                     BF_POS_LEFT,
                                     view->drink.display_liquid_ml,
                                     view->drink.target_ml,
                                     disp_use_scale_unit(view));
}

void disp_build_steam_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_OFF);

    if (view->drink.tail_spray_pending || view->drink.tail_spray_running) {
        disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0U, (uint16_t)(1U << BF_KEY_K9));
        disp_set_key_icon(d, BF_KEY_K5, WHITE_LIGHT_HALF);
        disp_set_key_text(d, BF_KEY_K5, WHITE_LIGHT_HALF);

        if (view->drink.countdown_seconds > 0U) {
            disp_show_time_s(d, view->drink.countdown_seconds);
        }
    } else {
        disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0, (uint16_t)(1U << BF_KEY_K5));
    }

    disp_show_number(d, SM_POS_2, (uint16_t)(view->setting.steam_level + 0.5f), 1);
}

void disp_build_grind_prepare_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE, (uint16_t)(1U << BF_KEY_K6), 0);
    disp_show_weight(d, BF_POS_LEFT, view->setting.grind_w, view->setting.grind_w);
    disp_set_l_indicator(d, BF_INDICATOR_L4, WHITE_LIGHT_FULL);
}

void disp_build_grind_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_show_grind_w(d, view->ms.powder_weight, view->setting.grind_w);
    disp_set_l_indicator(d, BF_INDICATOR_L4, WHITE_LIGHT_FULL);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0, (uint16_t)(1U << BF_KEY_K6));
}

void disp_build_clear_bean_page(const app_display_view_t *view, disp_element_t *d)
{
    uint16_t blink_mask = (uint16_t)(1U << BF_KEY_K6);

    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0U, blink_mask);
    disp_set_l_indicator(d, BF_INDICATOR_L4, WHITE_LIGHT_FULL);

    if (view->clear_bean.step == CLEAR_BEAN_STEP_RUNNING ||
        view->clear_bean.step == CLEAR_BEAN_STEP_DONE) {
        disp_show_grind_w(d, view->ms.powder_weight, view->setting.grind_w);
    }
}

static void disp_setting_espresso(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K1, false);
    disp_show_weight_with_unit(d,
                               BF_POS_LEFT,
                               view->setting.esp_brew_w,
                               view->setting.esp_brew_w,
                               disp_use_scale_unit(view));
    disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->setting.esp_brew_t + 0.5f));
    disp_render_current_param(view, d);
}

static void disp_setting_america(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K2, true);
    disp_show_weight_with_unit(d,
                               BF_POS_LEFT,
                               view->setting.ame_brew_w,
                               view->setting.ame_brew_w,
                               disp_use_scale_unit(view));
    disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->setting.ame_brew_t + 0.5f));
    disp_show_weight_with_unit(d,
                               BF_POS_RIGHT,
                               view->setting.ame_water_w,
                               view->setting.ame_water_w,
                               disp_use_scale_unit(view));
    disp_show_temp(d, BF_POS_RIGHT, (uint8_t)(view->setting.ame_water_t + 0.5f));
    disp_render_current_param(view, d);
}

static void disp_setting_coldbrew(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K3, false);
    disp_show_weight_with_unit(d,
                               BF_POS_LEFT,
                               view->setting.cold_brew_w,
                               view->setting.cold_brew_w,
                               disp_use_scale_unit(view));
    disp_render_current_param(view, d);
}

static void disp_setting_water(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K4, false);
    disp_show_weight_with_unit(d,
                               BF_POS_LEFT,
                               view->setting.hot_water_w,
                               view->setting.hot_water_w,
                               disp_use_scale_unit(view));
    disp_show_temp(d, BF_POS_LEFT, (uint8_t)(view->setting.hot_water_t + 0.5f));
    disp_render_current_param(view, d);
}

static void disp_setting_steam(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K5, false);
    disp_show_number(d, SM_POS_2, (uint16_t)(view->setting.steam_level + 0.5f), 1);
    disp_render_current_param(view, d);
}

static void disp_setting_grind(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K6, false);
    disp_show_weight(d, BF_POS_LEFT, view->setting.grind_w, view->setting.grind_w);
    disp_render_current_param(view, d);
}

static void disp_setting_clean(const app_display_view_t *view, disp_element_t *d)
{
    disp_build_setting_header(view, d, BF_KEY_K9, false);
    disp_show_weight(d, BF_POS_LEFT, view->setting.clean_v, view->setting.clean_v);
    disp_render_current_param(view, d);
}

void disp_build_drink_set_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    switch (view->setting.drink_type) {
    case DRINK_GRIND:
        disp_setting_grind(view, d);
        break;
    case DRINK_ESPRESSO:
        disp_setting_espresso(view, d);
        break;
    case DRINK_AMERICANO:
        disp_setting_america(view, d);
        break;
    case DRINK_COLD_BREW:
        disp_setting_coldbrew(view, d);
        break;
    case DRINK_WATER:
        disp_setting_water(view, d);
        break;
    case DRINK_STEAM:
        disp_setting_steam(view, d);
        break;
    case KEY_CLEAN:
        disp_setting_clean(view, d);
        break;
    default:
        disp_clear(d);
        break;
    }
}

void disp_build_water_in_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    uint16_t selected_mask = 0;
    uint16_t blink_mask = (uint16_t)((1U << BF_KEY_K6) | (1U << BF_KEY_K7));

    if (view->setting.water_in_mode == WATER_IN_MODE_TANK) {
        selected_mask = (uint16_t)(1U << BF_KEY_K1);
    } else if (view->setting.water_in_mode == WATER_IN_MODE_BUCKET) {
        selected_mask = (uint16_t)(1U << BF_KEY_K2);
    }

    disp_build_combo_select_page(view, d, selected_mask, blink_mask);
}

void disp_build_hardness_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    uint16_t selected_mask = 0;
    uint16_t blink_mask = (uint16_t)((1U << BF_KEY_K7) | (1U << BF_KEY_K8));

    if (view->setting.hardness == HARDNESS_LEVEL_A) {
        selected_mask = (uint16_t)(1U << BF_KEY_K1);
    } else if (view->setting.hardness == HARDNESS_LEVEL_B) {
        selected_mask = (uint16_t)(1U << BF_KEY_K2);
    } else if (view->setting.hardness == HARDNESS_LEVEL_C) {
        selected_mask = (uint16_t)(1U << BF_KEY_K3);
    }

    disp_build_combo_select_page(view, d, selected_mask, blink_mask);
}

void disp_build_factory_reset_page(const app_display_view_t *view, disp_element_t *d)
{
    uint16_t combo_mask;

    if (!view || !d) {
        return;
    }

    combo_mask = (uint16_t)((1U << BF_KEY_K8) | (1U << BF_KEY_K10));

    if (view->setting.reset_step == RESET_STEP_DONE_AND_SHUTDOWN) {
        disp_build_combo_select_page(view,
                                     d,
                                     0U,
                                     (uint16_t)(combo_mask | (1U << BF_KEY_K1)));
        return;
    }

    disp_build_combo_select_page(view, d, combo_mask, 0U);
}

void disp_build_calibration_page(const app_display_view_t *view, disp_element_t *d)
{
    uint16_t selected_mask = 0U;
    uint16_t blink_mask = 0U;

    if (!view || !d) {
        return;
    }

    switch (view->calibration.mode) {
    case CAL_MODE_POWDER:
        if (view->calibration.step == CAL_STEP_POWDER_PLACE_CUP ||
            view->calibration.step == CAL_STEP_POWDER_WAIT_START ||
            view->calibration.step == CAL_STEP_RUNNING) {
            selected_mask = (uint16_t)(1U << BF_KEY_K1);
            blink_mask = (uint16_t)(1U << BF_KEY_K1);
        }
        break;

    case CAL_MODE_HOT_DRINK:
        if (view->calibration.step == CAL_STEP_FLOW_SELECT) {
            selected_mask = (uint16_t)((1U << BF_KEY_K1) | (1U << BF_KEY_K2));
        } else {
            selected_mask = (uint16_t)((1U << BF_KEY_K1) |
                                       (1U << BF_KEY_K5) |
                                       (1U << BF_KEY_K6) |
                                       (1U << BF_KEY_K7) |
                                       (1U << BF_KEY_K10));
        }
        break;

    case CAL_MODE_HOT_WATER:
        if (view->calibration.step == CAL_STEP_FLOW_SELECT) {
            selected_mask = (uint16_t)((1U << BF_KEY_K1) | (1U << BF_KEY_K2));
        } else {
            selected_mask = (uint16_t)((1U << BF_KEY_K2) |
                                       (1U << BF_KEY_K5) |
                                       (1U << BF_KEY_K6) |
                                       (1U << BF_KEY_K7) |
                                       (1U << BF_KEY_K10));
        }
        break;

    case CAL_MODE_NONE:
    default:
        selected_mask = (uint16_t)((1U << BF_KEY_K1) | (1U << BF_KEY_K2));
        break;
    }

    if (view->calibration.step == CAL_STEP_DONE || view->calibration.step == CAL_STEP_FAIL) {
        blink_mask = selected_mask;
    }

    disp_build_combo_select_page(view, d, selected_mask, blink_mask);

    switch (view->calibration.mode) {
    case CAL_MODE_HOT_DRINK:
        disp_show_flow_coeff(d,
                             BF_POS_LEFT,
                             view->calibration.flow_coeff_current,
                             view->calibration.flow_adjust_percent);
        break;

    case CAL_MODE_HOT_WATER:
        disp_show_flow_coeff(d,
                             BF_POS_RIGHT,
                             view->calibration.flow_coeff_current,
                             view->calibration.flow_adjust_percent);
        break;

    case CAL_MODE_NONE:
    case CAL_MODE_POWDER:
    default:
        break;
    }
}

void disp_build_detection_page(const app_display_view_t *view, disp_element_t *d)
{
    uint8_t pass_mask;
    uint8_t fail_mask;

    static const key_id_t pass_keys[5] = {
        BF_KEY_K6,
        BF_KEY_K7,
        BF_KEY_K8,
        BF_KEY_K9,
        BF_KEY_K10,
    };
    static const p_indicator_t fail_icons[5] = {
        BF_STATUS_P2,
        BF_STATUS_P1,
        BF_STATUS_P3,
        BF_STATUS_P4,
        BF_STATUS_P5,
    };

    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_HALF,
        WHITE_LIGHT_HALF,
        WHITE_LIGHT_HALF,
        WHITE_LIGHT_HALF);
    disp_apply_key_mode(view, d, KEY_MODE_ALL_OFF, 0U, 0U);

    switch (view->detection.step) {
    case DET_STEP_PLACE_PORTAFILTER:
        if (view->ms.grind_handle_postion_flag == 1U) {
            disp_set_key_icon(d, BF_KEY_K1, WHITE_LIGHT_FULL);
            disp_set_key_text(d, BF_KEY_K1, WHITE_LIGHT_FULL);
        }
        break;

    case DET_STEP_REMOVE_PORTAFILTER:
        disp_set_key_icon(d, BF_KEY_K1, WHITE_LIGHT_HALF);
        disp_set_key_text(d, BF_KEY_K1, WHITE_LIGHT_HALF);
        disp_set_key_icon(d, BF_KEY_K2, WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K2, WHITE_LIGHT_FULL);
        break;

    case DET_STEP_REMOVE_WATER_TANK:
        disp_set_key_icon(d, BF_KEY_K2, view->ms.water_box_shortage_flag ? WHITE_LIGHT_HALF : WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K2, view->ms.water_box_shortage_flag ? WHITE_LIGHT_HALF : WHITE_LIGHT_FULL);
        break;

    case DET_STEP_PUTBACK_WATER_TANK:
        disp_set_key_icon(d, BF_KEY_K2, view->ms.water_box_shortage_flag ? WHITE_LIGHT_HALF : WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K2, view->ms.water_box_shortage_flag ? WHITE_LIGHT_HALF : WHITE_LIGHT_FULL);
        break;

    case DET_STEP_REMOVE_BEAN_HOPPER:
        disp_set_key_icon(d, BF_KEY_K2, WHITE_LIGHT_HALF);
        disp_set_key_text(d, BF_KEY_K2, WHITE_LIGHT_HALF);
        disp_set_key_icon(d, BF_KEY_K3, (view->ms.beanbox_in_place == 1U) ? WHITE_LIGHT_FULL : WHITE_LIGHT_HALF);
        disp_set_key_text(d, BF_KEY_K3, (view->ms.beanbox_in_place == 1U) ? WHITE_LIGHT_FULL : WHITE_LIGHT_HALF);
        break;

    case DET_STEP_PUTBACK_BEAN_HOPPER:
        disp_set_key_icon(d, BF_KEY_K3, (view->ms.beanbox_in_place == 1U) ? WHITE_LIGHT_FULL : WHITE_LIGHT_HALF);
        disp_set_key_text(d, BF_KEY_K3, (view->ms.beanbox_in_place == 1U) ? WHITE_LIGHT_FULL : WHITE_LIGHT_HALF);
        break;

    case DET_STEP_UNLOCK_HOPPER:
        disp_set_key_icon(d, BF_KEY_K4, WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K4, WHITE_LIGHT_FULL);
        break;

    case DET_STEP_CLEAR_BEAN_RUNNING:
        disp_set_key_icon(d, BF_KEY_K4, WHITE_LIGHT_HALF);
        disp_set_key_text(d, BF_KEY_K4, WHITE_LIGHT_HALF);
        break;

    case DET_STEP_REMOVE_BEAN_HOPPER_AFTER_CLEAR:
        break;

    case DET_STEP_HANDLE_FIT:
        if (view->ms.brew_handle_postion_flag == 1U) {
            disp_set_key_icon(d, BF_KEY_K5, WHITE_LIGHT_FULL);
            disp_set_key_text(d, BF_KEY_K5, WHITE_LIGHT_FULL);
        }
        break;

    case DET_STEP_REMOVE_PORTAFILTER_FINAL:
        break;

    case DET_STEP_PASS:
    case DET_STEP_FAIL:
        pass_mask = view->detection.pass_mask;
        fail_mask = view->detection.fail_mask;
        for (int i = 0; i < 5; i++) {
            if (pass_mask & (1U << i)) {
                disp_set_key_icon(d, pass_keys[i], WHITE_LIGHT_FULL);
                if (pass_keys[i] <= BF_KEY_K5) {
                    disp_set_key_text(d, pass_keys[i], WHITE_LIGHT_FULL);
                }
            }
            if (fail_mask & (1U << i)) {
                disp_set_p_indicator(d, fail_icons[i], true, false);
            }
        }
        break;

    case DET_STEP_NONE:
    default:
        break;
    }
}

void disp_build_ota_page(const app_display_view_t *view, disp_element_t *d)
{
    ota_ui_substate_t phase;

    if (!view || !d) {
        return;
    }

    phase = (ota_ui_substate_t)view->substate;
    switch (phase) {
    case OTA_UI_REMINDER:
        disp_build_ready_page(view, d);
        disp_set_ota_wifi_white(d, true);
        break;

    case OTA_UI_UPGRADING:
        disp_clear(d);
        disp_apply_key_mode(view, d, KEY_MODE_ALL_OFF, 0U, 0U);
        disp_set_ota_wifi_white(d, true);
        break;

    case OTA_UI_SUCCESS:
    case OTA_UI_FAIL:
        disp_clear(d);
        disp_apply_key_mode(view, d, KEY_MODE_ALL_OFF, 0U, 0U);
        disp_set_ota_wifi_white(d, false);
        break;

    case OTA_UI_NONE:
    default:
        disp_build_ready_page(view, d);
        break;
    }
}

void disp_build_clean_page(const app_display_view_t *view, disp_element_t *d)
{
    uint16_t full_mask = 0U;
    uint16_t blink_mask = 0U;
    bool clean_icon_off = false;
    bool water_notice_blink = false;
    p_indicator_t maint_indicator = BF_STATUS_NO;

    if (!view || !d) {
        return;
    }

    disp_clear(d);

    if (view->state == ST_CLEAN_BREW) {
        if (view->maint.clean_state == MAINT_CLEAN_SUB_FINISH) {
            full_mask = (uint16_t)(1U << BF_KEY_K9);
        } else {
            blink_mask = (uint16_t)(1U << BF_KEY_K9);
        }
    } else if (view->state == ST_MAINT_STEAM) {
        maint_indicator = BF_STATUS_P3;
        switch (view->maint.steam_step) {
        case STEAM_STEP_WAIT_CLEAN1:
        case STEAM_STEP_WAIT_CLEAN2:
            blink_mask = (uint16_t)(1U << BF_KEY_K9);
            break;

        case STEAM_STEP_FINISH:
            clean_icon_off = true;
            break;

        case STEAM_STEP_WATER_LACK:
            full_mask = (uint16_t)(1U << BF_KEY_K9);
            water_notice_blink = true;
            break;

        case STEAM_STEP_ADD_WATER:
        case STEAM_STEP_ADD_POWDER:
        case STEAM_STEP_SOAK1:
        case STEAM_STEP_WAIT_CLICK1:
        case STEAM_STEP_WASH_PITCHER:
        case STEAM_STEP_SOAK2:
        case STEAM_STEP_WAIT_CLICK2:
        default:
            full_mask = (uint16_t)(1U << BF_KEY_K9);
            break;
        }
    } else if (view->state == ST_MAINT_BREW) {
        maint_indicator = BF_STATUS_P4;
        switch (view->maint.brew_step) {
        case BREW_STEP_WAIT_CLEAN1:
        case BREW_STEP_WAIT_CLEAN2:
            blink_mask = (uint16_t)(1U << BF_KEY_K9);
            break;

        case BREW_STEP_FINISH:
            clean_icon_off = true;
            break;

        case BREW_STEP_WATER_LACK:
            full_mask = (uint16_t)(1U << BF_KEY_K9);
            water_notice_blink = true;
            break;

        case BREW_STEP_ADD_WATER:
        case BREW_STEP_ADD_POWDER:
        case BREW_STEP_WAIT_HANDLE:
        case BREW_STEP_WAIT_CLICK1:
        case BREW_STEP_REMOVE_HANDLE:
        case BREW_STEP_WAIT_CLICK2:
        default:
            full_mask = (uint16_t)(1U << BF_KEY_K9);
            break;
        }
    } else if (view->state == ST_MAINT_DES) {
        maint_indicator = BF_STATUS_P5;
        switch (view->maint.des_step) {
        case DES_STEP_WAIT_CLEAN1:
        case DES_STEP_WAIT_CLEAN2:
            blink_mask = (uint16_t)(1U << BF_KEY_K9);
            break;

        case DES_STEP_FINISH:
            clean_icon_off = true;
            break;

        case DES_STEP_WATER_LACK:
            full_mask = (uint16_t)(1U << BF_KEY_K9);
            water_notice_blink = true;
            break;

        case DES_STEP_ADD_WATER:
        case DES_STEP_SWITCH_WATER_MODE:
        case DES_STEP_WAIT_TABLET:
        case DES_STEP_ADD_POWDER:
        case DES_STEP_WAIT_CLICK1:
        case DES_STEP_CHANGE_WATER:
        case DES_STEP_WAIT_CLICK2:
        default:
            full_mask = (uint16_t)(1U << BF_KEY_K9);
            break;
        }
    } else if (view->state == ST_MAINT_DRAIN) {
        switch (view->setting.clear_step) {
        case CLEAR_STEP_CUTOFF_SUPPLY:
        case CLEAR_STEP_CLEARING:
        case CLEAR_STEP_CLEARING_DONE:
            blink_mask = (uint16_t)((1U << BF_KEY_K9) | (1U << BF_KEY_K10));
            break;

        case CLEAR_STEP_FACTORY_RESET:
            blink_mask = (uint16_t)(1U << BF_KEY_K1);
            break;

        default:
            blink_mask = 0U;
            break;
        }

        if (view->setting.clear_step == CLEAR_STEP_CLEARING &&
            view->ms.water_box_shortage_flag) {
            disp_set_p_indicator(d, BF_STATUS_P1, true, false);
        }
    } else {
        blink_mask = (uint16_t)(1U << BF_KEY_K9);
    }

    if (blink_mask != 0U) {
        disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0U, blink_mask);
    } else {
        disp_apply_key_mode(view, d, KEY_MODE_SINGLE, full_mask, 0U);
    }

    if (full_mask != 0U && blink_mask == 0U) {
        disp_set_key_icon(d, BF_KEY_K9, WHITE_LIGHT_FULL);
    }

    if (clean_icon_off) {
        disp_set_key_icon(d, BF_KEY_K9, WHITE_LIGHT_OFF);
    }

    if (water_notice_blink) {
        disp_set_p_indicator(d, BF_STATUS_P1, true, true);
    }

    if (maint_indicator != BF_STATUS_NO) {
        disp_set_p_indicator(d, maint_indicator, true, false);
    }

    disp_set_l_indicator(d, BF_INDICATOR_L3, WHITE_LIGHT_FULL);
}

void disp_build_alarm_page(const app_display_view_t *view, disp_element_t *d)
{
    key_id_t target_key;

    if (!view || !d) {
        return;
    }

    target_key = disp_alarm_target_key(view);

    switch (view->alarm.warning) {
    case WARN_WATER_EMPTY:
        if (disp_alarm_ready_like_state(view->state)) {
            for (int i = BF_KEY_K1; i <= BF_KEY_K9; i++) {
                disp_set_key_icon(d, i, WHITE_LIGHT_HALF);
                if (i <= BF_KEY_K5) {
                    disp_set_key_text(d, i, WHITE_LIGHT_HALF);
                }
                disp_set_key_blink(d, i, false);
            }
        } else if (view->state == ST_WATER) {
            disp_set_key_icon(d, BF_KEY_K4, WHITE_LIGHT_FULL);
            disp_set_key_text(d, BF_KEY_K4, WHITE_LIGHT_FULL);
            disp_set_key_blink(d, BF_KEY_K4, true);
        }
        disp_set_p_indicator(d, BF_STATUS_P1, true, false);
        break;

    case WARN_WATER_BEAN_MISS:
        for (int i = BF_KEY_K1; i <= BF_KEY_K9; i++) {
            disp_set_key_icon(d, i, WHITE_LIGHT_HALF);
            if (i <= BF_KEY_K5) {
                disp_set_key_text(d, i, WHITE_LIGHT_HALF);
            }
            disp_set_key_blink(d, i, false);
        }
        disp_set_p_indicator(d, BF_STATUS_P1, true, false);
        disp_set_p_indicator(d, BF_STATUS_P2, true, false);
        break;

    case WARN_BEAN_MISS:
        if (disp_alarm_ready_like_state(view->state)) {
            disp_set_key_icon(d, BF_KEY_K1, WHITE_LIGHT_HALF);
            disp_set_key_text(d, BF_KEY_K1, WHITE_LIGHT_HALF);
            disp_set_key_icon(d, BF_KEY_K2, WHITE_LIGHT_HALF);
            disp_set_key_text(d, BF_KEY_K2, WHITE_LIGHT_HALF);
            disp_set_key_icon(d, BF_KEY_K3, WHITE_LIGHT_HALF);
            disp_set_key_text(d, BF_KEY_K3, WHITE_LIGHT_HALF);
            disp_set_key_icon(d, BF_KEY_K6, WHITE_LIGHT_HALF);
            disp_set_key_blink(d, BF_KEY_K6, false);
        }
        disp_set_p_indicator(d, BF_STATUS_P2, true, false);
        break;

    case WARN_BEAN_EMPTY:
        disp_set_key_icon(d, BF_KEY_K6, WHITE_LIGHT_FULL);
        disp_set_key_blink(d, BF_KEY_K6, true);
        disp_set_p_indicator(d, BF_STATUS_P2, true, false);
        break;

    case WARN_BREW_HD_MISS:
        if (target_key < BF_KEY_COUNT) {
            disp_set_key_icon(d, target_key, WHITE_LIGHT_FULL);
            if (target_key <= BF_KEY_K5) {
                disp_set_key_text(d, target_key, WHITE_LIGHT_FULL);
            }
            disp_set_key_blink(d, target_key, true);
        }
        disp_set_p_indicator(d, BF_STATUS_P2, true, false);
        break;

    case WARN_GRIND_HD_MISS:
        disp_set_key_icon(d, BF_KEY_K6, WHITE_LIGHT_FULL);
        disp_set_key_blink(d, BF_KEY_K6, true);
        break;

    case WARN_STEAM_NOT_READY:
        disp_set_key_icon(d, BF_KEY_K5, WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K5, WHITE_LIGHT_FULL);
        disp_set_key_blink(d, BF_KEY_K5, true);
        break;

    case WARN_NONE:
    case WARN_TRAY_MISS:
    case WARN_LIQUID_ABNM:
    default:
        if (view->alarm.notice_p != BF_STATUS_NO) {
            disp_set_p_indicator(d, view->alarm.notice_p, true, false);
        }
        break;
    }
}

void disp_build_fault_page(const app_display_view_t *view, disp_element_t *d)
{
    uint16_t blink_mask = 0U;

    if (!view || !d) {
        return;
    }

    disp_clear(d);
    if (view->alarm.major == 5 && view->alarm.retry_count < 3U) {
        blink_mask = (uint16_t)(1U << BF_KEY_K9);
    } else if (view->alarm.major == 10 && view->alarm.sub == 2) {
        blink_mask = (uint16_t)(1U << BF_KEY_K6);
    }

    disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0U, blink_mask);
    disp_set_l_indicator(d, BF_INDICATOR_L3, WHITE_LIGHT_FULL);
    disp_set_l_indicator(d, BF_INDICATOR_L4, WHITE_LIGHT_FULL);
    disp_show_h_code(d, view->alarm.major, view->alarm.sub);
}

void disp_build_lock_page(const app_display_view_t *view, disp_element_t *d)
{
    if (!view || !d) {
        return;
    }

    disp_clear(d);
    disp_set_main_indicators(
        d,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_OFF,
        WHITE_LIGHT_FULL,
        WHITE_LIGHT_FULL);

    if (view->child_lock.enabled) {
        disp_apply_key_mode(view, d, KEY_MODE_CHILD_LOCK, 0, 0);
    }

    if (view->child_lock.ui_hint == DISP_CHILD_LOCKED) {
        disp_apply_key_mode(view, d, KEY_MODE_SINGLE_BLINK, 0, (uint16_t)(1U << BF_KEY_K8));
    } else if (view->child_lock.ui_hint == DISP_CHILD_UNLOCK) {
        disp_apply_key_mode(view, d, KEY_MODE_CHILD_LOCK, 0, 0);
    }
}

