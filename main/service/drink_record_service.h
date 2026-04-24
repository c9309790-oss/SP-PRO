#pragma once

#include <stdbool.h>
#include "service_domain_types.h"
#include "sp_pro_app_types.h"
#include "controller_status_types.h"

bool drink_record_service_init(void);
void drink_record_notify_local_state_start(app_state_t state, const app_beverage_settings_t *settings);
void drink_record_notify_local_state_success(app_state_t state);
void drink_record_notify_local_state_cancel(app_state_t state);
void drink_record_notify_local_state_fail(app_state_t state);
void drink_record_notify_remote_action_start(control_action_t action, const formula_info_t *formula);
void drink_record_notify_remote_cancel(void);
void drink_record_notify_remote_fail(void);
void drink_record_handle_machine_status(const MACHINE_STATUS *status);
