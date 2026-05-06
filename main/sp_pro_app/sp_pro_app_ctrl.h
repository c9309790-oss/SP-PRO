#ifndef SP_PRO_APP_CTRL_H
#define SP_PRO_APP_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include "sp_pro_app_types.h"
#include "service_domain_types.h"

bool build_control_cmd(control_action_t action, char *buf, uint16_t len);
bool build_param_cmd(uint16_t val, char *buf, uint16_t len);
bool ctr_cmd_action(control_action_t action, void *param);
bool ctr_cmd_param(encoder_mode_t mode);

void sp_pro_app_set_ctrl_src(ctrl_src_t src);
void sp_pro_app_get_command_view(app_command_view_t *view);
void sp_pro_app_get_beverage_settings(app_beverage_settings_t *settings);
bool sp_pro_app_save_settings(void);
void sp_pro_app_set_clean_volume(float clean_v);
float sp_pro_app_get_clean_volume(void);
void sp_pro_app_set_water_in_mode(setting_water_in_t mode);
void sp_pro_app_restore_default_settings(void);
void sp_pro_app_reload_beverage_settings_from_formula_store(void);
bool sp_pro_app_is_off(void);
void sp_pro_app_enter_on(void);
void sp_pro_app_enter_ready(void);
void sp_pro_app_enter_off(void);
void sp_pro_app_request_remote_power_on(void);
void sp_pro_app_request_remote_power_off(void);
void sp_pro_app_enter_clean_brew(void);
void sp_pro_app_enter_maint_brew(void);
void sp_pro_app_enter_maint_des(void);
void sp_pro_app_enter_maint_steam(void);
void sp_pro_app_enter_maint_drain(void);
bool sp_pro_app_toggle_child_lock(void);
bool sp_pro_app_is_child_lock_enabled(void);
app_state_t sp_pro_app_get_state(void);
int sp_pro_app_get_maintain_notice_status(maint_type_t type);
bool sp_pro_app_state_requires_brew_handle(app_state_t state);
bool sp_pro_app_formula_requires_brew_handle(const formula_info_t *formula);
bool sp_pro_app_is_brew_handle_in_place(void);
bool sp_pro_app_is_grind_handle_in_place(void);
bool sp_pro_app_start_remote_drink(const formula_info_t *formula);

#endif /* SP_PRO_APP_CTRL_H */
