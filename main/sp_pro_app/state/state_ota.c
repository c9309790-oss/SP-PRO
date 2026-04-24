#include "sp_pro_app_state.h"
#include "sp_pro_app.h"
#include "mqtt_protocol.h"
#include "ota_ctr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "state_ota";

static ota_ui_substate_t ota_current_phase(void)
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

static bool ota_phase_has_barrier_voice(ota_ui_substate_t phase)
{
    return phase == OTA_UI_SUCCESS || phase == OTA_UI_FAIL;
}

static bool ota_prompt_dismissed(const app_ctx_t *ctx)
{
    if (!ctx || g_ota_info.ota_tkid[0] == '\0') {
        return false;
    }

    return strcmp(ctx->state_runtime.ota.dismissed_task_id, g_ota_info.ota_tkid) == 0;
}

static void ota_clear_runtime(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.ota.entered = false;
    ctx->state_runtime.ota.last_phase = OTA_UI_NONE;
    ctx->state_runtime.ota.phase_voice_played = false;
}

static void ota_on_phase_change(app_ctx_t *ctx, ota_ui_substate_t phase)
{
    if (!ctx) {
        return;
    }

    if (ctx->state_runtime.ota.last_phase == phase) {
        return;
    }

    ESP_LOGI(TAG, "OTA UI phase -> %u", (unsigned)phase);
    ctx->state_runtime.ota.last_phase = phase;
    ctx->state_runtime.ota.phase_voice_played = false;
}

static void ota_local_confirm(void)
{
    publish_ota_status_to_mqtt(OTA_YES_CONFIRM);
    ota_params_edit(OTA_INFO_AUTOUP, "9");
    ctr_ota_task_run();
}

app_state_t state_handle_ota(app_ctx_t *ctx)
{
    ota_ui_substate_t phase;

    if (!ctx) {
        return ST_READY;
    }

    phase = ota_current_phase();
    if (phase == OTA_UI_NONE || (phase == OTA_UI_REMINDER && ota_prompt_dismissed(ctx))) {
        if (ctx->state_runtime.ota.entered &&
            ota_phase_has_barrier_voice(ctx->state_runtime.ota.last_phase) &&
            ctx->state_runtime.ota.phase_voice_played &&
            voice_play_is_busy()) {
            ctx->core.substate = (brew_substate_t)ctx->state_runtime.ota.last_phase;
            return ST_OTA;
        }
        ota_clear_runtime(ctx);
        return ST_READY;
    }

    if (!ctx->state_runtime.ota.entered) {
        ctx->state_runtime.ota.entered = true;
        ctx->state_runtime.ota.last_phase = OTA_UI_NONE;
        ctx->state_runtime.ota.phase_voice_played = false;
    }

    ctx->core.substate = (brew_substate_t)phase;
    ota_on_phase_change(ctx, phase);

    switch (phase) {
    case OTA_UI_REMINDER:
        if (!ctx->state_runtime.ota.phase_voice_played) {
            ctx->state_runtime.ota.phase_voice_played =
                voice_manager_play_interrupt(VOICE_UPGRADEREMINDER);
        }

        if (ctx->input.key_long & (1U << KEY_WIFI)) {
            ctx->input.key_long &= ~(1U << KEY_WIFI);
            snprintf(ctx->state_runtime.ota.dismissed_task_id,
                     sizeof(ctx->state_runtime.ota.dismissed_task_id),
                     "%s",
                     g_ota_info.ota_tkid);
            voice_manager_stop();
            voice_manager_clear_queue();
            voice_manager_play_touch_tone();
            ota_clear_runtime(ctx);
            ESP_LOGI(TAG, "Dismiss OTA reminder locally taskId=%s", g_ota_info.ota_tkid);
            return ST_READY;
        }

        if (ctx->input.key_pressed & (1U << KEY_WIFI)) {
            ctx->input.key_pressed &= ~(1U << KEY_WIFI);
            voice_manager_stop();
            voice_manager_clear_queue();
            ota_local_confirm();
            ctx->core.substate = (brew_substate_t)OTA_UI_UPGRADING;
            ctx->state_runtime.ota.last_phase = OTA_UI_UPGRADING;
            ctx->state_runtime.ota.phase_voice_played = false;
            ESP_LOGI(TAG, "Start OTA from local HMI confirm taskId=%s", g_ota_info.ota_tkid);
        }
        break;

    case OTA_UI_UPGRADING:
        break;

    case OTA_UI_SUCCESS:
        if (!ctx->state_runtime.ota.phase_voice_played) {
            ctx->state_runtime.ota.phase_voice_played =
                voice_manager_play_interrupt(VOICE_UPGRADESUCCEEDED);
        }
        break;

    case OTA_UI_FAIL:
        if (!ctx->state_runtime.ota.phase_voice_played) {
            ctx->state_runtime.ota.phase_voice_played =
                voice_manager_play_interrupt(VOICE_UPGRADEFAILED);
        }
        break;

    case OTA_UI_NONE:
    default:
        ota_clear_runtime(ctx);
        return ST_READY;
    }

    return ST_OTA;
}
