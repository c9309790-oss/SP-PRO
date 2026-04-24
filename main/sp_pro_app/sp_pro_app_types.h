#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "controller_status_types.h"
#include "sp_pro_proto.h"

/* High-level application states shared across app/service layers. */
typedef enum {
    ST_OFF,
    ST_ON,
    ST_READY,
    ST_STANDBY,
    ST_ESPRESSO,
    ST_AMERICANO,
    ST_COLD_BREW,
    ST_WATER,
    ST_STEAM,
    ST_GRIND,
    ST_CLEAR_BEAN,
    ST_LOCK,
    ST_CLEAN_BREW,
    ST_MAINT_BREW,
    ST_MAINT_DES,
    ST_MAINT_STEAM,
    ST_MAINT_DRAIN,
    ST_DRINK_SET,
    ST_SETTING,
    ST_CALIBRATION,
    ST_DETECTION,
    ST_ALARM,
    ST_WIFI,
    ST_OTA,
    ST_AUTO_TEST,
    ST_MASTER,
} app_state_t;

typedef enum {
    MAINT_TYPE_NONE = 0,
    MAINT_TYPE_BREW,
    MAINT_TYPE_DES,
    MAINT_TYPE_STEAM,
} maint_type_t;

/* Controller actions sent to the coffee machine. */
typedef enum {
    CTRL_ACT_NONE = 0,
    CTRL_ACT_ESPRESSO,
    CTRL_ACT_COLD_BREW,
    CTRL_ACT_HOT_WATER,
    CTRL_ACT_STEAM_START,
    CTRL_ACT_STEAM_STOP,
    CTRL_ACT_GRIND_START,
    CTRL_ACT_GRIND_PAUSE,
    CTRL_ACT_GRIND_CONTINUE,
    CTRL_ACT_GRIND_STOP,
    CTRL_ACT_CHILD_LOCK_ON,
    CTRL_ACT_CHILD_LOCK_OFF,
    CTRL_ACT_CLEAN_BREW,
    CTRL_ACT_MAINT_BREW1,
    CTRL_ACT_MAINT_BREW2,
    CTRL_ACT_MAINT_DES1,
    CTRL_ACT_MAINT_DES2,
    CTRL_ACT_MAINT_STEAM,
    CTRL_ACT_MAINT_STOP,
    CTRL_ACT_WIFI_ON,
    CTRL_ACT_WIFI_OFF,
    CTRL_ACT_CANCEL,
    CTRL_ACT_READ_STATUS,
    CTRL_ACT_OTA_BOOT,
    CTRL_ACT_DRAIN,
    CTRL_ACT_POWER,
    CTRL_ACT_STANDBY,
    CTRL_ACT_STEAM_SET_NORMAL,
    CTRL_ACT_AMERICANO_BREW,
    CTRL_ACT_AMERICANO_WATER,
    CTRL_ACT_FACTORY_WRITE,
    CTRL_ACT_FACTORY_READ
} control_action_t;

typedef struct {
    float esp_brew_w;
    float esp_brew_t;
    float ame_brew_w;
    float ame_brew_t;
    float ame_water_w;
    float ame_water_t;
    float cold_brew_w;
    float grind_w;
} app_beverage_settings_t;

typedef struct {
    float display_offset_ml;
    float stop_ahead_ml;
} drink_liquid_compensation_t;

/* Brewing sub-states shared by multiple drink flows. */
typedef enum {
    BREW_SUB_PREPARE = 1,
    BREW_SUB_REMOTE_COUNTDOWN,
    BREW_SUB_RUNNING_1,
    BREW_SUB_RUNNING_2,
    BREW_SUB_FINISH,
    BREW_SUB_WAIT_TAIL_SPRAY,
    BREW_SUB_TAIL_SPRAY_COUNTDOWN,
    BREW_SUB_TAIL_SPRAY_RUNNING,
} brew_substate_t;

typedef enum {
    POWER_ON_SUB_FLASH = 100,
    POWER_ON_SUB_REPLENISH,
    POWER_ON_SUB_LOADING,
} power_on_substate_t;

typedef enum {
    OTA_UI_NONE = 0,
    OTA_UI_REMINDER = 200,
    OTA_UI_UPGRADING,
    OTA_UI_SUCCESS,
    OTA_UI_FAIL,
} ota_ui_substate_t;

typedef enum {
    MAINT_CLEAN_SUB_IDLE = 0,
    MAINT_CLEAN_SUB_RUN,
    MAINT_CLEAN_SUB_FINISH,
} maint_clean_sub_t;

typedef enum {
    BREW_STEP_ADD_WATER = 0,
    BREW_STEP_ADD_POWDER,
    BREW_STEP_WAIT_HANDLE,
    BREW_STEP_WATER_LACK,
    BREW_STEP_WAIT_CLICK1,
    BREW_STEP_WAIT_CLEAN1,
    BREW_STEP_REMOVE_HANDLE,
    BREW_STEP_WAIT_CLICK2,
    BREW_STEP_WAIT_CLEAN2,
    BREW_STEP_FINISH
} maint_brew_sub_t;

typedef enum {
    DES_STEP_ADD_WATER = 0,
    DES_STEP_SWITCH_WATER_MODE,
    DES_STEP_WAIT_TABLET,
    DES_STEP_ADD_POWDER,
    DES_STEP_WATER_LACK,
    DES_STEP_WAIT_CLICK1,
    DES_STEP_WAIT_CLEAN1,
    DES_STEP_CHANGE_WATER,
    DES_STEP_WAIT_CLICK2,
    DES_STEP_WAIT_CLEAN2,
    DES_STEP_FINISH
} maint_des_sub_t;

typedef enum {
    STEAM_STEP_ADD_WATER = 0,
    STEAM_STEP_ADD_POWDER,
    STEAM_STEP_SOAK1,
    STEAM_STEP_WATER_LACK,
    STEAM_STEP_WAIT_CLICK1,
    STEAM_STEP_WAIT_CLEAN1,
    STEAM_STEP_WASH_PITCHER,
    STEAM_STEP_SOAK2,
    STEAM_STEP_WAIT_CLICK2,
    STEAM_STEP_WAIT_CLEAN2,
    STEAM_STEP_FINISH
} maint_steam_sub_t;

typedef enum {
    MAINT_NOTICE_NONE = 0,
    MAINT_NOTICE_BREW,
    MAINT_NOTICE_DES,
    MAINT_NOTICE_STEAM,
} maint_notice_t;

/* Reserved for persisted maintenance progress recovery. */
typedef enum {
    MAINT_PROGRESS_NONE = 0,
    MAINT_PROGRESS_PREPARE,
    MAINT_PROGRESS_RUN1,
    MAINT_PROGRESS_RUN2,
} maint_progress_t;

/* Voice prompt identifiers. */
typedef enum {
    VOICE_NONE = 0,

    /* Child lock. */
    VOICE_PRSCHILDLOCK3S2UNLOCK,

    /* Safety prompts. */
    VOICE_CAUTIONHOTSTEAM,

    /* Drink flow. */
    VOICE_CANCELMAKECOFFEE,

    /* Basic alerts. */
    VOICE_ALERTBEANBINDISPLACED,
    VOICE_KNOBATUNLOCKEDPOSITION,
    VOICE_PORTAFILTERNOTPLACED,

    /* Steam maintenance. */
    VOICE_CLEANSTEAMWAND,
    VOICE_ADDWATERTOCLEANINGLINE,
    VOICE_ADDCLEANPOWDER,
    VOICE_SOAKSTEAMNOZZLE,
    VOICE_KEY,
    VOICE_POWERON,
    VOICE_POWEROFF,
    VOICE_CLICKCLEANBUTTON,
    VOICE_WASHMILKPITCHER,
    VOICE_SECONDSOAKSTEAMNOZZLE,
    VOICE_CLEANFINISHED,
    VOICE_CONTINUECLEANAFTERPOWEROFF,

    /* Brew-head maintenance. */
    VOICE_CLEANFRONTBREWUNIT,
    VOICE_TANKMODEWATERADDREMINDER,
    VOICE_BUCKETMODEWATERADDREMINDER,
    VOICE_PUTCLEANINGPOWDER,
    VOICE_PROMPTHANDLEFITTING,
    VOICE_TAKEOFFHANDLEANDWASH,
    VOICE_FINISHANDREMINDTODUMP,
    VOICE_INSTALLHANDLE,

    /* Descaling. */
    VOICE_DESCALING,
    VOICE_SWITCHWATERTANKMODEREMIND,
    VOICE_ADDWATERANDDESCALINGPOWDER,
    VOICE_PROMPTPLACECONTAINER,
    VOICE_CHANGEWATER,
    VOICE_FINISHANDREMINDDUMPING,

    /* Settings. */
    VOICE_CURRENTWATERTANKMODE,
    VOICE_CURRENTWATERBUCKETMODE,
    VOICE_WATERTANKMODE,
    VOICE_WATERBUCKETMODE,
    VOICE_EXITSWITCHINLETMODE,
    VOICE_SWITCHWATERHARDNESSLEVELA,
    VOICE_SWITCHWATERHARDNESSLEVELB,
    VOICE_SWITCHWATERHARDNESSLEVELC,
    VOICE_WATERHARDNESSLEVELA,
    VOICE_WATERHARDNESSLEVELB,
    VOICE_WATERHARDNESSLEVELC,

    /* IoT upgrade prompts. */
    VOICE_UPGRADEREMINDER,
    VOICE_UPGRADESUCCEEDED,
    VOICE_UPGRADEFAILED,

    /* Warnings. */
    VOICE_TANKMODEALERT,
    VOICE_BUCKETMODEALERT,
    VOICE_EMPTYWATERTRAY,
    VOICE_FACTORYRESTOREWARNING,
    VOICE_AUTOPOWEROFF,

    /* Factory reset. */
    VOICE_CONFIRMFACTORYRESET,
    VOICE_FACTORYRESETCOMPLETED,

    /* Fault prompts. */
    VOICE_SYSTEMLACKSWATER,
    VOICE_WATERPUMPFAULT,
    VOICE_FIRSTFAULTWARNING,
    VOICE_SECONDFAULTWARNING,
    VOICE_LOWTEMPERATUREERROR,
    VOICE_PRESSURESENSORFAULTWARNING,
    VOICE_BEANHOPPERWARNING,

    /* Calibration. */
    VOICE_PLACEGROUNDCUP,
    VOICE_PLACECALIBRATIONWEIGHT,
    VOICE_CALIBRATIONCOMPLETED,
    VOICE_HOTDRINKCALIBRATION,
    VOICE_HOTWATERCALIBRATION,

    /* Detection and fixture flow. */
    VOICE_PLACEPORTAFILTERONSTAND,
    VOICE_REMOVEPORTAFILTER,
    VOICE_REMOVEWATERTANK,
    VOICE_PUTBACKWATERTANK,
    VOICE_REMOVEBEANHOPPER,
    VOICE_PUTBACKBEANHOPPER,
    VOICE_UNLOCKHOPPER,
    VOICE_DETECTIONPASSED,
    VOICE_DETECTIONABNORMAL,

    /* Additional prompts. */
    VOICE_GRINDINGWITHOUTHANDLE,
    VOICE_STEAMNOTREADY,
    VOICE_FILLWATERTOMAX,
    VOICE_BEANHOPPERMISSING,
    VOICE_FILLCOFFEEBEANS,
    VOICE_WEIGHTABNORMAL,
    VOICE_BEANHOPPERMISSING_FILLWATERTOMAX,

    VOICE_MAX
} voice_id_t;

typedef enum {
    VOICE_PRIO_LOW = 0,
    VOICE_PRIO_NORMAL,
    VOICE_PRIO_HIGH,
    VOICE_PRIO_CRITICAL,
} voice_priority_t;

/* Logical key events used by the app state machine. */
typedef enum {
    KEY_ESPRESSO = 0,
    KEY_AMERICANO,
    KEY_COLD_BREW,
    KEY_WATER,
    KEY_STEAM,
    KEY_GRIND,
    KEY_TEMP,
    KEY_CHILD,
    KEY_CLEAN,
    KEY_WIFI,
    KEY_BACK,
    KEY_KNOB_CLICK,
    KEY_START,
    KEY_MAX
} key_event_t;

typedef enum {
    KEY_COMBO_NONE = 0,
    KEY_COMBO_WATER_IN_MODE,
    KEY_COMBO_CLEAR_PIPE,
    KEY_COMBO_FACTORY_RESET,
    KEY_COMBO_WATER_HARDNESS,
    KEY_COMBO_CAL_POWDER,
    KEY_COMBO_CAL_FLOW,
    KEY_COMBO_DETECTION,
    KEY_COMBO_MAINT_BREW,
    KEY_COMBO_MAINT_DES,
    KEY_COMBO_MAINT_STEAM,
} key_combo_id_t;

typedef enum {
    PARAM_NONE,
    PARAM_VOLUME,
    PARAM_TEMPERATURE,
} param_type_t;

#define KEY_LONG_TICKS     40
#define KEY_BIT(key_id)    (1U << ((key_id) - BF_KEY_K1))

#define Q1_GAUGE_MAX_VAL   17
#define Q2_GAUGE_MAX_VAL   25

#define CMD_BUFFER_SIZE    256
#define PRESSURE_MAX_VAL   12.0f
#define LOCK_BLINK_TICKS   10

#define LOGIC_TASK_MS      50

#define STAY_TICKS(ms)     ((ms) / LOGIC_TASK_MS)

#define STAY_TICKS_1S      STAY_TICKS(1000)
#define STAY_TICKS_2S      STAY_TICKS(2000)
#define STAY_TICKS_3S      STAY_TICKS(3000)
#define STAY_TICKS_4S      STAY_TICKS(4000)
#define STAY_TICKS_15S     STAY_TICKS(15000)

#define LOCK_HOLD_TICKS    STAY_TICKS_2S

#define STEAM_SOAK1_TICKS  STAY_TICKS(30000)
#define STEAM_SOAK2_TICKS  STAY_TICKS(30000)

typedef enum {
    DRINK_ESPRESSO = 0,
    DRINK_AMERICANO,
    DRINK_COLD_BREW,
    DRINK_WATER,
    DRINK_STEAM,
    DRINK_GRIND,
    DRINK_MASTER
} drink_type_t;

typedef enum {
    DRINK_EXIT_NONE = 0,
    DRINK_EXIT_CANCEL,
    DRINK_EXIT_FAIL,
    DRINK_EXIT_SWITCH_TO_STEAM,
} drink_exit_reason_t;

typedef enum {
    CLEAR_BEAN_STEP_NONE = 0,
    CLEAR_BEAN_STEP_DISPLACED,
    CLEAR_BEAN_STEP_UNLOCK_HINT,
    CLEAR_BEAN_STEP_WAIT_RUN_DELAY,
    CLEAR_BEAN_STEP_WAIT_HANDLE,
    CLEAR_BEAN_STEP_RUNNING,
    CLEAR_BEAN_STEP_DONE,
} clear_bean_step_t;

typedef enum {
    SET_SUB_WATER_IN = 1,
    SET_SUB_CLEAR_WATERWAY,
    SET_SUB_FACTORY_RESET,
    SET_SUB_WATER_HARDNESS,
} setting_substate_t;

typedef enum {
    CAL_MODE_NONE = 0,
    CAL_MODE_POWDER,
    CAL_MODE_HOT_DRINK,
    CAL_MODE_HOT_WATER,
} calibration_mode_t;

typedef enum {
    CAL_STEP_NONE = 0,
    CAL_STEP_POWDER_PLACE_CUP,
    CAL_STEP_POWDER_WAIT_WEIGHT,
    CAL_STEP_POWDER_WAIT_START,
    CAL_STEP_FLOW_SELECT,
    CAL_STEP_RUNNING,
    CAL_STEP_DONE,
    CAL_STEP_FAIL,
} calibration_step_t;

typedef enum {
    DET_STEP_NONE = 0,
    DET_STEP_PLACE_PORTAFILTER,
    DET_STEP_REMOVE_PORTAFILTER,
    DET_STEP_REMOVE_WATER_TANK,
    DET_STEP_PUTBACK_WATER_TANK,
    DET_STEP_REMOVE_BEAN_HOPPER,
    DET_STEP_PUTBACK_BEAN_HOPPER,
    DET_STEP_UNLOCK_HOPPER,
    DET_STEP_CLEAR_BEAN_RUNNING,
    DET_STEP_REMOVE_BEAN_HOPPER_AFTER_CLEAR,
    DET_STEP_HANDLE_FIT,
    DET_STEP_REMOVE_PORTAFILTER_FINAL,
    DET_STEP_PASS,
    DET_STEP_FAIL,
} detection_step_t;

typedef enum {
    DET_RESULT_GRIND_HANDLE = (1U << 0),
    DET_RESULT_WATER_TANK   = (1U << 1),
    DET_RESULT_BEAN_HOPPER  = (1U << 2),
    DET_RESULT_CLEAR_BEAN   = (1U << 3),
    DET_RESULT_BREW_HANDLE  = (1U << 4),
} detection_result_flag_t;

typedef enum {
    WATER_IN_MODE_NONE = 0,
    WATER_IN_MODE_TANK,
    WATER_IN_MODE_BUCKET
} setting_water_in_t;

typedef enum {
    CLEAR_STEP_NONE = 0,
    CLEAR_STEP_CUTOFF_SUPPLY,
    CLEAR_STEP_CLEARING,
    CLEAR_STEP_CLEARING_DONE,
    CLEAR_STEP_FACTORY_RESET,
    CLEAR_STEP_WILL_SHUTDOWN,
    CLEAR_STEP_NOW_SHUTDOWN
} setting_clear_waterway_t;

typedef enum {
    RESET_STEP_NONE = 0,
    RESET_STEP_DOUBLE_CONFIRM,
    RESET_STEP_DONE_AND_SHUTDOWN
} setting_factory_reset_t;

typedef enum {
    HARDNESS_LEVEL_A,
    HARDNESS_LEVEL_B,
    HARDNESS_LEVEL_C
} setting_water_hardness_t;

typedef enum {
    WARN_NONE = 0,
    WARN_WATER_EMPTY,
    WARN_BEAN_EMPTY,
    WARN_TRAY_MISS,
    WARN_BEAN_MISS,
    WARN_LIQUID_ABNM,
    WARN_BREW_HD_MISS,
    WARN_GRIND_HD_MISS,
    WARN_STEAM_NOT_READY,
    WARN_WATER_BEAN_MISS,
} warning_type_t;

typedef enum {
    BF_STATUS_NO,
    BF_STATUS_P1,
    BF_STATUS_P2,
    BF_STATUS_P3,
    BF_STATUS_P4,
    BF_STATUS_P5,
} p_indicator_t;

typedef enum {
    BF_KEY_K1 = 0,
    BF_KEY_K2,
    BF_KEY_K3,
    BF_KEY_K4,
    BF_KEY_K5,
    BF_KEY_K6,
    BF_KEY_K7,
    BF_KEY_K8,
    BF_KEY_K9,
    BF_KEY_K10,
    BF_KEY_COUNT
} key_id_t;

typedef enum {
    BF_INDICATOR_L1,
    BF_INDICATOR_L2,
    BF_INDICATOR_L3,
    BF_INDICATOR_L4,
    BF_INDICATOR_COUNT
} l_indicator_t;

typedef enum {
    BF_UNIT_S1,
    BF_UNIT_S2,
    BF_UNIT_S3,
    BF_UNIT_S4,
    BF_UNIT_S5,
    BF_UNIT_S6,
    BF_UNIT_S7,
    BF_UNIT_S8,
    BF_UNIT_S9,
    BF_UNIT_S10,
    BF_UNIT_S11,
    BF_UNIT_COUNT
} unit_indicator_t;

typedef enum {
    BF_POS_LEFT,
    BF_POS_RIGHT,
    BF_POS_COUNT
} pos_indicator_t;

typedef enum {
    KEY_MODE_ALL_OFF,
    KEY_MODE_READY,
    KEY_MODE_SINGLE,
    KEY_MODE_SINGLE_BLINK,
    KEY_MODE_COMBO_BLINK,
    KEY_MODE_CHILD_LOCK,
} key_display_mode_t;

typedef enum {
    SET_UI_PHASE_STATIC = 0,
    SET_UI_PHASE_HINT,
    SET_UI_PHASE_ADJUST,
} setting_ui_phase_t;

typedef enum {
    DISP_RENDER_STATIC,
    DISP_RENDER_BLINK,
} disp_render_mode_t;

typedef enum {
    DISP_KIND_WEIGHT,
    DISP_KIND_TEMP,
    DISP_KIND_LEVEL,
} disp_kind_t;

typedef enum {
    DISP_NO_LOCK,
    DISP_CHILD_LOCKED,
    DISP_CHILD_UNLOCK,
} lock_hint_t;

typedef enum {
    ENCODER_MODE_IDLE = 0,
    ENCODER_MODE_ESP_BREW_WEIGHT,
    ENCODER_MODE_ESP_BREW_TEMP,
    ENCODER_MODE_AME_BREW_WEIGHT,
    ENCODER_MODE_AME_WATER_WEIGHT,
    ENCODER_MODE_AME_BREW_TEMP,
    ENCODER_MODE_AME_WATER_TEMP,
    ENCODER_MODE_COLD_BREW_WEIGHT,
    ENCODER_MODE_HOT_WATER_WEIGHT,
    ENCODER_MODE_HOT_WATER_TEMP,
    ENCODER_MODE_GRIND_WEIGHT,
    ENCODER_MODE_CLEAN_VOLUME,
    ENCODER_MODE_STEAM_LEVEL,
    ENCODER_MODE_WATER_IN,
    ENCODER_MODE_WATER_HARDNESS,
    ENCODER_MODE_MAX
} encoder_mode_t;

typedef enum {
    CTRL_SRC_UI = 0,
    CTRL_SRC_MQTT,
    CTRL_SRC_AUTOTEST,
    CTRL_SRC_RECOVERY
} ctrl_src_t;

typedef struct {
    uint8_t count;
    const encoder_mode_t *modes;
} sp_pro_param_map_t;

typedef struct {
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
    float current_val;
    float target_val;
    encoder_mode_t current_mode;
    setting_ui_phase_t ui_phase;
    setting_substate_t sub;
    setting_water_in_t water_in_mode;
    setting_clear_waterway_t clear_step;
    setting_factory_reset_t reset_step;
    setting_water_hardness_t hardness;
} app_setting_view_t;

typedef struct {
    uint32_t elapsed_tick;
    uint16_t target_ml;
    uint16_t secondary_target_ml;
    drink_type_t target_drink;
    float target_temp;
    float secondary_target_temp;
    float display_liquid_ml;
    uint8_t countdown_seconds;
    bool remote_active;
    bool tail_spray_pending;
    bool tail_spray_running;
} app_drink_view_t;

typedef struct {
    clear_bean_step_t step;
} app_clear_bean_view_t;

typedef struct {
    bool active;
    bool is_notice;
    uint8_t major;
    uint8_t sub;
    p_indicator_t notice_p;
    warning_type_t warning;
    uint8_t retry_count;
} app_alarm_view_t;

typedef struct {
    calibration_mode_t mode;
    calibration_step_t step;
    float flow_coeff_base;
    float flow_coeff_current;
    int8_t flow_adjust_percent;
} app_calibration_view_t;

typedef struct {
    detection_step_t step;
    uint8_t pass_mask;
    uint8_t fail_mask;
} app_detection_view_t;

typedef struct {
    maint_clean_sub_t clean_state;
    maint_brew_sub_t brew_step;
    maint_des_sub_t des_step;
    maint_steam_sub_t steam_step;
} app_maint_view_t;

typedef struct {
    uint8_t enabled;
    lock_hint_t ui_hint;
} app_child_lock_view_t;

typedef struct {
    uint32_t tick;
    uint8_t anim_step;
    app_state_t state;
    brew_substate_t substate;
    bool network_connected;
    bool network_connecting;
    bool ota_upgrade_prompt;
    bool ota_upgrading;
    bool ota_prompt_dismissed;
    MACHINE_STATUS ms;
    app_drink_view_t drink;
    app_clear_bean_view_t clear_bean;
    app_setting_view_t setting;
    app_calibration_view_t calibration;
    app_detection_view_t detection;
    app_maint_view_t maint;
    app_alarm_view_t alarm;
    app_child_lock_view_t child_lock;
} app_display_view_t;

typedef struct {
    ctrl_src_t src;
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
} app_command_view_t;

typedef struct {
    disp_element_t disp;
    bool dirty;
} disp_model_t;

typedef struct {
    encoder_mode_t mode;
    disp_kind_t kind;
    pos_indicator_t pos;
    uint8_t digits;
    uint16_t unit_mask;
    uint8_t dot_id;
    bool gauge_enable;
    uint16_t gauge_max;
    void (*clear_fn)(disp_element_t *d, pos_indicator_t pos);
} disp_param_desc_t;
