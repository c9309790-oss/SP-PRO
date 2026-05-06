#pragma once

#include <stdbool.h>
#include "service_domain_types.h"
#include "sp_pro_app_types.h"
#include "controller_status_types.h"

bool device_statistics_store_init(void);
void device_statistics_notify_local_state_start(app_state_t state);
void device_statistics_notify_local_state_cancel(app_state_t state);
void device_statistics_notify_remote_action_start(control_action_t action, const formula_info_t *formula);
void device_statistics_notify_remote_cancel(void);
void device_statistics_notify_maintain_success(app_state_t state, float water_volume);
void device_statistics_handle_machine_status(const MACHINE_STATUS *status);
bool device_statistics_fill_snapshot(statistics_info_t *statistics);
bool device_statistics_should_raise_notice(maint_type_t type);
bool device_statistics_factory_reset(void);
