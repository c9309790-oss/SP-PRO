#ifndef SP_PRO_APP_CONTEXT_H
#define SP_PRO_APP_CONTEXT_H

#include <stdint.h>
#include <stdbool.h>
#include "controller_status_types.h"
#include "service_domain_types.h"
#include "sp_pro_app_types.h"

#define KEY_COUNT 10

typedef struct {
    struct {
        app_state_t state;
        app_state_t prev_state;
        brew_substate_t substate;
    } core;

    /* Snapshot mirrored from the latest controller status frame. */
    MACHINE_STATUS ms;
    uint32_t error_code;

    struct {
        uint8_t progress;
        uint32_t start_tick;
        uint32_t elapsed_tick;
        uint32_t finish_tick;
        uint32_t target_time;
        uint16_t target_ml;
        uint16_t steam_level;
        drink_type_t target_drink;
        uint8_t marquee_step;
        uint8_t marquee_tick;
        float target_temp;
        float current_temp;
    } drink;

    struct {
        bool active;
        uint8_t step;
        uint16_t tick;
        uint16_t interval;
    } anim;

    struct {
        uint16_t key_down;
        uint16_t key_pressed;
        uint16_t key_long;
        uint16_t key_combo;
        uint16_t combo_lock_mask;
        uint16_t long_lock_mask;
        uint16_t key_time[KEY_COUNT];
    } input;

    struct {
        ctrl_src_t src;
        const formula_info_t *formula;
        formula_info_t remote_formula;
        drink_type_t request_drink;
        uint32_t cmd_seq;
        bool busy;
    } ctrl;

    /* Use 32-bit counters to avoid wraparound during long uptime. */
    struct {
        uint32_t tick;
        uint32_t operation_timer;
        uint32_t standby_timer;
        uint32_t alarm_timer;
        uint32_t clean_timer;
        uint32_t auto_save_timeout;
    } timer;

    struct {
        float esp_brew_w;
        float esp_brew_t;
        float ame_brew_w;
        float ame_brew_t;
        float ame_water_w;
        float ame_water_t;
        float cold_brew_w;
        float hot_water_w;
        float hot_water_t;
        float grind_w;
        float clean_v;
        float steam_level;

        bool active;
        uint8_t drink_type;
        uint8_t param_index;
        float current_val;
        float target_val;
        uint32_t idle_timer;
        uint32_t last_enc_seq;
        uint8_t last_enc_active;
        uint8_t last_enc_param_id;
        encoder_event_t last_enc_evt_type;
        encoder_rotate_t last_enc_rotate;
        float last_enc_value;
        encoder_mode_t current_mode;
        setting_ui_phase_t ui_phase;
        bool formula_dirty;

        setting_substate_t sub;
        setting_water_in_t water_in_mode;
        setting_clear_waterway_t clear_step;
        setting_factory_reset_t reset_step;
        setting_water_hardness_t hardness;
    } setting;

    struct {
        bool active;
        bool is_notice;
        uint8_t major;
        uint8_t sub;
        p_indicator_t notice_p;
        warning_type_t warning;
        maint_type_t notice_type;
    } alarm;

    struct {
        uint8_t enabled;
        lock_hint_t ui_hint;
        uint32_t press_tick;
    } child_lock;

    struct {
        calibration_mode_t mode;
        calibration_step_t step;
        uint32_t step_tick;
    } calibration;

    struct {
        detection_step_t step;
        uint32_t step_tick;
        uint8_t pass_mask;
        uint8_t fail_mask;
    } detection;

    struct {
        clear_bean_step_t step;
        uint32_t step_tick;
    } clear_bean;

    struct {
        maint_type_t type;
        maint_clean_sub_t clean_state;
        maint_brew_sub_t brew_state;
        maint_des_sub_t des_state;
        maint_steam_sub_t steam_state;
        uint8_t resume_stage;
        bool resume_flag;
        uint8_t step;
        uint32_t start_tick;
        uint32_t finish_tick;
        uint32_t maint_tick;
        uint32_t brew_tick;
        uint32_t desc_tick;
        uint32_t steam_tick;
    } maint;

    /* Per-state one-shot flags and working variables. */
    struct {
        struct {
            bool espresso_started;
            bool master_started;
            bool americano_started;
            bool americano_wait_none_started;
            bool americano_water_started;
            bool cold_brew_started;
            bool water_started;
            bool steam_started;
            bool grind_started;
            bool grind_prepare_wait_logged;
            bool grind_allow_without_handle;
            bool prepare_cmd_sent;
            bool grind_notice_pause_sent;
            bool grind_resume_pending;
            bool remote_action_started;
            bool liquid_output_seen;
            bool finish_wait_none_started;
            bool water_idle_seen_after_start;
            float grind_resume_remaining_g;
            float liquid_session_base_ml;
            float display_liquid_base_ml;
            float display_liquid_ml;
            float display_flow_rate_ml_s;
            float water_start_liquid_ml;
            uint32_t grind_resume_pause_tick;
            uint32_t finish_wait_tick;
            uint32_t last_encoder_evt_seq;
            uint32_t last_water_log_sec;
            uint32_t last_steam_log_sec;
            uint8_t last_steam_flag;
            uint8_t water_start_hot_flag;
            drink_exit_reason_t exit_reason;
        } drink;

        struct {
            bool clean_brew_started;
            bool resume_prompt_played;

            bool brew_started;
            maint_brew_sub_t brew_step;
            uint32_t brew_step_tick;
            uint8_t brew_voice_count;
            uint8_t brew_tablet_voice_count;

            bool des_started;
            maint_des_sub_t des_step;
            uint32_t des_step_tick;
            uint8_t des_voice_count;

            bool steam_started;
            maint_steam_sub_t steam_step;
            uint32_t steam_step_tick;
            uint8_t steam_voice_count;

            bool drain_started;
            uint32_t drain_step_tick;
            uint8_t drain_voice_count;
            uint8_t drain_finish_voice_count;
            uint8_t drain_prompt_voice_count;
            uint8_t drain_shutdown_voice_count;
        } maint;

        struct {
            bool water_in_entered;
            bool water_hardness_entered;
            bool factory_reset_entered;
            uint32_t factory_reset_step_tick;
            uint8_t factory_reset_prompt_voice_count;
            uint8_t factory_reset_done_voice_count;
        } setting;

        struct {
            uint8_t steam_click_count;
            uint32_t steam_click_deadline;
            uint32_t steam_not_ready_notice_deadline;
        } ready;

        struct {
            bool entered;
            calibration_step_t last_step;
            bool result_voice_played;
            int32_t powder_adc_baseline;
            int32_t powder_adc_weight_point;
            float powder_weight_baseline;
            bool powder_signal_seen;
            float flow_coeff_base;
            int8_t flow_adjust_percent;
        } calibration;

        struct {
            bool entered;
            detection_step_t last_step;
            bool result_voice_played;
            bool sensor_latched;
            bool transition_armed;
            bool grinder_started;
        } detection;

        struct {
            bool entered;
            bool grinder_started;
        } clear_bean;

        struct {
            bool replenish_cmd_sent;
            bool replenish_activity_seen;
            uint32_t replenish_tick;
            uint32_t phase_tick;
            uint32_t last_diag_tick;
        } power_on;

        struct {
            bool active;
            bool is_notice;
            uint8_t major;
            uint8_t sub;
            p_indicator_t notice_p;
            warning_type_t warning;
            uint32_t error_code;
            uint32_t suppressed_error_mask;
            uint32_t fault_enter_tick;
            bool second_fault_played;
            uint8_t water_pump_retry_count;
            bool water_pump_replenishing;
            uint32_t water_pump_retry_tick;
            bool fault_cancel_sent;
            uint8_t dismissed_maint_notice_mask;
            bool shutdown_pending;
            uint32_t shutdown_tick;
        } alarm;

        struct {
            bool entered;
            ota_ui_substate_t last_phase;
            bool phase_voice_played;
            char dismissed_task_id[100];
        } ota;

        struct {
            bool entered;
            bool wifi_connected_seen;
        } wifi;

        struct {
            bool started;
        } auto_test;
    } state_runtime;

    struct {
        const char *file;
        bool interruptible;
        bool allow_repeat;
        bool force_play;
    } audio;

    struct {
        bool auto_test_enable;
        uint32_t auto_test_start_tick;
        uint8_t auto_test_substate;
    } auto_test;
} app_ctx_t;

extern app_ctx_t g_ctx;

#endif /* SP_PRO_APP_CONTEXT_H */
