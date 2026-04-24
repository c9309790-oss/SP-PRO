#ifndef SP_PRO_APP_STATE_H
#define SP_PRO_APP_STATE_H

#include "sp_pro_app_context.h"

typedef app_state_t (*state_handler_t)(app_ctx_t *ctx);

void setting_send_param(app_ctx_t *ctx);
encoder_mode_t current_encoder_mode_for_ctx(app_ctx_t *ctx);
float get_cur_param_value_for_ctx(app_ctx_t *ctx, encoder_mode_t mode);
bool mode_is_temp(encoder_mode_t mode);
void alarm_check(app_ctx_t *ctx);
void handle_making_base(app_ctx_t *ctx);
void key_combo_tick(app_ctx_t *ctx);
void key_scan_ticks(app_ctx_t *ctx);
void key_event_handle(app_ctx_t *ctx, const bf7613_key_event_t *event);
bool clear_bean_should_enter(const app_ctx_t *ctx);
bool clear_bean_should_suppress_hopper_notice(const app_ctx_t *ctx);

const disp_param_desc_t *disp_param_of(encoder_mode_t mode);

void sp_pro_disp_model(disp_model_t *model);
void sp_pro_build_command_view(app_command_view_t *view);

float disp_get_setting_value(const app_display_view_t *view, encoder_mode_t mode);
setting_ui_phase_t ui_render_state(const app_display_view_t *view);
bool render_visible(const app_display_view_t *view);

void disp_clear(disp_element_t *d);
void disp_show_number(disp_element_t *d,
                      seg_position_t start_pos,
                      uint16_t number,
                      uint8_t digits);
void disp_set_key_icon(disp_element_t *d,
                       key_id_t key,
                       white_light_level_t level);
void disp_set_key_text(disp_element_t *d,
                       key_id_t key,
                       white_light_level_t level);
void disp_set_key_blink(disp_element_t *d,
                        key_id_t key,
                        bool blink);
void disp_set_l_indicator(disp_element_t *d,
                          l_indicator_t indicator,
                          uint8_t level);
void disp_set_s_indicator(disp_element_t *d,
                          unit_indicator_t unit,
                          bool on,
                          bool blink);
void disp_set_p_indicator(disp_element_t *d,
                          p_indicator_t status,
                          bool on,
                          bool blink);
void disp_show_h_code(disp_element_t *d, uint8_t major, uint8_t sub);
void disp_set_gauge_q1(disp_element_t *d, uint8_t level);
void disp_set_gauge_q2(disp_element_t *d, uint8_t level);
void disp_show_weight(disp_element_t *d,
                      pos_indicator_t pos,
                      float weight_g,
                      float target_g);
void disp_show_weight_with_unit(disp_element_t *d,
                                pos_indicator_t pos,
                                float weight,
                                float target,
                                bool use_gram_unit);
void disp_show_flow_coeff(disp_element_t *d,
                          pos_indicator_t pos,
                          float coeff,
                          int8_t adjust_percent);
void disp_show_brew_yield(disp_element_t *d,
                          pos_indicator_t pos,
                          float value,
                          float target,
                          bool use_gram_unit);
void disp_show_grind_w(disp_element_t *d,
                       float weight_g,
                       float target_g);
void disp_show_pressure_bar(disp_element_t *d,
                            float value_bar,
                            float max_bar);
void disp_show_temp(disp_element_t *d, pos_indicator_t pos, uint8_t temp);
void disp_show_time_s(disp_element_t *d, uint16_t seconds);
void disp_preheat_marquee(disp_element_t *d, uint8_t step);
void disp_set_voice_play(disp_element_t *d,
                         uint8_t voice_seq,
                         uint8_t voice_data);
void disp_apply_key_mode(const app_display_view_t *view,
                         disp_element_t *d,
                         key_display_mode_t mode,
                         uint16_t full_mask,
                         uint16_t blink_mask);
void disp_render_current_param(const app_display_view_t *view,
                               disp_element_t *d);

void disp_build_preheat_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_power_on_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_espresso_brew_page(const app_display_view_t *view, disp_element_t *d, key_id_t id);
void disp_build_cold_brew_prepare_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_cold_brew_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_water_prepare_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_water_page(const app_display_view_t *view, disp_element_t *d, key_id_t id);
void disp_build_steam_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_ready_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_grind_prepare_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_grind_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_clear_bean_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_lock_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_clean_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_alarm_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_fault_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_drink_set_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_water_in_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_hardness_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_factory_reset_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_calibration_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_detection_page(const app_display_view_t *view, disp_element_t *d);
void disp_build_ota_page(const app_display_view_t *view, disp_element_t *d);

drink_liquid_compensation_t sp_pro_get_active_liquid_compensation(const app_ctx_t *ctx);
float sp_pro_get_raw_liquid_progress_ml(const app_ctx_t *ctx);
float sp_pro_get_display_liquid_ml(const app_ctx_t *ctx);

app_state_t state_handle_off(app_ctx_t *ctx);
app_state_t state_handle_on(app_ctx_t *ctx);
app_state_t state_handle_ready(app_ctx_t *ctx);
app_state_t state_handle_standby(app_ctx_t *ctx);
app_state_t state_handle_preheat(app_ctx_t *ctx);
app_state_t state_handle_espresso(app_ctx_t *ctx);
app_state_t state_handle_master(app_ctx_t *ctx);
app_state_t state_handle_americano(app_ctx_t *ctx);
app_state_t state_handle_cold_brew(app_ctx_t *ctx);
app_state_t state_handle_drink_set(app_ctx_t *ctx);
app_state_t state_handle_setting(app_ctx_t *ctx);
app_state_t state_handle_calibration(app_ctx_t *ctx);
app_state_t state_handle_detection(app_ctx_t *ctx);
app_state_t state_handle_alarm(app_ctx_t *ctx);
app_state_t state_handle_ota(app_ctx_t *ctx);
app_state_t state_handle_wifi(app_ctx_t *ctx);
app_state_t state_handle_water(app_ctx_t *ctx);
app_state_t state_handle_steam(app_ctx_t *ctx);
app_state_t state_handle_grind(app_ctx_t *ctx);
app_state_t state_handle_clear_bean(app_ctx_t *ctx);
app_state_t state_handle_child_lock(app_ctx_t *ctx);
app_state_t state_handle_clean_brew(app_ctx_t *ctx);
app_state_t state_handle_maint_brew(app_ctx_t *ctx);
app_state_t state_handle_maint_des(app_ctx_t *ctx);
app_state_t state_handle_maint_steam(app_ctx_t *ctx);
app_state_t state_handle_maint_drain(app_ctx_t *ctx);
app_state_t state_handle_auto_test(app_ctx_t *ctx);

#endif /* SP_PRO_APP_STATE_H */
