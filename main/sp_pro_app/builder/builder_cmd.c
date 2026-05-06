#include "sp_pro_app_types.h"
#include "sp_pro_app_ctrl.h"
#include "sp_pro_app_context.h"
#include "sp_pro_app_state.h"
#include "ctr_scheduler.h"
#include "controller_status_types.h"
#include "factory_cfg.h"
#include "uart_ctr.h"
#include <stddef.h>
#include <stdio.h>
#include "esp_log.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

static const char *TAG = "cmd_builder";
extern FLASH_FACTORY_DATA factory_data;

#define FACTORY_WRITE_MODEL_FALLBACK      "SP_PRO"
#define FACTORY_WRITE_MAINS_FREQ_FALLBACK FACTORY_MAINS_PROFILE_220V_50HZ_CCC
#define FACTORY_WRITE_LIQUID_SCALE_ENABLED 0
#define FACTORY_WRITE_MAX_ABS_FLOAT       1000000.0f
#define FACTORY_WRITE_FLOWMETER_DEFAULT   0.45f

typedef bool (*ctrl_cmd_formatter_t)(const app_command_view_t *view,
                                     char *buf,
                                     uint16_t len,
                                     void *param);

typedef struct {
    control_action_t action;
    ctrl_cmd_formatter_t ui_formatter;
    ctrl_cmd_formatter_t mqtt_formatter;
} ctrl_cmd_desc_t;

#define CTRL_CMD_DESC_SPLIT(action_, ui_fmt_, mqtt_fmt_) { action_, ui_fmt_, mqtt_fmt_ }
#define CTRL_CMD_DESC_SHARED(action_, fmt_)             { action_, fmt_, NULL }
#define MQTT_FORMULA_STAGE_PRIORITY_PRESSURE_FIRST     0
#define MQTT_PRESSURE_FIRST_FIXED_FLOW                 3

static bool factory_write_float_valid(float value)
{
    return isfinite(value) && fabsf(value) <= FACTORY_WRITE_MAX_ABS_FLOAT;
}

typedef struct {
    float temperature;
    int frequency;
    int freq_cut_water;
} steam_level_param_t;

typedef enum {
    MQTT_BREW_PROFILE_NONE = 0,
    MQTT_BREW_PROFILE_PRESSURE_FIRST,
} mqtt_brew_profile_t;

/* Steam parameter presets indexed by UI steam level. */
static const steam_level_param_t steam_param[] = {
    { 160.0f, 6, 1 },
    { 168.0f, 11, 2 }
};

static const steam_level_param_t *steam_param_from_level(uint8_t level)
{
    if (level < 1U || level > (sizeof(steam_param) / sizeof(steam_param[0]))) {
        return NULL;
    }

    return &steam_param[level - 1U];
}

static ctrl_cmd_formatter_t ctrl_cmd_select_formatter(const ctrl_cmd_desc_t *desc, ctrl_src_t src)
{
    ctrl_cmd_formatter_t formatter = (src == CTRL_SRC_UI) ? desc->ui_formatter : desc->mqtt_formatter;

    if (!formatter) {
        formatter = (src == CTRL_SRC_UI) ? desc->mqtt_formatter : desc->ui_formatter;
    }

    return formatter;
}

static bool cmd_buf_append(char *buf, uint16_t len, size_t *offset, const char *fmt, ...)
{
    va_list args;
    int written;

    if (!buf || !offset || *offset >= len) {
        return false;
    }

    va_start(args, fmt);
    written = vsnprintf(buf + *offset, len - *offset, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= (len - *offset)) {
        return false;
    }

    *offset += (size_t)written;
    return true;
}

static int formula_total_stage_count(const formula_info_t *formula)
{
    if (!formula) {
        return 0;
    }

    return formula->pressure_stage_cnt;
}

static bool formula_has_profile_segments(const formula_info_t *formula)
{
    return formula && formula_total_stage_count(formula) > 0;
}

static int formula_brew_temperature(const formula_info_t *formula)
{
    if (!formula) {
        return 0;
    }

    if (formula->preset_temperature > 0) {
        return formula->preset_temperature;
    }

    if (formula->water_temperature > 0) {
        return formula->water_temperature;
    }

    return (formula->drink_id == DRINK_ID_COLDBREW) ? 0 : 93;
}

static int formula_final_liquid_weight(const formula_info_t *formula)
{
    if (!formula) {
        return 0;
    }

    if (formula->preset_liquid_weight > 0) {
        return formula->preset_liquid_weight;
    }

    if (formula->water_weight > 0) {
        return formula->water_weight;
    }

    return 0;
}

static int formula_stage_flow_velocity(const formula_info_t *formula, int index)
{
    if (!formula || index < 0 || index >= formula->pressure_stage_cnt) {
        return 0;
    }

    return MQTT_PRESSURE_FIRST_FIXED_FLOW;
}

static int formula_stage_wait_time(const formula_info_t *formula, int index)
{
    if (!formula || index < 0) {
        return 0;
    }

    if (index < formula->pressure_stage_cnt && formula->pressure_stage[index].wait_time > 0) {
        return formula->pressure_stage[index].wait_time;
    }

    return 0;
}

static int formula_stage_pressure_value(const formula_info_t *formula, int index)
{
    if (!formula || index < 0) {
        return 9;
    }

    if (index < formula->pressure_stage_cnt && formula->pressure_stage[index].pressure > 0) {
        return (int)(formula->pressure_stage[index].pressure + 0.5f);
    }

    return 9;
}

static mqtt_brew_profile_t formula_detect_brew_profile(const formula_info_t *formula)
{
    int stage_count;

    if (!formula) {
        return MQTT_BREW_PROFILE_NONE;
    }

    stage_count = formula_total_stage_count(formula);
    if (stage_count <= 0) {
        return MQTT_BREW_PROFILE_NONE;
    }

    return (formula->stage_priority == MQTT_FORMULA_STAGE_PRIORITY_PRESSURE_FIRST)
               ? MQTT_BREW_PROFILE_PRESSURE_FIRST
               : MQTT_BREW_PROFILE_NONE;
}

static bool fmt_mqtt_formula_brew_profile(const formula_info_t *formula, char *buf, uint16_t len)
{
    mqtt_brew_profile_t profile;
    size_t offset = 0;
    int brew_temp;
    int final_liquid;
    int stage_count;

    if (!formula || !buf || len == 0U) {
        return false;
    }

    profile = formula_detect_brew_profile(formula);
    if (profile == MQTT_BREW_PROFILE_NONE) {
        return false;
    }

    brew_temp = formula_brew_temperature(formula);
    final_liquid = formula_final_liquid_weight(formula);
    stage_count = formula_total_stage_count(formula);

    switch (profile)
    {
    case MQTT_BREW_PROFILE_PRESSURE_FIRST:
    {
        const char *cmd_name = "PRESSURE_FIRST";
        const char *seg_name = "PRESSURE";

        if (stage_count <= 0 ||
            !cmd_buf_append(buf, len, &offset, "123@%s@TOTAL=%d", cmd_name, stage_count)) {
            return false;
        }

        if (!cmd_buf_append(buf, len, &offset, "|PRE_INFUSION=%u,%u,%u,%u",
                            formula->prebrew.status ? 1U : 0U,
                            formula->prebrew.water_volume,
                            formula->prebrew.flow_velocity,
                            formula->prebrew.wait_time)) {
            return false;
        }

        for (int i = 0; i < stage_count; i++) {
            int stage_time = formula_stage_wait_time(formula, i);
            int stage_flow = formula_stage_flow_velocity(formula, i);
            int stage_pressure = formula_stage_pressure_value(formula, i);
            bool is_last_stage = (i == stage_count - 1) && final_liquid > 0;

            if (stage_time <= 0 || stage_pressure <= 0 || (!is_last_stage && stage_flow <= 0)) {
                ESP_LOGE(TAG,
                         "Invalid formula stage for %s, idx=%d time=%d flow=%d pressure=%d final=%d",
                         cmd_name,
                         i,
                         stage_time,
                         stage_flow,
                         stage_pressure,
                         final_liquid);
                return false;
            }

            if (is_last_stage) {
                if (!cmd_buf_append(buf, len, &offset, "|%s=%d,%d,%d,%d,%d",
                                    seg_name,
                                    i + 1,
                                    stage_time,
                                    stage_pressure,
                                    final_liquid,
                                    brew_temp)) {
                    return false;
                }
            } else {
                if (!cmd_buf_append(buf, len, &offset, "|%s=%d,%d,%d,%d,%d",
                                    seg_name,
                                    i + 1,
                                    stage_time,
                                    stage_flow,
                                    stage_pressure,
                                    brew_temp)) {
                    return false;
                }
            }
        }

        return cmd_buf_append(buf, len, &offset, "#123");
    }

    default:
        return false;
    }
}

/* Espresso. */
static bool fmt_ui_espresso(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    return view && snprintf(buf, len,
                            "123@FREE_PRESSURE@FREE_PRESSURE=%d,%d,%.1f#123",
                            (int)view->esp_brew_w,
                            9,
                            view->esp_brew_t) > 0;
}

static bool fmt_mqtt_espresso(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    formula_info_t *formula = (formula_info_t *)param;
    if (!formula) {
        return false;
    }

    if (formula_has_profile_segments(formula)) {
        return fmt_mqtt_formula_brew_profile(formula, buf, len);
    }

    return snprintf(buf, len,
                    "123@FREE_PRESSURE@FREE_PRESSURE=%d,%d,%.1f#123",
                    (int)formula->preset_liquid_weight,
                    9,
                    (float)formula->preset_temperature) > 0;
}

/* Americano. */
static bool fmt_ui_americano_brew(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    return view && snprintf(buf, len,
                            "123@FREE_PRESSURE@FREE_PRESSURE=%d,%d,%.1f#123",
                            (int)view->ame_brew_w,
                            9,
                            view->ame_brew_t) > 0;
}

static bool fmt_ui_americano_water(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    return view && snprintf(buf, len,
                            "123@RUN@HEAT_COFFEE=%.1f,0,0|VALVE=1|PUMP_COFFEE=0,0,1,%d,6,0#123",
                            view->ame_water_t,
                            (int)view->ame_water_w) > 0;
}

static bool fmt_mqtt_americano_brew(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    formula_info_t *formula = (formula_info_t *)param;
    if (!formula) {
        return false;
    }

    if (formula_has_profile_segments(formula)) {
        return fmt_mqtt_formula_brew_profile(formula, buf, len);
    }

    return snprintf(buf, len,
                    "123@FREE_PRESSURE@FREE_PRESSURE=%d,%d,%.1f#123",
                    (int)formula->preset_liquid_weight,
                    9,
                    (float)formula->preset_temperature) > 0;
}

static bool fmt_mqtt_americano_water(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    formula_info_t *formula = (formula_info_t *)param;
    if (!formula) {
        return false;
    }

    return snprintf(buf, len,
                    "123@RUN@HEAT_COFFEE=%.1f,0,0|VALVE=1|PUMP_COFFEE=0,0,1,%d,6,0#123",
                    (float)formula->water_temperature,
                    (int)formula->water_weight) > 0;
}

/* Cold brew. */
static bool fmt_ui_cold_brew(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    return view && snprintf(buf, len,
                            "123@FREE_PRESSURE@FREE_PRESSURE=%d,6,0#123",
                            (int)view->cold_brew_w) > 0;
}

static bool fmt_mqtt_cold_brew(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    formula_info_t *formula = (formula_info_t *)param;
    if (!formula) {
        return false;
    }

    if (formula_has_profile_segments(formula)) {
        return fmt_mqtt_formula_brew_profile(formula, buf, len);
    }

    return snprintf(buf, len,
                    "123@FREE_PRESSURE@FREE_PRESSURE=%d,6,0#123",
                    (int)formula->water_weight) > 0;
}

/* Steam. */
static bool fmt_ui_steam_start(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    if (!view) {
        return false;
    }

    uint8_t level = (uint8_t)(view->steam_level + 0.5f);
    const steam_level_param_t *steam_cfg = steam_param_from_level(level);
    ESP_LOGI(TAG, "steam level %u", level);
    if (!steam_cfg) {
        ESP_LOGE(TAG, "Invalid steam level %u", level);
        return false;
    }

    return snprintf(buf, len,
                    "123@RUN@VALVE=2|HEAT_STEAM=%.1f,1,0|STEAM=%d,%d,90000,1#123",
                    steam_cfg->temperature,
                    steam_cfg->frequency,
                    steam_cfg->freq_cut_water) > 0;
}

static bool fmt_mqtt_steam_start(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    uint8_t level = 1U;
    const steam_level_param_t *steam_cfg;

    if (param) {
        level = *(const uint8_t *)param;
    } else if (view) {
        level = (uint8_t)(view->steam_level + 0.5f);
    }

    steam_cfg = steam_param_from_level(level);
    if (!steam_cfg) {
        ESP_LOGE(TAG, "Invalid MQTT steam level %u", level);
        return false;
    }

    return snprintf(buf, len,
                    "123@RUN@VALVE=2|HEAT_STEAM=%.1f,1,0|STEAM=%d,%d,90000,1#123",
                    steam_cfg->temperature,
                    steam_cfg->frequency,
                    steam_cfg->freq_cut_water) > 0;
}

static bool fmt_steam_stop(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@OUT_STEAM@NULL#123") > 0;
}

/* Hot water. */
static bool fmt_ui_hot_water(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    return view && snprintf(buf, len,
                            "123@RUN@HEAT_COFFEE=%.1f,0,0|VALVE=1|PUMP_COFFEE=0,0,1,%d,2,0#123",
                            view->hot_water_t,
                            (int)view->hot_water_w) > 0;
}

static bool fmt_mqtt_hot_water(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    formula_info_t *formula = (formula_info_t *)param;
    if (!formula) {
        return false;
    }

    if (formula_has_profile_segments(formula)) {
        return fmt_mqtt_formula_brew_profile(formula, buf, len);
    }

    return snprintf(buf, len,
                    "123@RUN@HEAT_COFFEE=%.1f,0,0|VALVE=1|PUMP_COFFEE=0,0,1,%d,2,0#123",
                    (float)formula->water_temperature,
                    (int)formula->water_weight) > 0;
}

/* Grind. */
static bool fmt_ui_grind_start(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    int target_weight;
    float requested_weight = 0.0f;

    if (!view) {
        return false;
    }

    if (param) {
        requested_weight = *(const float *)param;
    } else {
        requested_weight = view->grind_w;
    }

    target_weight = (requested_weight > 0.0f) ? (int)(requested_weight + 0.5f) : 0;
    return snprintf(buf, len,
                    "123@RUN@GRIND=0,%d,0#123",
                    target_weight) > 0;
}

static bool fmt_mqtt_grind_start(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    int target_weight;

    (void)view;
    formula_info_t *formula = (formula_info_t *)param;
    if (!formula) {
        return false;
    }

    target_weight = (formula->grind_weight > 0.0f) ? (int)(formula->grind_weight + 0.5f) : 0;
    return snprintf(buf, len,
                    "123@RUN@GRIND=0,%d,0#123",
                    target_weight) > 0;
}

static bool fmt_grind_pause(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@RUN@GRIND=0,0,1#123") > 0;
}

static bool fmt_grind_continue(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@RUN@GRIND=0,0,2#123") > 0;
}

static bool fmt_grind_stop(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@OUT_GRIND@NULL#123") > 0;
}

/* Cancel and status read. */
static bool fmt_out_null(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@OUT@NULL#123") > 0;
}

static bool fmt_read_all(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@READ@ALL#123") > 0;
}

static bool fmt_ota_boot(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@OTA@NULL#123") > 0;
}

static bool fmt_drain(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@DRAIN@NULL#123") > 0;
}

static bool fmt_power_on(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@POWER_ON@NULL#123") > 0;
}

static bool fmt_steam_set_standby(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@RUN@STEAM_SET=0,90000,60,0#123") > 0;
}

static bool fmt_steam_set_normal(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@RUN@STEAM_SET=0,90000,165,0#123") > 0;
}

static bool fmt_factory_write(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    char sn_buf[FACTORY_CFG_SN_MAX_LEN] = {0};
    char reason[64] = {0};
    const char *factory_prefix;
    const char *sn = factory_data.sn_num;
    const char *model = factory_data.model_name;
    int mains_frequency = factory_data.mains_frequency;
    int written;

    (void)view;
    (void)param;

    if (g_ctx.setting.water_in_mode == WATER_IN_MODE_BUCKET) {
        factory_data.water_supply_mode = 1;
    } else if (g_ctx.setting.water_in_mode == WATER_IN_MODE_TANK) {
        factory_data.water_supply_mode = 0;
    }

    factory_prefix = sn ? strstr(sn, "FACTORY=") : NULL;
    if (factory_prefix) {
        factory_prefix += strlen("FACTORY=");
        if (*factory_prefix != '\0') {
            const char *comma = strchr(factory_prefix, ',');
            size_t sn_len = comma && comma > factory_prefix
                              ? (size_t)(comma - factory_prefix)
                              : strlen(factory_prefix);
            if (comma && comma > factory_prefix) {
                sn_len = (size_t)(comma - factory_prefix);
            }
            if (sn_len >= sizeof(factory_data.sn_num)) {
                sn_len = sizeof(factory_data.sn_num) - 1U;
            }
            memcpy(factory_data.sn_num, factory_prefix, sn_len);
            factory_data.sn_num[sn_len] = '\0';
            sn = factory_data.sn_num;
            ESP_LOGW(TAG, "factory write sanitized SN=%s", sn);
        }
    }

    if (!sn || sn[0] == '\0') {
        if (factory_cfg_get_sn(sn_buf, sizeof(sn_buf))) {
            size_t sn_len = strnlen(sn_buf, sizeof(sn_buf));
            if (sn_len >= sizeof(factory_data.sn_num)) {
                sn_len = sizeof(factory_data.sn_num) - 1U;
            }
            memcpy(factory_data.sn_num, sn_buf, sn_len);
            factory_data.sn_num[sn_len] = '\0';
            sn = factory_data.sn_num;
            ESP_LOGW(TAG, "factory write missing SN, fallback to factory_cfg SN=%s", sn);
        } else {
            ESP_LOGW(TAG, "factory write missing SN, keeping empty field");
            sn = "";
        }
    }

    if (!model || model[0] == '\0') {
        snprintf(factory_data.model_name, sizeof(factory_data.model_name), "%s", FACTORY_WRITE_MODEL_FALLBACK);
        model = factory_data.model_name;
        ESP_LOGW(TAG, "factory write missing model_name, fallback to %s", model);
    }

    if (mains_frequency < FACTORY_MAINS_PROFILE_220V_50HZ_CCC ||
        mains_frequency > FACTORY_MAINS_PROFILE_120V_60HZ_UL) {
        factory_data.mains_frequency = FACTORY_WRITE_MAINS_FREQ_FALLBACK;
        mains_frequency = factory_data.mains_frequency;
        ESP_LOGW(TAG, "factory write missing mains_profile, fallback to %d", mains_frequency);
    }

    if (!factory_write_float_valid(factory_data.powder_k_value)) {
        ESP_LOGW(TAG,
                 "factory write invalid powder_k=%.8g, default to 0 before powder calibration",
                 (double)factory_data.powder_k_value);
        factory_data.powder_k_value = 0.0f;
    }
    if (!factory_write_float_valid(factory_data.powder_b_value)) {
        ESP_LOGW(TAG,
                 "factory write invalid powder_b=%.8g, default to 0 before powder calibration",
                 (double)factory_data.powder_b_value);
        factory_data.powder_b_value = 0.0f;
    }

#if !FACTORY_WRITE_LIQUID_SCALE_ENABLED
    if (factory_data.liquid_k_value != 0.0f || factory_data.liquid_b_value != 0.0f) {
        ESP_LOGI(TAG,
                 "factory write override liquid coeffs to defaults liquid_k=0 liquid_b=0");
    }
    factory_data.liquid_k_value = 0.0f;
    factory_data.liquid_b_value = 0.0f;
#endif

    if (!factory_write_float_valid(factory_data.flowmeter_coff) ||
        factory_data.flowmeter_coff != FACTORY_WRITE_FLOWMETER_DEFAULT) {
        ESP_LOGI(TAG,
                 "factory write override flowmeter_coff %.8g -> %.2f",
                 (double)factory_data.flowmeter_coff,
                 (double)FACTORY_WRITE_FLOWMETER_DEFAULT);
    }
    factory_data.flowmeter_coff = FACTORY_WRITE_FLOWMETER_DEFAULT;

    if (!ctr_factory_data_is_valid(&factory_data, reason, sizeof(reason))) {
        ESP_LOGE(TAG,
                 "reject PCWRITE@FACTORY: %s sn=%s model=%s mains=%d powderK=%.8g powderB=%.8g liquidK=%.8g liquidB=%.8g flow=%.8g",
                 reason,
                 factory_data.sn_num,
                 factory_data.model_name,
                 factory_data.mains_frequency,
                 (double)factory_data.powder_k_value,
                 (double)factory_data.powder_b_value,
                 (double)factory_data.liquid_k_value,
                 (double)factory_data.liquid_b_value,
                 (double)factory_data.flowmeter_coff);
        return false;
    }

    written = snprintf(buf, len,
                       "123@PCWRITE@FACTORY=%s,%s,%d,%.8g,%.8g,%.8g,%.8g,%.8g,%d,%d,%d,%d,%d,%d#123",
                       sn,
                       model,
                       mains_frequency,
                       (double)factory_data.powder_k_value,
                       (double)factory_data.powder_b_value,
                       (double)factory_data.liquid_k_value,
                       (double)factory_data.liquid_b_value,
                       (double)factory_data.flowmeter_coff,
                       factory_data.powder_weight_coff,
                       factory_data.first_powered_on,
                       factory_data.water_supply_mode,
                       factory_data.reserved_2,
                       factory_data.reserved_3,
                       factory_data.reserved_4);
    if (written <= 0 || (size_t)written >= len) {
        ESP_LOGE(TAG,
                 "reject PCWRITE@FACTORY: command truncated written=%d buf_len=%u",
                 written,
                 (unsigned)len);
        return false;
    }

    ctr_factory_data_persist();
    return true;
}

static bool fmt_factory_read(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@PCREAD@NULL#123") > 0;
}

/* Cleaning and maintenance commands. */
static bool fmt_clean_brew(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)param;
    if (!view) {
        return false;
    }

    return snprintf(buf, len,
                    "123@RUN@HEAT_COFFEE=90,2,1000|VALVE=0|PUMP_COFFEE=0,0,10,%d,4,12000#123",
                    (int)(view->clean_v + 0.5f)) > 0;
}

static bool fmt_main_brew1(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@BREW_CLEAN_1@NULL#123") > 0;
}

static bool fmt_main_brew2(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@BREW_CLEAN_2@NULL#123") > 0;
}

static bool fmt_main_des1(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@DESCALING_1@NULL#123") > 0;
}

static bool fmt_main_des2(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@DESCALING_2@NULL#123") > 0;
}

static bool fmt_main_steam(const app_command_view_t *view, char *buf, uint16_t len, void *param)
{
    (void)view;
    (void)param;
    return snprintf(buf, len, "123@STEAM_CLEAN@NULL#123") > 0;
}

static const ctrl_cmd_desc_t g_ctrl_cmd_table[CTRL_ACT_FACTORY_READ + 1] = {
    [CTRL_ACT_ESPRESSO]    = CTRL_CMD_DESC_SPLIT(CTRL_ACT_ESPRESSO,    fmt_ui_espresso,    fmt_mqtt_espresso),
    [CTRL_ACT_COLD_BREW]   = CTRL_CMD_DESC_SPLIT(CTRL_ACT_COLD_BREW,   fmt_ui_cold_brew,   fmt_mqtt_cold_brew),
    [CTRL_ACT_HOT_WATER]   = CTRL_CMD_DESC_SPLIT(CTRL_ACT_HOT_WATER,   fmt_ui_hot_water,   fmt_mqtt_hot_water),
    [CTRL_ACT_STEAM_START] = CTRL_CMD_DESC_SPLIT(CTRL_ACT_STEAM_START, fmt_ui_steam_start, fmt_mqtt_steam_start),
    [CTRL_ACT_STEAM_STOP]  = CTRL_CMD_DESC_SHARED(CTRL_ACT_STEAM_STOP,  fmt_steam_stop),
    [CTRL_ACT_GRIND_START] = CTRL_CMD_DESC_SPLIT(CTRL_ACT_GRIND_START, fmt_ui_grind_start, fmt_mqtt_grind_start),
    [CTRL_ACT_GRIND_PAUSE] = CTRL_CMD_DESC_SHARED(CTRL_ACT_GRIND_PAUSE, fmt_grind_pause),
    [CTRL_ACT_GRIND_CONTINUE] =
        CTRL_CMD_DESC_SHARED(CTRL_ACT_GRIND_CONTINUE, fmt_grind_continue),
    [CTRL_ACT_GRIND_STOP]  = CTRL_CMD_DESC_SHARED(CTRL_ACT_GRIND_STOP,  fmt_grind_stop),
    [CTRL_ACT_CANCEL]      = CTRL_CMD_DESC_SHARED(CTRL_ACT_CANCEL,      fmt_out_null),
    [CTRL_ACT_READ_STATUS] = CTRL_CMD_DESC_SHARED(CTRL_ACT_READ_STATUS, fmt_read_all),
    [CTRL_ACT_OTA_BOOT]    = CTRL_CMD_DESC_SHARED(CTRL_ACT_OTA_BOOT,    fmt_ota_boot),
    [CTRL_ACT_DRAIN]       = CTRL_CMD_DESC_SHARED(CTRL_ACT_DRAIN,       fmt_drain),
    [CTRL_ACT_POWER]       = CTRL_CMD_DESC_SHARED(CTRL_ACT_POWER,       fmt_power_on),
    [CTRL_ACT_STANDBY]     = CTRL_CMD_DESC_SHARED(CTRL_ACT_STANDBY,     fmt_steam_set_standby),
    [CTRL_ACT_STEAM_SET_NORMAL] =
        CTRL_CMD_DESC_SHARED(CTRL_ACT_STEAM_SET_NORMAL, fmt_steam_set_normal),
    [CTRL_ACT_AMERICANO_BREW] =
        CTRL_CMD_DESC_SPLIT(CTRL_ACT_AMERICANO_BREW, fmt_ui_americano_brew, fmt_mqtt_americano_brew),
    [CTRL_ACT_AMERICANO_WATER] =
        CTRL_CMD_DESC_SPLIT(CTRL_ACT_AMERICANO_WATER, fmt_ui_americano_water, fmt_mqtt_americano_water),
    [CTRL_ACT_FACTORY_WRITE] =
        CTRL_CMD_DESC_SHARED(CTRL_ACT_FACTORY_WRITE, fmt_factory_write),
    [CTRL_ACT_FACTORY_READ] =
        CTRL_CMD_DESC_SHARED(CTRL_ACT_FACTORY_READ, fmt_factory_read),
    [CTRL_ACT_CLEAN_BREW]  = CTRL_CMD_DESC_SHARED(CTRL_ACT_CLEAN_BREW,  fmt_clean_brew),
    [CTRL_ACT_MAINT_BREW1] = CTRL_CMD_DESC_SHARED(CTRL_ACT_MAINT_BREW1, fmt_main_brew1),
    [CTRL_ACT_MAINT_BREW2] = CTRL_CMD_DESC_SHARED(CTRL_ACT_MAINT_BREW2, fmt_main_brew2),
    [CTRL_ACT_MAINT_DES1]  = CTRL_CMD_DESC_SHARED(CTRL_ACT_MAINT_DES1,  fmt_main_des1),
    [CTRL_ACT_MAINT_DES2]  = CTRL_CMD_DESC_SHARED(CTRL_ACT_MAINT_DES2,  fmt_main_des2),
    [CTRL_ACT_MAINT_STEAM] = CTRL_CMD_DESC_SHARED(CTRL_ACT_MAINT_STEAM, fmt_main_steam),
};

bool build_param_cmd(uint16_t val, char *buf, uint16_t len)
{
    if (!buf || len < 32U) {
        return false;
    }

    snprintf(buf, len, "123@RUN@PARAM_SET=%d#123", val);
    return true;
}

bool ctr_cmd_action(control_action_t action, void *param)
{
    char cmd_buf[CMD_BUFFER_SIZE] = {0};
    app_command_view_t command_view = {0};
    const ctrl_cmd_desc_t *desc;
    ctrl_cmd_formatter_t formatter;

    if (action <= CTRL_ACT_NONE || action > CTRL_ACT_FACTORY_READ) {
        ESP_LOGE(TAG, "Unknown action %d", action);
        return false;
    }

    desc = &g_ctrl_cmd_table[action];
    if (desc->action != action) {
        ESP_LOGE(TAG, "Action %d not mapped in g_ctrl_cmd_table", action);
        return false;
    }

    sp_pro_build_command_view(&command_view);
    formatter = ctrl_cmd_select_formatter(desc, command_view.src);
    if (!formatter) {
        ESP_LOGE(TAG, "Formatter not set for action %d, src=%d", action, command_view.src);
        return false;
    }

    if (!formatter(&command_view, cmd_buf, sizeof(cmd_buf), param)) {
        return false;
    }

    ESP_LOGI(TAG, "[CTRL][BUILD] src=%d action=%d cmd=%s", command_view.src, action, cmd_buf);
    ctr_send_cmd(CTR_CMD_CTRL, cmd_buf);
    return true;
}

bool ctr_cmd_param(encoder_mode_t mode)
{
    char cmd_buf[CMD_BUFFER_SIZE] = {0};

    if (!build_param_cmd(mode, cmd_buf, sizeof(cmd_buf))) {
        ESP_LOGE(TAG, "build param failed, mode=%d", mode);
        return false;
    }

    ctr_send_cmd(CTR_CMD_CTRL, cmd_buf);
    ESP_LOGI(TAG, "[CTRL][PARAM] mode=%d cmd=%s", mode, cmd_buf);
    return true;
}
