#ifndef SERVICE_DOMAIN_TYPES_H
#define SERVICE_DOMAIN_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_STAGE_NUM 5

typedef enum {
    DRINK_ID_MASTER     = 0,
    DRINK_ID_ESPRESSO   = 1,
    DRINK_ID_AMERICAN   = 2,
    DRINK_ID_COLDBREW   = 3,
    DRINK_ID_WATER      = 5,
} drink_id_t;

typedef struct
{
    uint8_t pressure;
    uint16_t wait_time;
} pressure_stage_t;

typedef struct
{
    uint16_t flow_velocity;
    uint16_t wait_time;
} velocity_stage_t;

typedef struct
{
    uint8_t status;
    uint16_t flow_velocity;
    uint16_t wait_time;
    uint16_t water_volume;
} prebrew_t;

typedef struct
{
    uint32_t record_id;
    uint32_t formula_id;

    char formula_name[32];
    char formula_remark[64];
    char support_mode[16];

    uint8_t drink_id;
    char drink_name[32];

    uint8_t label_id;
    char label[16];

    uint8_t grind_range;
    uint16_t grind_weight;

    uint16_t preset_temperature;
    uint16_t preset_liquid_weight;
    uint16_t water_temperature;
    uint16_t water_weight;
    uint16_t milk_temperature;

    prebrew_t prebrew;
    uint8_t stage_priority;

    pressure_stage_t pressure_stage[MAX_STAGE_NUM];
    uint8_t pressure_stage_cnt;

    velocity_stage_t velocity_stage[MAX_STAGE_NUM];
    uint8_t velocity_stage_cnt;
} formula_info_t;

typedef struct
{
    uint8_t type;
    uint8_t location;
    float value;
} material_item_t;

typedef struct
{
    uint16_t drink_name_index;
    long produce_time;
    int8_t result;
    uint8_t data_type;
    formula_info_t semi_formula;
    material_item_t materials[10];
    uint16_t material_cnt;
} drink_record_t;

typedef struct
{
    char key[32];
    char value[16];
} event_item_t;

typedef struct
{
    event_item_t events[10];
    int event_cnt;
} event_record_t;

typedef struct {
    int version;
    bool force_update;
    formula_info_t *formula_intel_list;
    int formula_intel_list_count;
    formula_info_t *formula_list;
    int formula_list_count;
} formula_overall_t;

typedef struct {
    int total_grind;
    int total_extraction;
    int steam_time;
    int total_water;
} material_statistics_t;

typedef struct {
    int breawing_head_cleaning_count;
    int descaling_count;
    int steam_pole_cleaning_count;
} maintain_statistics_t;

typedef struct {
    char formula_name[64];
    int drink_id;
    int drink_count;
} beverage_data_t;

typedef struct {
    int period;
    beverage_data_t *data;
    int data_count;
} beverage_period_data_t;

typedef struct {
    beverage_period_data_t *data;
    int data_count;
    int total;
} beverage_statistics_t;

typedef struct {
    int back_wash_count;
    int back_wash_total_count;
    int steam_pole_cleaning_count;
    int steam_pole_cleaning_total_count;
    int descaling_water_count;
    int descaling_water_total_count;
} clean_statistics_t;

typedef struct {
    int steam_pole_cleaning;
    int back_wash;
    int descaling_water;
} clean_result_info_t;

typedef struct {
    material_statistics_t material_statistics;
    maintain_statistics_t maintain_statistics;
    beverage_statistics_t beverage_statistics;
    clean_statistics_t clean_statistics;
} statistics_info_t;

#endif /* SERVICE_DOMAIN_TYPES_H */
