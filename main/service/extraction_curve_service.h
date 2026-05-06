#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "service_domain_types.h"
#include "sp_pro_app_types.h"
#include "controller_status_types.h"

#ifndef EXTRACTION_CURVE_FEATURE_ENABLE
#define EXTRACTION_CURVE_FEATURE_ENABLE 0
#endif

bool extraction_curve_service_init(void);
void extraction_curve_notify_local_state_start(app_state_t state, const app_beverage_settings_t *settings);
void extraction_curve_notify_local_state_success(app_state_t state);
void extraction_curve_notify_local_state_cancel(app_state_t state);
void extraction_curve_notify_local_state_fail(app_state_t state);
void extraction_curve_notify_remote_action_start(control_action_t action, const formula_info_t *formula);
void extraction_curve_notify_remote_cancel(void);
void extraction_curve_notify_remote_fail(void);
void extraction_curve_handle_machine_status(const MACHINE_STATUS *status);
void extraction_curve_handle_ack(const uint32_t *curve_ids, size_t curve_id_count);
