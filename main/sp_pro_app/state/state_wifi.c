#include "sp_pro_app_state.h"
#include "sp_pro_app.h"
#include "wifi.h"
#include "ble.h"
#include "ram_diag.h"
#include "system_runtime.h"
#include "esp_log.h"

static const char *TAG = "state_wifi";

static void wifi_restore_app_mode(void)
{
    if (sys_pra.wifi_state == WIFI_CONNECTED) {
        if (sys_pra.mqtt_state == MQTT_CONNECTED ||
            sys_pra.mqtt_state == MQTT_CONNECTING) {
            sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
        } else {
            sys_pra.app_mode = APP_MODE_MQTT_CONFIG;
        }
        return;
    }

    sys_pra.app_mode = APP_MODE_RUNTIME_BRIDGE;
}

static void wifi_clear_runtime(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->state_runtime.wifi.entered = false;
    ctx->state_runtime.wifi.wifi_connected_seen = false;
}

app_state_t state_handle_wifi(app_ctx_t *ctx)
{
    bool wifi_connected;

    if (!ctx) {
        return ST_READY;
    }

    wifi_connected = (sys_pra.wifi_state == WIFI_CONNECTED);

    if (!ctx->state_runtime.wifi.entered) {
        ctx->state_runtime.wifi.entered = true;
        ctx->state_runtime.wifi.wifi_connected_seen = false;

        sys_pra.app_mode = APP_MODE_BT_PAIRING;
        ram_diag_snapshot("wifi_state/enter_before_reprovision");
        wifi_enter_reprovision_mode();
        ctx->state_runtime.wifi.wifi_connected_seen = (sys_pra.wifi_state == WIFI_CONNECTED);
        ram_diag_snapshot("wifi_state/after_reprovision_start");
        ble_app_start_advertising();
        ram_diag_snapshot("wifi_state/after_ble_adv_start");
        ESP_LOGI(TAG, "Enter WIFI pairing flow, connected=%d", wifi_connected);
    }

    /* Re-read the live Wi-Fi state after reprovision mode changes it. */
    wifi_connected = (sys_pra.wifi_state == WIFI_CONNECTED);

    if (ctx->input.key_long & (1U << KEY_WIFI)) {
        ctx->input.key_long &= ~(1U << KEY_WIFI);
        ctx->input.key_pressed &= ~(1U << KEY_WIFI);
        voice_manager_stop();
        ram_diag_snapshot("wifi_state/exit_before_ble_adv_stop");
        ble_app_stop_advertising();
        ram_diag_snapshot("wifi_state/exit_after_ble_adv_stop");
        wifi_exit_reprovision_mode(true);
        ram_diag_snapshot("wifi_state/exit_after_wifi_restore");
        wifi_restore_app_mode();
        wifi_clear_runtime(ctx);
        ESP_LOGI(TAG, "Exit WIFI pairing flow by long press");
        return ST_READY;
    }

    ctx->input.key_pressed = 0;
    ctx->input.key_long = 0;

    if (!ctx->state_runtime.wifi.wifi_connected_seen && wifi_connected) {
        ram_diag_snapshot("wifi_state/connected_before_exit");
        wifi_exit_reprovision_mode(false);
        ram_diag_snapshot("wifi_state/connected_after_wifi_exit");
        wifi_restore_app_mode();
        voice_manager_play_touch_tone();
        ram_diag_snapshot("wifi_state/connected_after_tone_request");
        wifi_clear_runtime(ctx);
        ESP_LOGI(TAG, "WIFI connected, return READY");
        return ST_READY;
    }

    return ST_WIFI;
}
