#include "drink_record_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "record_publish_port.h"

#define DRINK_RECORD_DATA_TYPE_OPERATION     1
#define DRINK_RECORD_RESULT_FAIL            -1
#define DRINK_RECORD_RESULT_SUCCESS          0
#define DRINK_RECORD_RESULT_CANCEL           1
#define DRINK_RECORD_MATERIAL_BEAN           0
#define DRINK_RECORD_MATERIAL_COFFEE_POWDER  1
#define DRINK_RECORD_LOCATION_DEFAULT        1
#define DRINK_RECORD_IDLE_MS              3000LL

static const char *TAG = "DRINK_RECORD";

typedef struct {
    bool active;
    bool local_session;
    bool machine_started;
    bool failed;
    app_state_t local_state;
    control_action_t remote_action;
    uint8_t drink_id;
    int64_t start_ms;
    int64_t idle_since_ms;
    formula_info_t formula;
} drink_record_session_t;

static drink_record_session_t s_session = {0};

static const char *drink_record_drink_name(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_MASTER:
        return "Player";
    case DRINK_ID_ESPRESSO:
        return "Espresso";
    case DRINK_ID_AMERICAN:
        return "Americano";
    case DRINK_ID_COLDBREW:
        return "Cold Brew";
    case DRINK_ID_WATER:
        return "Water";
    default:
        return "Unknown";
    }
}

static bool drink_record_is_local_beverage_state(app_state_t state)
{
    return state == ST_ESPRESSO ||
           state == ST_MASTER ||
           state == ST_AMERICANO ||
           state == ST_COLD_BREW;
}

static uint8_t drink_record_state_to_drink_id(app_state_t state)
{
    switch (state) {
    case ST_MASTER:
        return DRINK_ID_MASTER;
    case ST_ESPRESSO:
        return DRINK_ID_ESPRESSO;
    case ST_AMERICANO:
        return DRINK_ID_AMERICAN;
    case ST_COLD_BREW:
        return DRINK_ID_COLDBREW;
    default:
        return DRINK_ID_MASTER;
    }
}

static int64_t drink_record_now_ms(void)
{
    return (int64_t)time(NULL) * 1000LL;
}

static void drink_record_reset_session(void)
{
    memset(&s_session, 0, sizeof(s_session));
}

static float drink_record_bean_weight_from_grind_range(uint8_t grind_range)
{
    switch (grind_range) {
    case 1:
        return 6.0f;
    case 2:
        return 7.25f;
    case 3:
        return 8.5f;
    case 4:
        return 9.25f;
    case 5:
        return 11.0f;
    default:
        return 0.0f;
    }
}

static void drink_record_add_material(drink_record_t *record,
                                      uint8_t type,
                                      uint8_t location,
                                      float value)
{
    material_item_t *item;

    if (!record || value <= 0.0f || record->material_cnt >= (sizeof(record->materials) / sizeof(record->materials[0]))) {
        return;
    }

    item = &record->materials[record->material_cnt++];
    item->type = type;
    item->location = location;
    item->value = value;
}

static void drink_record_fill_materials(const formula_info_t *formula, drink_record_t *record)
{
    float bean_weight;

    if (!formula || !record) {
        return;
    }

    bean_weight = drink_record_bean_weight_from_grind_range(formula->grind_range);
    if (bean_weight > 0.0f) {
        drink_record_add_material(record,
                                  DRINK_RECORD_MATERIAL_BEAN,
                                  DRINK_RECORD_LOCATION_DEFAULT,
                                  bean_weight);
        return;
    }

    if (formula->grind_weight > 0U) {
        drink_record_add_material(record,
                                  DRINK_RECORD_MATERIAL_COFFEE_POWDER,
                                  DRINK_RECORD_LOCATION_DEFAULT,
                                  (float)formula->grind_weight);
    }
}

static void drink_record_build_local_formula(app_state_t state,
                                             const app_beverage_settings_t *settings,
                                             formula_info_t *formula)
{
    uint8_t drink_id;
    const char *drink_name;

    if (!settings || !formula) {
        return;
    }

    memset(formula, 0, sizeof(*formula));
    drink_id = drink_record_state_to_drink_id(state);
    drink_name = drink_record_drink_name(drink_id);

    formula->drink_id = drink_id;
    snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", drink_name);
    snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", drink_name);
    formula->grind_weight = (uint16_t)(settings->grind_w + 0.5f);

    switch (state) {
    case ST_MASTER:
        formula->preset_temperature = (uint16_t)(settings->esp_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->esp_brew_w + 0.5f);
        break;
    case ST_ESPRESSO:
        formula->preset_temperature = (uint16_t)(settings->esp_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->esp_brew_w + 0.5f);
        break;
    case ST_AMERICANO:
        formula->preset_temperature = (uint16_t)(settings->ame_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->ame_brew_w + 0.5f);
        formula->water_temperature = (uint16_t)(settings->ame_water_t + 0.5f);
        formula->water_weight = (uint16_t)(settings->ame_water_w + 0.5f);
        break;
    case ST_COLD_BREW:
        formula->preset_liquid_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        formula->water_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        break;
    default:
        break;
    }
}

static bool drink_record_is_remote_record_action(control_action_t action)
{
    return action == CTRL_ACT_ESPRESSO ||
           action == CTRL_ACT_AMERICANO_BREW ||
           action == CTRL_ACT_COLD_BREW ||
           action == CTRL_ACT_HOT_WATER ||
           action == CTRL_ACT_GRIND_START;
}

static void drink_record_prepare_remote_formula(control_action_t action,
                                                const formula_info_t *formula,
                                                formula_info_t *prepared)
{
    if (!formula || !prepared) {
        return;
    }

    *prepared = *formula;
    if (action == CTRL_ACT_GRIND_START) {
        snprintf(prepared->support_mode, sizeof(prepared->support_mode), "%s", "playerMode");
        if (prepared->formula_name[0] == 0) {
            snprintf(prepared->formula_name, sizeof(prepared->formula_name), "%s", "Grind");
        }
        if (prepared->drink_name[0] == 0) {
            snprintf(prepared->drink_name, sizeof(prepared->drink_name), "%s", "Player");
        }
    }
}

static void drink_record_publish_result(int result)
{
    drink_record_t record = {0};

    if (!s_session.active) {
        return;
    }

    record.drink_name_index = s_session.drink_id;
    record.produce_time = (long)(drink_record_now_ms() / 1000LL);
    record.result = (int8_t)result;
    record.data_type = DRINK_RECORD_DATA_TYPE_OPERATION;
    record.semi_formula = s_session.formula;
    drink_record_fill_materials(&record.semi_formula, &record);

    ESP_LOGI(TAG,
             "publish drink record result=%d drinkId=%u local=%d formula=%s materialCnt=%u",
             result,
             (unsigned int)record.drink_name_index,
             s_session.local_session ? 1 : 0,
             record.semi_formula.formula_name,
             (unsigned int)record.material_cnt);
    record_publish_drink(&record);
    drink_record_reset_session();
}

static void drink_record_start_session(bool local_session,
                                       app_state_t local_state,
                                       control_action_t remote_action,
                                       const formula_info_t *formula)
{
    if (!formula) {
        return;
    }

    drink_record_reset_session();
    s_session.active = true;
    s_session.local_session = local_session;
    s_session.local_state = local_state;
    s_session.remote_action = remote_action;
    s_session.drink_id = formula->drink_id;
    s_session.start_ms = drink_record_now_ms();
    s_session.formula = *formula;

    ESP_LOGI(TAG,
             "drink record session start local=%d state=%d action=%d drinkId=%u formula=%s",
             local_session ? 1 : 0,
             (int)local_state,
             (int)remote_action,
             (unsigned int)s_session.drink_id,
             s_session.formula.formula_name);
}

static bool drink_record_is_remote_action_active(control_action_t action, const MACHINE_STATUS *status)
{
    if (!status) {
        return false;
    }

    if (action == CTRL_ACT_GRIND_START) {
        return status->grind_run_flg != 0U;
    }

    return (status->drink_making_flg != DRINK_MAKER_NONE) || (status->hot_water_flg != 0U);
}

bool drink_record_service_init(void)
{
    drink_record_reset_session();
    return true;
}

void drink_record_notify_local_state_start(app_state_t state, const app_beverage_settings_t *settings)
{
    formula_info_t formula;

    if (!drink_record_is_local_beverage_state(state) || !settings) {
        return;
    }

    if (s_session.active && s_session.local_session && s_session.local_state == state) {
        return;
    }

    drink_record_build_local_formula(state, settings, &formula);
    drink_record_start_session(true, state, CTRL_ACT_NONE, &formula);
}

void drink_record_notify_local_state_success(app_state_t state)
{
    if (!s_session.active || !s_session.local_session || s_session.local_state != state) {
        return;
    }

    drink_record_publish_result(DRINK_RECORD_RESULT_SUCCESS);
}

void drink_record_notify_local_state_cancel(app_state_t state)
{
    if (!s_session.active || !s_session.local_session || s_session.local_state != state) {
        return;
    }

    drink_record_publish_result(DRINK_RECORD_RESULT_CANCEL);
}

void drink_record_notify_local_state_fail(app_state_t state)
{
    if (!s_session.active || !s_session.local_session || s_session.local_state != state) {
        return;
    }

    drink_record_publish_result(DRINK_RECORD_RESULT_FAIL);
}

void drink_record_notify_remote_action_start(control_action_t action, const formula_info_t *formula)
{
    formula_info_t prepared_formula = {0};

    if (!drink_record_is_remote_record_action(action) || !formula) {
        return;
    }

    drink_record_prepare_remote_formula(action, formula, &prepared_formula);
    drink_record_start_session(false, ST_READY, action, &prepared_formula);
}

void drink_record_notify_remote_cancel(void)
{
    if (!s_session.active || s_session.local_session) {
        return;
    }

    drink_record_publish_result(DRINK_RECORD_RESULT_CANCEL);
}

void drink_record_notify_remote_fail(void)
{
    if (!s_session.active || s_session.local_session) {
        return;
    }

    drink_record_publish_result(DRINK_RECORD_RESULT_FAIL);
}

void drink_record_handle_machine_status(const MACHINE_STATUS *status)
{
    int64_t now_ms;

    if (!s_session.active || !status || s_session.local_session) {
        return;
    }

    now_ms = drink_record_now_ms();

    if (drink_record_is_remote_action_active(s_session.remote_action, status)) {
        s_session.machine_started = true;
        s_session.idle_since_ms = 0;
        if (status->error_code != 0U) {
            s_session.failed = true;
        }
        return;
    }

    if (!s_session.machine_started) {
        return;
    }

    if (status->error_code != 0U) {
        s_session.failed = true;
    }

    if (s_session.idle_since_ms == 0) {
        s_session.idle_since_ms = now_ms;
        return;
    }

    if ((now_ms - s_session.idle_since_ms) >= DRINK_RECORD_IDLE_MS) {
        drink_record_publish_result(s_session.failed ? DRINK_RECORD_RESULT_FAIL
                                                     : DRINK_RECORD_RESULT_SUCCESS);
    }
}
