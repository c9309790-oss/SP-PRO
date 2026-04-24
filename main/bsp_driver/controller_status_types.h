#ifndef CONTROLLER_STATUS_TYPES_H
#define CONTROLLER_STATUS_TYPES_H

#include <stdint.h>

typedef enum {
    STEAM_IDLE = 0,
    STEAM_UNREADY,
    STEAM_READY,
    STEAM_RUNNING,
} STEAM_STATUS_NUM;

typedef enum {
    DRINK_MAKER_NONE = 0,
    DRINK_MAKER_MAINTANCE = 1,
    DRINK_MAKER_FLOW_FIRST = 2,
    DRINK_MAKER_PRESSURE_FIRST = 3,
    DRINK_MAKER_DYNAMIC_MODE = 4,
    DRINK_MAKER_BREW_HEAT_MODE = 5,
    DRINK_MAKER_CALIBRATION_MODE = 6,
} DRINK_MAKER_STATUS;

enum {
    WATER_PUMP_ERROR = (1u << 0),
    WATER_WAY_ERROR = (1u << 1),
    BREW_HEAT_PLATE_ERROR = (1u << 2),
    BREW_HEAT_PLATE_FAST_TEMP_ERROR = (1u << 3),
    BREW_HEAT_PLATE_HIGH_TEMP_ERROR = (1u << 4),
    STEAM_HEAT_PLATE_ERROR = (1u << 5),
    STEAM_HEAT_PLATE_FAST_TEMP_ERROR = (1u << 6),
    STEAM_HEAT_PLATE_HIGH_TEMP_ERROR = (1u << 7),
    LOW_MACHINE_TEMP_ERROR = (1u << 8),
    NTC_COFFEE_ERROR = (1u << 9),
    NTC_FOAM_ERROR = (1u << 10),
    NTC_BREW_ERROR = (1u << 11),
    NTC_STEAM_ERROR = (1u << 12),
    NTC_RELIEF_ERROR = (1u << 13),
    PRESSURE_SIGNAL_ERROR = (1u << 14),
    PRESSURE_VALUE_ERROR = (1u << 15),
    E_FAST_ERROR = (1u << 16),
    BEANBOX_ERROR = (1u << 17),
};

typedef enum {
    ENC_ROT_NONE = 0,
    ENC_ROT_CW,
    ENC_ROT_CCW,
} encoder_rotate_t;

typedef enum {
    ENC_EVT_NONE = 0,
    ENC_EVT_ROTATE,
    ENC_EVT_CLICK,
} encoder_event_t;

typedef struct {
    uint8_t active;
    uint8_t param_id;
    float cur_value;
    encoder_rotate_t rotate;
    encoder_event_t evt_type;
    uint32_t evt_seq;
} ENCODER_STATUS;

typedef struct {
    uint32_t error_code;
    uint8_t ctr_status;
    uint8_t grind_run_flg;
    uint8_t grind_level;
    uint8_t bean_detect_flag;
    uint8_t beanbox_in_place;
    uint8_t grind_handle_postion_flag;
    float flow_rate;
    float pressure;
    float brew_current_temp;
    uint8_t brew_handle_postion_flag;
    uint8_t water_box_shortage_flag;
    uint8_t bin_ready_state;
    uint8_t weight_fluid_flag;
    uint8_t current_stage;
    uint8_t total_stage;
    DRINK_MAKER_STATUS drink_making_flg;
    uint8_t hot_water_flg;
    float hot_current_temp;
    STEAM_STATUS_NUM steam_flag;
    uint8_t steam_level;
    float steam_current_temp;
    float steam_target_temp;
    float milk_target_temp;
    float milk_current_temp;
    int32_t powder_adc;
    int32_t liquid_adc;
    float powder_weight;
    float liquid_weight;
    uint8_t ucFwVersion[16];
    ENCODER_STATUS encoder;
} MACHINE_STATUS;

typedef struct {
    char sn_num[40];
    char model_name[16];
    int mains_frequency;
    float powder_k_value;
    float powder_b_value;
    float liquid_k_value;
    float liquid_b_value;
    float flowmeter_coff;
    int powder_weight_coff;
    int first_powered_on;
    int water_supply_mode;
    int reserved_2;
    int reserved_3;
    int reserved_4;
} FLASH_FACTORY_DATA;

#endif /* CONTROLLER_STATUS_TYPES_H */
