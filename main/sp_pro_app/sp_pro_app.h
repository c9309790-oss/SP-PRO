#ifndef __SP_PRO_APP_H__
#define __SP_PRO_APP_H__

/* Public umbrella for app-layer headers. */
#include "sp_pro_app_types.h"
#include "sp_pro_app_context.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_state.h"

// #define AUTO_TEST   // Enable auto-test state handling.
// #define MAINT_TEST  // Enable maintenance test shortcuts.

/* Core app entry points. */
void sp_pro_app_init(void);
void sp_pro_state_machine(void);
void sp_pro_update_machine_status(const MACHINE_STATUS *status);
void handle_machine_status(const MACHINE_STATUS *status);
void sp_pro_handle_key_event(const bf7613_key_event_t *event);
disp_model_t *sp_pro_get_disp_model(void);
void sp_pro_consume_notice(app_ctx_t *ctx);
void sp_pro_dismiss_notice(app_ctx_t *ctx);

/* Voice manager API. */
void voice_manager_init(void);
bool voice_manager_play(voice_id_t id);
bool voice_manager_play_touch_tone(void);
void voice_manager_stop(void);
bool voice_manager_play_interrupt(voice_id_t id);
bool voice_manager_interval(voice_id_t id, uint32_t interval_ms);
bool voice_play_is_busy(void);
void voice_manager_clear_queue(void);

#endif /* __SP_PRO_APP_H__ */















