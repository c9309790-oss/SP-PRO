#include "formula_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_retry.h"
#include "mqtt_protocol.h"
#include "mqtt_protocol_codec.h"
#include "sp_pro_app_ctrl.h"

#define FORMULA_STORE_PARTITION_LABEL     "formula_nvs"
#define FORMULA_STORE_NVS_NAMESPACE      "formula_cfg"
#define FORMULA_STORE_KEY_VERSION        "version"
#define FORMULA_STORE_KEY_FORCE_UPD      "force_upd"
#define FORMULA_STORE_KEY_PAYLOAD        "payload"
#define FORMULA_STORE_KEY_PAYLOAD_CRC    "payload_crc"
#define FORMULA_STORE_MAX_PAYLOAD_LEN    (32U * 1024U)
#define FORMULA_STORE_MAX_FORMULA_COUNT  64
#define FORMULA_LABEL_AI                 "AI"
#define FORMULA_LABEL_DEFAULT            "DEFAULT"
#define FORMULA_STORE_DEFAULT_LABEL_ID   (-1)

#define RECORD_ID_ESPRESSO_DEFAULT       1001U
#define RECORD_ID_AMERICANO_DEFAULT      1002U
#define RECORD_ID_COLD_BREW_DEFAULT      1003U
#define RECORD_ID_HOT_WATER_DEFAULT      1005U

typedef struct {
    const char *payload;
    size_t len;
    int version;
    bool force_update;
    uint32_t crc32;
} formula_store_save_ctx_t;

static esp_err_t formula_store_write_payload_to_nvs_retry(nvs_handle_t nvs_handle, void *ctx)
{
    const formula_store_save_ctx_t *save_ctx = (const formula_store_save_ctx_t *)ctx;
    esp_err_t err;

    if (!save_ctx || !save_ctx->payload) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_set_i32(nvs_handle, FORMULA_STORE_KEY_VERSION, save_ctx->version);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, FORMULA_STORE_KEY_FORCE_UPD, save_ctx->force_update ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, FORMULA_STORE_KEY_PAYLOAD, save_ctx->payload, save_ctx->len);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, FORMULA_STORE_KEY_PAYLOAD_CRC, save_ctx->crc32);
    }
    return err;
}

static esp_err_t formula_store_write_force_update_to_nvs(nvs_handle_t nvs_handle, void *ctx)
{
    const bool *force_update = (const bool *)ctx;

    if (!force_update) {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_set_u8(nvs_handle, FORMULA_STORE_KEY_FORCE_UPD, *force_update ? 1U : 0U);
}
#define RECORD_ID_GRIND_DEFAULT          1006U
#define RECORD_ID_PLAYER_DEEP_DEFAULT    1U
#define RECORD_ID_PLAYER_MID_DEFAULT     2U
#define RECORD_ID_PLAYER_LIGHT_DEFAULT   3U
#define LEGACY_RECORD_ID_ESPRESSO        5001U
#define LEGACY_RECORD_ID_AMERICANO       5002U
#define LEGACY_RECORD_ID_COLD_BREW       5003U
#define LEGACY_RECORD_ID_HOT_WATER       5004U
#define LEGACY_RECORD_ID_GRIND           5005U
#define PLAYER_DRINK_NAME                u8"\u73a9\u5bb6"

static const char *TAG = "FORMULA_STORE";

typedef struct {
    uint32_t record_id;
    uint32_t formula_id;
    uint8_t drink_id;
    uint8_t grind_range;
    const char *formula_name;
    const char *drink_name;
    const char *support_mode;
} formula_store_default_meta_t;

typedef struct {
    uint32_t record_id;
    uint32_t formula_id;
    const char *formula_name;
    const char *formula_remark;
    const char *support_mode;
    uint8_t drink_id;
    const char *drink_name;
    int32_t label_id;
    const char *label;
    uint8_t grind_range;
    float grind_weight;
    uint16_t preset_temperature;
    uint16_t preset_liquid_weight;
    uint16_t water_temperature;
    uint16_t water_weight;
    uint16_t milk_temperature;
    prebrew_t prebrew;
    uint8_t stage_priority;
    pressure_stage_t pressure_stage[MAX_STAGE_NUM];
    uint8_t pressure_stage_cnt;
} formula_store_default_formula_t;

typedef struct {
    bool initialized;
    bool loaded;
    bool persisted_force_update;
    formula_overall_t overall;
    char *payload_json;
    size_t payload_len;
} formula_store_cache_t;

static formula_store_cache_t s_formula_store = {0};
static bool s_formula_store_partition_init_attempted = false;
static bool s_formula_store_partition_ready = false;

static const formula_store_default_meta_t s_default_formula_meta[] = {
    { RECORD_ID_ESPRESSO_DEFAULT,  RECORD_ID_ESPRESSO_DEFAULT,  DRINK_ID_ESPRESSO, 6U, u8"\u610f\u5f0f\u6d53\u7f29", u8"\u610f\u5f0f\u6d53\u7f29", "DEFAULT" },
    { RECORD_ID_AMERICANO_DEFAULT, RECORD_ID_AMERICANO_DEFAULT, DRINK_ID_AMERICAN, 6U, u8"\u7f8e\u5f0f",             u8"\u7f8e\u5f0f",             "DEFAULT" },
    { RECORD_ID_COLD_BREW_DEFAULT, RECORD_ID_COLD_BREW_DEFAULT, DRINK_ID_COLDBREW, 6U, u8"\u51b7\u8403",             u8"\u51b7\u8403",             "DEFAULT" },
    { RECORD_ID_HOT_WATER_DEFAULT, RECORD_ID_HOT_WATER_DEFAULT, DRINK_ID_WATER,    0U, u8"\u996e\u7528\u6c34",       u8"\u996e\u7528\u6c34",       "DEFAULT" },
};

static const formula_store_default_formula_t s_sheet_formula_intel_defaults[] = {
    {
        RECORD_ID_ESPRESSO_DEFAULT, RECORD_ID_ESPRESSO_DEFAULT,
        u8"\u610f\u5f0f\u6d53\u7f29", "", "DEFAULT",
        DRINK_ID_ESPRESSO, u8"\u610f\u5f0f\u6d53\u7f29",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        6U, 18U, 92U, 36U, 0U, 0U, 0U,
        {0}, 0U,
        {{9U, 0U}}, 1U
    },
    {
        RECORD_ID_AMERICANO_DEFAULT, RECORD_ID_AMERICANO_DEFAULT,
        u8"\u7f8e\u5f0f", "", "DEFAULT",
        DRINK_ID_AMERICAN, u8"\u7f8e\u5f0f",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        6U, 18U, 92U, 36U, 70U, 210U, 0U,
        {0}, 0U,
        {{9U, 0U}}, 1U
    },
    {
        RECORD_ID_COLD_BREW_DEFAULT, RECORD_ID_COLD_BREW_DEFAULT,
        u8"\u51b7\u8403", "", "DEFAULT",
        DRINK_ID_COLDBREW, u8"\u51b7\u8403",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        6U, 18U, 0U, 125U, 0U, 0U, 0U,
        {0}, 0U,
        {{7U, 0U}}, 1U
    },
    {
        RECORD_ID_HOT_WATER_DEFAULT, RECORD_ID_HOT_WATER_DEFAULT,
        u8"\u996e\u7528\u6c34", "", "DEFAULT",
        DRINK_ID_WATER, u8"\u996e\u7528\u6c34",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        0U, 0U, 0U, 0U, 45U, 150U, 0U,
        {0}, 0U,
        {{0}}, 0U
    },
};

static const formula_store_default_formula_t s_sheet_formula_player_defaults[] = {
    {
        RECORD_ID_PLAYER_DEEP_DEFAULT, RECORD_ID_PLAYER_DEEP_DEFAULT,
        u8"\u51a0\u519b|\u6df1\u70d8\u6d53\u90c1\u66f2\u7ebf",
        u8"\u6e10\u964d\u538b\u529b\uff0c\u964d\u4f4e\u82e6\u6da9\uff0c\u589e\u5f3a\u6d53\u90c1\u5ea6",
        "",
        DRINK_ID_MASTER, u8"\u73a9\u5bb6",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        7U, 18U, 88U, 28U, 0U, 0U, 0U,
        {0}, 0U,
        {{9U, 10U}, {6U, 10U}, {3U, 30U}}, 3U
    },
    {
        RECORD_ID_PLAYER_MID_DEFAULT, RECORD_ID_PLAYER_MID_DEFAULT,
        u8"\u51a0\u519b|\u4e2d\u70d8\u5e73\u8861\u66f2\u7ebf",
        u8"\u56db\u6bb5\u53d8\u538b\uff0c\u7a81\u51fa\u9178\u751c\u5e73\u8861\u4e0e\u5c42\u6b21",
        "",
        DRINK_ID_MASTER, u8"\u73a9\u5bb6",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        8U, 18U, 92U, 34U, 0U, 0U, 0U,
        {0}, 0U,
        {{9U, 10U}, {7U, 5U}, {5U, 5U}, {3U, 30U}}, 4U
    },
    {
        RECORD_ID_PLAYER_LIGHT_DEFAULT, RECORD_ID_PLAYER_LIGHT_DEFAULT,
        u8"\u51a0\u519b|\u6d45\u70d8\u751c\u611f\u66f2\u7ebf",
        u8"\u4f4e\u538b\u6da6\u6e7f\u540e\u9ad8\u538b\u6e10\u964d\uff0c\u7a81\u51fa\u751c\u611f\u4e0e\u82b1\u679c\u9999",
        "",
        DRINK_ID_MASTER, u8"\u73a9\u5bb6",
        FORMULA_STORE_DEFAULT_LABEL_ID, FORMULA_LABEL_DEFAULT,
        8U, 18U, 92U, 50U, 0U, 0U, 0U,
        {0}, 0U,
        {{3U, 5U}, {9U, 10U}, {6U, 5U}, {3U, 30U}}, 4U
    },
};

static bool formula_store_erase_nvs_data(void);
bool formula_store_save_to_nvs(const formula_overall_t *overall);

static const char *formula_store_partition_desc(bool legacy_public_nvs)
{
    return legacy_public_nvs ? "default_nvs" : FORMULA_STORE_PARTITION_LABEL;
}

static esp_err_t formula_store_partition_init(void)
{
    esp_err_t err;

    if (s_formula_store_partition_init_attempted) {
        return s_formula_store_partition_ready ? ESP_OK : ESP_FAIL;
    }

    s_formula_store_partition_init_attempted = true;
    err = nvs_flash_init_partition(FORMULA_STORE_PARTITION_LABEL);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG,
                 "formula_nvs partition init returned %s, erase and retry",
                 esp_err_to_name(err));
        err = nvs_flash_erase_partition(FORMULA_STORE_PARTITION_LABEL);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(FORMULA_STORE_PARTITION_LABEL);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to init formula partition '%s': %s",
                 FORMULA_STORE_PARTITION_LABEL,
                 esp_err_to_name(err));
        return err;
    }

    s_formula_store_partition_ready = true;
    return ESP_OK;
}

static esp_err_t formula_store_open_namespace(bool legacy_public_nvs,
                                              nvs_open_mode_t open_mode,
                                              nvs_handle_t *nvs_handle)
{
    if (!nvs_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (legacy_public_nvs) {
        return nvs_open(FORMULA_STORE_NVS_NAMESPACE, open_mode, nvs_handle);
    }

    if (formula_store_partition_init() != ESP_OK) {
        return ESP_FAIL;
    }

    return nvs_open_from_partition(FORMULA_STORE_PARTITION_LABEL,
                                   FORMULA_STORE_NVS_NAMESPACE,
                                   open_mode,
                                   nvs_handle);
}

static void formula_store_set_result(formula_store_result_t *result, int ack_code, const char *msg)
{
    if (!result) {
        return;
    }

    result->ack_code = ack_code;
    snprintf(result->msg, sizeof(result->msg), "%s", msg ? msg : "");
}

static void formula_store_release_payload(void)
{
    free(s_formula_store.payload_json);
    s_formula_store.payload_json = NULL;
    s_formula_store.payload_len = 0;
}

void formula_store_clear_cache(void)
{
    mqtt_free_formula_overall(&s_formula_store.overall);
    formula_store_release_payload();
    s_formula_store.loaded = false;
    s_formula_store.persisted_force_update = false;
}

bool formula_store_factory_reset(void)
{
    formula_store_clear_cache();

    if (!formula_store_erase_nvs_data()) {
        return false;
    }

    s_formula_store.initialized = true;
    ESP_LOGW(TAG, "formula store reset to factory defaults");
    return true;
}

static uint32_t formula_store_calc_crc32(const char *payload, size_t len)
{
    if (!payload || len == 0) {
        return 0;
    }

    return esp_crc32_le(UINT32_MAX, (const uint8_t *)payload, (uint32_t)len);
}

static bool formula_store_payload_size_valid(size_t len)
{
    return (len > 0U && len <= FORMULA_STORE_MAX_PAYLOAD_LEN);
}

static bool formula_store_is_valid_drink_id(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_MASTER:
    case DRINK_ID_ESPRESSO:
    case DRINK_ID_AMERICAN:
    case DRINK_ID_COLDBREW:
    case DRINK_ID_WATER:
        return true;
    default:
        return false;
    }
}

static bool formula_store_label_equals(const char *label, const char *expected)
{
    if (!label || !expected) {
        return false;
    }

    return strcmp(label, expected) == 0;
}

static bool formula_store_validate_formula_common(const formula_info_t *formula, const char *list_name, int index)
{
    if (!formula) {
        ESP_LOGE(TAG, "formula validation failed: %s[%d] is null", list_name ? list_name : "list", index);
        return false;
    }

    if (formula->record_id == 0U && formula->formula_id == 0U) {
        ESP_LOGE(TAG, "formula validation failed: %s[%d] recordId/formulaId both missing", list_name ? list_name : "list", index);
        return false;
    }

    if (formula->pressure_stage_cnt > MAX_STAGE_NUM) {
        ESP_LOGE(TAG, "formula validation failed: %s[%d] pressureStage count=%u overflow", list_name ? list_name : "list", index, formula->pressure_stage_cnt);
        return false;
    }

    if (formula->velocity_stage_cnt > MAX_STAGE_NUM) {
        ESP_LOGE(TAG, "formula validation failed: %s[%d] velocityStage count=%u overflow", list_name ? list_name : "list", index, formula->velocity_stage_cnt);
        return false;
    }

    if (formula->prebrew.status > 1U) {
        ESP_LOGE(TAG, "formula validation failed: %s[%d] preBrew.status=%u invalid", list_name ? list_name : "list", index, formula->prebrew.status);
        return false;
    }

    return true;
}

static bool formula_store_validate_formula_intel_item(const formula_info_t *formula, const char *list_name, int index)
{
    if (!formula_store_validate_formula_common(formula, list_name, index)) {
        return false;
    }

    return true;
}

static bool formula_store_validate_formula_player_item(const formula_info_t *formula, const char *list_name, int index)
{
    return formula_store_validate_formula_common(formula, list_name, index);
}

static void formula_store_canonicalize_water_formula(formula_info_t *formula)
{
    if (!formula) {
        return;
    }

    formula->record_id = RECORD_ID_HOT_WATER_DEFAULT;
    formula->formula_id = RECORD_ID_HOT_WATER_DEFAULT;
    formula->drink_id = DRINK_ID_WATER;
    formula->grind_range = 0U;
    formula->grind_weight = 0.0f;
    formula->preset_temperature = 0U;
    formula->preset_liquid_weight = 0U;
    formula->milk_temperature = 0U;
    formula->prebrew.status = 0U;
    formula->prebrew.flow_velocity = 0U;
    formula->prebrew.wait_time = 0U;
    formula->prebrew.water_volume = 0U;
    formula->stage_priority = 0U;
    formula->pressure_stage_cnt = 0U;
    memset(formula->pressure_stage, 0, sizeof(formula->pressure_stage));

    if (formula->water_temperature == 0U) {
        formula->water_temperature = 45U;
    }
    if (formula->water_weight == 0U) {
        formula->water_weight = 150U;
    }

    snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", u8"\u996e\u7528\u6c34");
    snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", u8"\u996e\u7528\u6c34");
    snprintf(formula->formula_remark, sizeof(formula->formula_remark), "%s", "");
    snprintf(formula->support_mode, sizeof(formula->support_mode), "%s", "");
}

static void formula_store_normalize_formula_item(formula_info_t *formula, bool is_player_formula)
{
    if (!formula) {
        return;
    }

    if (formula->record_id == 0U && formula->formula_id > 0U) {
        formula->record_id = formula->formula_id;
    }

    if (formula->record_id > 0U) {
        formula->formula_id = formula->record_id;
    }

    if (formula->formula_name[0] == 0) {
        snprintf(formula->formula_name, sizeof(formula->formula_name), "%s",
                 is_player_formula ? PLAYER_DRINK_NAME : u8"\u914d\u65b9");
    }

    if (formula->label[0] == 0) {
        snprintf(formula->label, sizeof(formula->label), "%s", FORMULA_LABEL_DEFAULT);
    }

    if (formula_store_label_equals(formula->label, FORMULA_LABEL_DEFAULT) &&
        formula->label_id == 0) {
        formula->label_id = FORMULA_STORE_DEFAULT_LABEL_ID;
    }

    if (formula_store_label_equals(formula->label, FORMULA_LABEL_AI) &&
        formula->label_id == 0) {
        ESP_LOGW(TAG,
                 "AI formula missing labelId, keep as 0 recordId=%lu formulaId=%lu",
                 (unsigned long)formula->record_id,
                 (unsigned long)formula->formula_id);
    }

    if (is_player_formula) {
        formula->drink_id = DRINK_ID_MASTER;
        snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", PLAYER_DRINK_NAME);
    } else if (formula->drink_id == DRINK_ID_WATER) {
        formula_store_canonicalize_water_formula(formula);
    } else if (formula->drink_name[0] == 0) {
        if (formula->drink_id == DRINK_ID_ESPRESSO) {
            snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", "Espresso");
        } else if (formula->drink_id == DRINK_ID_AMERICAN) {
            snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", "Americano");
        } else if (formula->drink_id == DRINK_ID_COLDBREW) {
            snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", "Cold Brew");
        } else if (formula->drink_id == DRINK_ID_WATER) {
            snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", "Hot Water");
        }
    }
}

static void formula_store_normalize_formula_overall(formula_overall_t *overall)
{
    if (!overall) {
        return;
    }

    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        formula_store_normalize_formula_item(&overall->formula_intel_list[i], false);
    }

    for (int i = 0; i < overall->formula_list_count; i++) {
        formula_store_normalize_formula_item(&overall->formula_list[i], true);
    }
}

static bool formula_store_validate_formula_overall(const formula_overall_t *overall)
{
    if (!overall) {
        ESP_LOGE(TAG, "formulaOverall validation failed: null payload");
        return false;
    }

    if (overall->version <= 0) {
        ESP_LOGE(TAG, "formulaOverall validation failed: version=%d invalid", overall->version);
        return false;
    }

    if (overall->formula_intel_list_count < 0 || overall->formula_intel_list_count > FORMULA_STORE_MAX_FORMULA_COUNT) {
        ESP_LOGE(TAG, "formulaOverall validation failed: formulaIntelList count=%d invalid", overall->formula_intel_list_count);
        return false;
    }

    if (overall->formula_list_count < 0 || overall->formula_list_count > FORMULA_STORE_MAX_FORMULA_COUNT) {
        ESP_LOGE(TAG, "formulaOverall validation failed: formulaList count=%d invalid", overall->formula_list_count);
        return false;
    }

    if (overall->formula_intel_list_count == 0 && overall->formula_list_count == 0) {
        ESP_LOGE(TAG, "formulaOverall validation failed: both formula lists are empty");
        return false;
    }

    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        if (!formula_store_validate_formula_intel_item(&overall->formula_intel_list[i], "formulaIntelList", i)) {
            return false;
        }
    }

    for (int i = 0; i < overall->formula_list_count; i++) {
        if (!formula_store_validate_formula_player_item(&overall->formula_list[i], "formulaList", i)) {
            return false;
        }
    }

    return true;
}

static void formula_store_init_formula_meta(formula_info_t *formula, const formula_store_default_meta_t *meta)
{
    if (!formula || !meta) {
        return;
    }

    memset(formula, 0, sizeof(*formula));
    formula->record_id = meta->record_id;
    formula->formula_id = meta->formula_id;
    formula->drink_id = meta->drink_id;
    formula->label_id = FORMULA_STORE_DEFAULT_LABEL_ID;
    formula->grind_range = meta->grind_range;
    snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", meta->formula_name);
    snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", meta->drink_name);
    snprintf(formula->label, sizeof(formula->label), "%s", FORMULA_LABEL_DEFAULT);
    snprintf(formula->support_mode, sizeof(formula->support_mode), "%s", meta->support_mode ? meta->support_mode : "");
}

static void formula_store_init_formula_from_default(formula_info_t *formula,
                                                    const formula_store_default_formula_t *preset)
{
    if (!formula || !preset) {
        return;
    }

    memset(formula, 0, sizeof(*formula));
    formula->record_id = preset->record_id;
    formula->formula_id = preset->formula_id;
    formula->drink_id = preset->drink_id;
    formula->label_id = preset->label_id;
    formula->grind_range = preset->grind_range;
    formula->grind_weight = preset->grind_weight;
    formula->preset_temperature = preset->preset_temperature;
    formula->preset_liquid_weight = preset->preset_liquid_weight;
    formula->water_temperature = preset->water_temperature;
    formula->water_weight = preset->water_weight;
    formula->milk_temperature = preset->milk_temperature;
    formula->prebrew = preset->prebrew;
    formula->stage_priority = preset->stage_priority;
    formula->pressure_stage_cnt = preset->pressure_stage_cnt;
    memcpy(formula->pressure_stage, preset->pressure_stage, sizeof(preset->pressure_stage));

    snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", preset->formula_name ? preset->formula_name : "");
    snprintf(formula->formula_remark, sizeof(formula->formula_remark), "%s", preset->formula_remark ? preset->formula_remark : "");
    snprintf(formula->support_mode, sizeof(formula->support_mode), "%s", preset->support_mode ? preset->support_mode : "");
    snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", preset->drink_name ? preset->drink_name : "");
    snprintf(formula->label, sizeof(formula->label), "%s", preset->label ? preset->label : FORMULA_LABEL_DEFAULT);
}

static void formula_store_apply_settings_to_formula(formula_info_t *formula, const app_command_view_t *settings)
{
    if (!formula || !settings) {
        return;
    }

    if (formula_store_label_equals(formula->label, FORMULA_LABEL_DEFAULT) &&
        formula->label_id != FORMULA_STORE_DEFAULT_LABEL_ID) {
        ESP_LOGI(TAG,
                 "Local formula edit clears AI-linked labelId, drinkId=%u oldLabelId=%d",
                 (unsigned)formula->drink_id,
                 (int)formula->label_id);
        formula->label_id = FORMULA_STORE_DEFAULT_LABEL_ID;
    }

    switch (formula->drink_id) {
    case DRINK_ID_ESPRESSO:
        formula->grind_weight = settings->grind_w;
        formula->preset_temperature = (uint16_t)(settings->esp_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->esp_brew_w + 0.5f);
        formula->water_temperature = 0U;
        formula->water_weight = 0U;
        formula->milk_temperature = 0U;
        break;

    case DRINK_ID_AMERICAN:
        formula->grind_weight = settings->grind_w;
        formula->preset_temperature = (uint16_t)(settings->ame_brew_t + 0.5f);
        formula->preset_liquid_weight = (uint16_t)(settings->ame_brew_w + 0.5f);
        formula->water_temperature = (uint16_t)(settings->ame_water_t + 0.5f);
        formula->water_weight = (uint16_t)(settings->ame_water_w + 0.5f);
        formula->milk_temperature = 0U;
        break;

    case DRINK_ID_COLDBREW:
        formula->grind_weight = settings->grind_w;
        formula->preset_temperature = 0U;
        formula->preset_liquid_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        formula->water_temperature = 0U;
        formula->water_weight = (uint16_t)(settings->cold_brew_w + 0.5f);
        formula->milk_temperature = 0U;
        break;

    case DRINK_ID_WATER:
        formula->grind_weight = 0.0f;
        formula->grind_range = 0U;
        formula->preset_temperature = 0U;
        formula->preset_liquid_weight = 0U;
        formula->water_temperature = (uint16_t)(settings->hot_water_t + 0.5f);
        formula->water_weight = (uint16_t)(settings->hot_water_w + 0.5f);
        formula->milk_temperature = 0U;
        formula->prebrew.status = 0U;
        formula->prebrew.flow_velocity = 0U;
        formula->prebrew.wait_time = 0U;
        formula->prebrew.water_volume = 0U;
        formula->stage_priority = 0U;
        formula->pressure_stage_cnt = 0U;
        memset(formula->pressure_stage, 0, sizeof(formula->pressure_stage));
        snprintf(formula->formula_name, sizeof(formula->formula_name), "%s", u8"\u996e\u7528\u6c34");
        snprintf(formula->drink_name, sizeof(formula->drink_name), "%s", u8"\u996e\u7528\u6c34");
        break;

    default:
        break;
    }
}

static bool formula_store_build_default_overall(const app_command_view_t *settings, formula_overall_t *overall)
{
    int intel_count;
    int player_count;

    if (!settings || !overall) {
        return false;
    }

    (void)settings;
    memset(overall, 0, sizeof(*overall));
    intel_count = (int)(sizeof(s_sheet_formula_intel_defaults) / sizeof(s_sheet_formula_intel_defaults[0]));
    player_count = (int)(sizeof(s_sheet_formula_player_defaults) / sizeof(s_sheet_formula_player_defaults[0]));

    overall->formula_intel_list = calloc((size_t)intel_count, sizeof(formula_info_t));
    overall->formula_list = calloc((size_t)player_count, sizeof(formula_info_t));
    if (!overall->formula_intel_list || !overall->formula_list) {
        mqtt_free_formula_overall(overall);
        return false;
    }

    overall->version = 1;
    overall->force_update = false;
    overall->formula_intel_list_count = intel_count;
    overall->formula_list_count = player_count;

    for (int i = 0; i < intel_count; i++) {
        formula_store_init_formula_from_default(&overall->formula_intel_list[i], &s_sheet_formula_intel_defaults[i]);
    }

    for (int i = 0; i < player_count; i++) {
        formula_store_init_formula_from_default(&overall->formula_list[i], &s_sheet_formula_player_defaults[i]);
    }

    return true;
}

static bool formula_store_matches_legacy_local_defaults(const formula_overall_t *overall)
{
    bool seen_espresso = false;
    bool seen_americano = false;
    bool seen_coldbrew = false;
    bool seen_water = false;
    bool seen_grind = false;

    if (!overall || !overall->formula_intel_list) {
        return false;
    }

    if (overall->formula_list_count != 0 || overall->formula_intel_list_count != 5) {
        return false;
    }

    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        const formula_info_t *formula = &overall->formula_intel_list[i];
        switch (formula->record_id) {
        case LEGACY_RECORD_ID_ESPRESSO:
            seen_espresso = (formula->record_id == LEGACY_RECORD_ID_ESPRESSO);
            break;
        case LEGACY_RECORD_ID_AMERICANO:
            seen_americano = (formula->record_id == LEGACY_RECORD_ID_AMERICANO);
            break;
        case LEGACY_RECORD_ID_COLD_BREW:
            seen_coldbrew = (formula->record_id == LEGACY_RECORD_ID_COLD_BREW);
            break;
        case LEGACY_RECORD_ID_HOT_WATER:
            seen_water = (formula->record_id == LEGACY_RECORD_ID_HOT_WATER);
            break;
        case LEGACY_RECORD_ID_GRIND:
            seen_grind = (formula->record_id == LEGACY_RECORD_ID_GRIND);
            break;
        default:
            return false;
        }
    }

    return seen_espresso && seen_americano && seen_coldbrew && seen_water && seen_grind;
}

static bool formula_store_migrate_legacy_local_defaults(void)
{
    app_command_view_t settings = {0};
    formula_overall_t defaults = {0};
    bool ok;

    if (!s_formula_store.loaded ||
        !formula_store_matches_legacy_local_defaults(&s_formula_store.overall)) {
        return true;
    }

    sp_pro_app_get_command_view(&settings);
    if (!formula_store_build_default_overall(&settings, &defaults)) {
        ESP_LOGE(TAG, "failed to rebuild sheet-based default formulaOverall");
        return false;
    }

    defaults.version = s_formula_store.overall.version + 1;
    ok = formula_store_save_to_nvs(&defaults);
    mqtt_free_formula_overall(&defaults);
    if (ok) {
        ESP_LOGW(TAG, "local default formulaOverall migrated to latest sheet defaults");
    }
    return ok;
}

static formula_info_t *formula_store_find_formula_mutable(formula_overall_t *overall, uint8_t drink_id)
{
    if (!overall || !overall->formula_intel_list) {
        return NULL;
    }

    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        if (overall->formula_intel_list[i].drink_id == drink_id) {
            return &overall->formula_intel_list[i];
        }
    }

    return NULL;
}

static const formula_store_default_meta_t *formula_store_find_default_meta(uint8_t drink_id)
{
    for (size_t i = 0; i < sizeof(s_default_formula_meta) / sizeof(s_default_formula_meta[0]); i++) {
        if (s_default_formula_meta[i].drink_id == drink_id) {
            return &s_default_formula_meta[i];
        }
    }

    return NULL;
}

static formula_info_t *formula_store_append_formula_intel(formula_overall_t *overall,
                                                          const formula_store_default_meta_t *meta,
                                                          const app_command_view_t *settings)
{
    formula_info_t *new_list;
    int new_count;

    if (!overall || !meta) {
        return NULL;
    }

    new_count = overall->formula_intel_list_count + 1;
    new_list = realloc(overall->formula_intel_list, (size_t)new_count * sizeof(formula_info_t));
    if (!new_list) {
        return NULL;
    }

    overall->formula_intel_list = new_list;
    overall->formula_intel_list_count = new_count;
    formula_store_init_formula_meta(&overall->formula_intel_list[new_count - 1], meta);
    formula_store_apply_settings_to_formula(&overall->formula_intel_list[new_count - 1], settings);
    return &overall->formula_intel_list[new_count - 1];
}

static bool formula_store_clone_overall(const formula_overall_t *src, formula_overall_t *dst)
{
    if (!src || !dst) {
        return false;
    }

    memset(dst, 0, sizeof(*dst));
    dst->version = src->version;
    dst->force_update = src->force_update;
    dst->formula_intel_list_count = src->formula_intel_list_count;
    dst->formula_list_count = src->formula_list_count;

    if (src->formula_intel_list_count > 0) {
        size_t len = (size_t)src->formula_intel_list_count * sizeof(formula_info_t);
        dst->formula_intel_list = malloc(len);
        if (!dst->formula_intel_list) {
            mqtt_free_formula_overall(dst);
            return false;
        }
        memcpy(dst->formula_intel_list, src->formula_intel_list, len);
    }

    if (src->formula_list_count > 0) {
        size_t len = (size_t)src->formula_list_count * sizeof(formula_info_t);
        dst->formula_list = malloc(len);
        if (!dst->formula_list) {
            mqtt_free_formula_overall(dst);
            return false;
        }
        memcpy(dst->formula_list, src->formula_list, len);
    }

    return true;
}

static bool formula_store_local_drink_type_to_id(uint8_t drink_type, uint8_t *drink_id)
{
    if (!drink_id) {
        return false;
    }

    switch (drink_type) {
    case KEY_ESPRESSO:
        *drink_id = DRINK_ID_ESPRESSO;
        return true;
    case KEY_AMERICANO:
        *drink_id = DRINK_ID_AMERICAN;
        return true;
    case KEY_COLD_BREW:
        *drink_id = DRINK_ID_COLDBREW;
        return true;
    case KEY_WATER:
        *drink_id = DRINK_ID_WATER;
        return true;
    default:
        return false;
    }
}

static bool formula_store_is_local_coffee_drink(uint8_t drink_id)
{
    switch (drink_id) {
    case DRINK_ID_ESPRESSO:
    case DRINK_ID_AMERICAN:
    case DRINK_ID_COLDBREW:
        return true;

    default:
        return false;
    }
}

static void formula_store_apply_local_grind_to_overall(formula_overall_t *overall, float grind_w)
{
    if (!overall || !overall->formula_intel_list || grind_w <= 0.0f) {
        return;
    }

    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        formula_info_t *formula = &overall->formula_intel_list[i];
        if (formula_store_is_local_coffee_drink(formula->drink_id)) {
            formula->grind_weight = grind_w;
            if (formula_store_label_equals(formula->label, FORMULA_LABEL_DEFAULT) &&
                formula->label_id != FORMULA_STORE_DEFAULT_LABEL_ID) {
                formula->label_id = FORMULA_STORE_DEFAULT_LABEL_ID;
            }
        }
    }
}

static bool formula_store_erase_nvs_data_internal(bool legacy_public_nvs)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = formula_store_open_namespace(legacy_public_nvs, NVS_READWRITE, &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "formula store erase open failed in %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        return false;
    }

    esp_err_t erase_err = nvs_erase_key(nvs_handle, FORMULA_STORE_KEY_VERSION);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_err;
    }

    erase_err = nvs_erase_key(nvs_handle, FORMULA_STORE_KEY_FORCE_UPD);
    if (err == ESP_OK && erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_err;
    }

    erase_err = nvs_erase_key(nvs_handle, FORMULA_STORE_KEY_PAYLOAD);
    if (err == ESP_OK && erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_err;
    }

    erase_err = nvs_erase_key(nvs_handle, FORMULA_STORE_KEY_PAYLOAD_CRC);
    if (err == ESP_OK && erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_err;
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "formula store erase failed in %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGW(TAG,
             "formula store invalid payload cleared from %s",
             formula_store_partition_desc(legacy_public_nvs));
    return true;
}

static bool formula_store_erase_nvs_data(void)
{
    return formula_store_erase_nvs_data_internal(false);
}

static bool formula_store_read_force_update_flag_internal(bool legacy_public_nvs, bool *force_update_out)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    uint8_t force_update = 0U;

    if (!force_update_out) {
        return false;
    }

    *force_update_out = false;

    err = formula_store_open_namespace(legacy_public_nvs, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "formula forceUpdate open failed in %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_get_u8(nvs_handle, FORMULA_STORE_KEY_FORCE_UPD, &force_update);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "formula forceUpdate read failed in %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        return false;
    }

    *force_update_out = (force_update != 0U);
    return true;
}

static bool formula_store_read_force_update_flag(bool *force_update_out)
{
    return formula_store_read_force_update_flag_internal(false, force_update_out);
}

static cJSON *formula_store_create_json(const formula_overall_t *overall)
{
    cJSON *root;
    cJSON *formula_intel_list;
    cJSON *formula_list;

    if (!overall) {
        return NULL;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", overall->version);
    cJSON_AddBoolToObject(root, "forceUpdate", overall->force_update);

    formula_intel_list = cJSON_CreateArray();
    if (!formula_intel_list) {
        cJSON_Delete(root);
        return NULL;
    }
    for (int i = 0; i < overall->formula_intel_list_count; i++) {
        cJSON_AddItemToArray(formula_intel_list, mqtt_create_formula_json(&overall->formula_intel_list[i]));
    }
    cJSON_AddItemToObject(root, "formulaIntelList", formula_intel_list);

    formula_list = cJSON_CreateArray();
    if (!formula_list) {
        cJSON_Delete(root);
        return NULL;
    }
    for (int i = 0; i < overall->formula_list_count; i++) {
        cJSON_AddItemToArray(formula_list, mqtt_create_formula_list_json(&overall->formula_list[i]));
    }
    cJSON_AddItemToObject(root, "formulaList", formula_list);

    return root;
}

static bool formula_store_build_payload(const formula_overall_t *overall, char **payload_out, size_t *len_out)
{
    cJSON *root;
    char *json;
    size_t len;

    if (!overall || !payload_out || !len_out) {
        return false;
    }

    *payload_out = NULL;
    *len_out = 0;

    root = formula_store_create_json(overall);
    if (!root) {
        return false;
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return false;
    }

    len = strlen(json) + 1U;
    if (!formula_store_payload_size_valid(len)) {
        ESP_LOGE(TAG, "formulaOverall payload too large: len=%u limit=%u", (unsigned)len, (unsigned)FORMULA_STORE_MAX_PAYLOAD_LEN);
        free(json);
        return false;
    }

    *payload_out = json;
    *len_out = len;
    return true;
}

static bool formula_store_parse_payload(const char *payload, formula_overall_t *overall)
{
    cJSON *root;
    bool ok;

    if (!payload || !overall) {
        return false;
    }

    root = cJSON_Parse(payload);
    if (!root) {
        return false;
    }

    ok = mqtt_parse_formula_overall_json(root, overall);
    cJSON_Delete(root);
    return ok;
}

static bool formula_store_update_cache_from_payload(const char *payload, size_t len)
{
    formula_overall_t parsed = {0};
    char *payload_copy;

    if (!payload || !formula_store_payload_size_valid(len)) {
        return false;
    }

    if (!formula_store_parse_payload(payload, &parsed)) {
        ESP_LOGE(TAG, "Failed to parse formula payload into cache");
        return false;
    }

    formula_store_normalize_formula_overall(&parsed);

    if (!formula_store_validate_formula_overall(&parsed)) {
        mqtt_free_formula_overall(&parsed);
        ESP_LOGE(TAG, "Rejected invalid formula payload while updating cache");
        return false;
    }

    payload_copy = malloc(len);
    if (!payload_copy) {
        mqtt_free_formula_overall(&parsed);
        ESP_LOGE(TAG, "No memory for formula payload cache, len=%u", (unsigned)len);
        return false;
    }

    memcpy(payload_copy, payload, len);

    formula_store_clear_cache();
    s_formula_store.overall = parsed;
    s_formula_store.payload_json = payload_copy;
    s_formula_store.payload_len = len;
    s_formula_store.loaded = true;
    s_formula_store.persisted_force_update = parsed.force_update;
    return true;
}

static bool formula_store_write_payload_to_nvs(const char *payload, size_t len, int version, bool force_update)
{
    nvs_handle_t nvs_handle;
    formula_store_save_ctx_t save_ctx;
    esp_err_t err;
    uint32_t crc32;

    if (!payload || !formula_store_payload_size_valid(len)) {
        return false;
    }

    crc32 = formula_store_calc_crc32(payload, len);
    save_ctx.payload = payload;
    save_ctx.len = len;
    save_ctx.version = version;
    save_ctx.force_update = force_update;
    save_ctx.crc32 = crc32;

    err = formula_store_open_namespace(false, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "formula_nvs open failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = formula_store_write_payload_to_nvs_retry(nvs_handle, &save_ctx);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        static const char *const keys[] = {
            FORMULA_STORE_KEY_VERSION,
            FORMULA_STORE_KEY_FORCE_UPD,
            FORMULA_STORE_KEY_PAYLOAD,
            FORMULA_STORE_KEY_PAYLOAD_CRC,
        };
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "formula payload",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   formula_store_write_payload_to_nvs_retry,
                                                   &save_ctx);
    }

    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist formula payload: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

bool formula_store_save_to_nvs(const formula_overall_t *overall)
{
    char *payload = NULL;
    size_t payload_len = 0;
    bool ok;

    if (!overall) {
        return false;
    }

    if (!formula_store_validate_formula_overall(overall)) {
        ESP_LOGE(TAG, "formulaOverall save rejected by validation");
        return false;
    }

    if (!formula_store_build_payload(overall, &payload, &payload_len)) {
        ESP_LOGE(TAG, "Failed to serialize formulaOverall for NVS");
        return false;
    }

    ok = formula_store_write_payload_to_nvs(payload, payload_len, overall->version, overall->force_update);
    if (ok) {
        ok = formula_store_update_cache_from_payload(payload, payload_len);
    }

    free(payload);
    return ok;
}

bool formula_store_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t payload_len = 0;
    char *payload = NULL;
    uint32_t stored_crc = 0;
    uint32_t actual_crc;
    bool ok = false;
    bool should_clear_invalid_nvs = false;
    bool persisted_force_update = false;
    bool legacy_public_nvs = false;

    formula_store_clear_cache();

    if (formula_store_read_force_update_flag(&persisted_force_update)) {
        s_formula_store.persisted_force_update = persisted_force_update;
    }

retry_load:
    err = formula_store_open_namespace(legacy_public_nvs, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "formula namespace not ready in %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        if (!legacy_public_nvs) {
            legacy_public_nvs = true;
            if (!s_formula_store.persisted_force_update &&
                formula_store_read_force_update_flag_internal(true, &persisted_force_update)) {
                s_formula_store.persisted_force_update = persisted_force_update;
            }
            goto retry_load;
        }
        return false;
    }

    err = nvs_get_blob(nvs_handle, FORMULA_STORE_KEY_PAYLOAD, NULL, &payload_len);
    if (err == ESP_ERR_NVS_NOT_FOUND || payload_len == 0U) {
        ESP_LOGI(TAG,
                 "No persisted formula payload in %s",
                 formula_store_partition_desc(legacy_public_nvs));
        nvs_close(nvs_handle);
        if (!legacy_public_nvs) {
            legacy_public_nvs = true;
            if (!s_formula_store.persisted_force_update &&
                formula_store_read_force_update_flag_internal(true, &persisted_force_update)) {
                s_formula_store.persisted_force_update = persisted_force_update;
            }
            goto retry_load;
        }
        return false;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to query formula payload length from %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        nvs_close(nvs_handle);
        formula_store_erase_nvs_data_internal(legacy_public_nvs);
        return false;
    }

    if (!formula_store_payload_size_valid(payload_len)) {
        ESP_LOGE(TAG,
                 "Persisted formula payload length invalid in %s: len=%u limit=%u",
                 formula_store_partition_desc(legacy_public_nvs),
                 (unsigned)payload_len,
                 (unsigned)FORMULA_STORE_MAX_PAYLOAD_LEN);
        nvs_close(nvs_handle);
        formula_store_erase_nvs_data_internal(legacy_public_nvs);
        return false;
    }

    payload = malloc(payload_len);
    if (!payload) {
        ESP_LOGE(TAG, "No memory to load formula payload, len=%u", (unsigned)payload_len);
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_blob(nvs_handle, FORMULA_STORE_KEY_PAYLOAD, payload, &payload_len);
    if (err == ESP_OK) {
        err = nvs_get_u32(nvs_handle, FORMULA_STORE_KEY_PAYLOAD_CRC, &stored_crc);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to read formula payload from %s: %s",
                 formula_store_partition_desc(legacy_public_nvs),
                 esp_err_to_name(err));
        should_clear_invalid_nvs = true;
        goto cleanup;
    }

    actual_crc = formula_store_calc_crc32(payload, payload_len);
    if (stored_crc == 0U || stored_crc != actual_crc) {
        ESP_LOGE(TAG, "Formula payload CRC mismatch, stored=0x%08lX actual=0x%08lX",
                 (unsigned long)stored_crc,
                 (unsigned long)actual_crc);
        should_clear_invalid_nvs = true;
        goto cleanup;
    }

    ok = formula_store_update_cache_from_payload(payload, payload_len);
    if (!ok) {
        should_clear_invalid_nvs = true;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Formula store loaded from %s, version=%d formulaList=%d formulaIntelList=%d forceUpdate=%d",
             formula_store_partition_desc(legacy_public_nvs),
             s_formula_store.overall.version,
             s_formula_store.overall.formula_list_count,
             s_formula_store.overall.formula_intel_list_count,
             s_formula_store.overall.force_update);

    if (legacy_public_nvs) {
        if (formula_store_write_payload_to_nvs(s_formula_store.payload_json,
                                               s_formula_store.payload_len,
                                               s_formula_store.overall.version,
                                               s_formula_store.overall.force_update)) {
            if (!formula_store_erase_nvs_data_internal(true)) {
                ESP_LOGW(TAG, "Legacy formula payload migration succeeded but cleanup failed");
            } else {
                ESP_LOGI(TAG,
                         "Migrated formula payload from default_nvs to %s",
                         FORMULA_STORE_PARTITION_LABEL);
            }
        } else {
            ESP_LOGW(TAG,
                     "Failed to migrate legacy formula payload into %s",
                     FORMULA_STORE_PARTITION_LABEL);
        }
    }

cleanup:
    free(payload);
    if (should_clear_invalid_nvs) {
        formula_store_erase_nvs_data_internal(legacy_public_nvs);
    }
    return ok;
}

int formula_store_get_version(void)
{
    return s_formula_store.loaded ? s_formula_store.overall.version : 0;
}

bool formula_store_has_data(void)
{
    return s_formula_store.loaded;
}

bool formula_store_get_overall_snapshot(formula_overall_t *overall)
{
    if (!overall) {
        return false;
    }

    memset(overall, 0, sizeof(*overall));
    overall->force_update = s_formula_store.persisted_force_update;

    if (!s_formula_store.loaded) {
        return false;
    }

    overall->version = s_formula_store.overall.version;
    overall->formula_intel_list = s_formula_store.overall.formula_intel_list;
    overall->formula_intel_list_count = s_formula_store.overall.formula_intel_list_count;
    overall->formula_list = s_formula_store.overall.formula_list;
    overall->formula_list_count = s_formula_store.overall.formula_list_count;
    return true;
}

bool formula_store_ensure_local_defaults(void)
{
    app_command_view_t settings = {0};
    formula_overall_t defaults = {0};
    bool ok;

    if (s_formula_store.loaded) {
        return true;
    }

    sp_pro_app_get_command_view(&settings);
    if (!formula_store_build_default_overall(&settings, &defaults)) {
        ESP_LOGE(TAG, "failed to build local default formulaOverall");
        return false;
    }

    ok = formula_store_save_to_nvs(&defaults);
    mqtt_free_formula_overall(&defaults);
    if (ok) {
        ESP_LOGI(TAG, "local default formulaOverall initialized");
    }
    return ok;
}

bool formula_store_sync_local_setting(uint8_t drink_type)
{
    app_command_view_t settings = {0};
    formula_overall_t updated = {0};
    formula_info_t *formula;
    const formula_store_default_meta_t *default_meta;
    uint8_t drink_id;
    bool ok;

    if (!formula_store_ensure_local_defaults()) {
        return false;
    }

    sp_pro_app_get_command_view(&settings);
    if (!formula_store_clone_overall(&s_formula_store.overall, &updated)) {
        ESP_LOGE(TAG, "failed to clone local formulaOverall for update");
        return false;
    }

    if (drink_type == KEY_GRIND) {
        formula_store_apply_local_grind_to_overall(&updated, settings.grind_w);
        updated.version = s_formula_store.overall.version + 1;
        updated.force_update = false;

        ok = formula_store_save_to_nvs(&updated);
        mqtt_free_formula_overall(&updated);
        if (ok) {
            mqtt_set_formula_force_update_pending(false);
            ESP_LOGI(TAG, "local formulaOverall updated grind version=%d", s_formula_store.overall.version);
        }
        return ok;
    }

    if (!formula_store_local_drink_type_to_id(drink_type, &drink_id)) {
        mqtt_free_formula_overall(&updated);
        return false;
    }

    formula = formula_store_find_formula_mutable(&updated, drink_id);
    if (!formula) {
        default_meta = formula_store_find_default_meta(drink_id);
        formula = formula_store_append_formula_intel(&updated, default_meta, &settings);
        if (!formula) {
            ESP_LOGE(TAG, "local formulaOverall missing drink_id=%u and append failed", drink_id);
            mqtt_free_formula_overall(&updated);
            return false;
        }
        ESP_LOGW(TAG, "local formulaOverall missing drink_id=%u, appended default formula", drink_id);
    }

    formula_store_apply_settings_to_formula(formula, &settings);
    updated.version = s_formula_store.overall.version + 1;
    updated.force_update = false;

    ok = formula_store_save_to_nvs(&updated);
    mqtt_free_formula_overall(&updated);
    if (ok) {
        mqtt_set_formula_force_update_pending(false);
        ESP_LOGI(TAG, "local formulaOverall updated drinkType=%u version=%d", drink_type, s_formula_store.overall.version);
    }
    return ok;
}

static bool formula_store_find_in_list(const formula_info_t *list, int count, uint32_t formula_id, uint32_t record_id, formula_info_t *out)
{
    if (!list || count <= 0 || !out) {
        return false;
    }

    if (formula_id > 0U && record_id > 0U) {
        for (int i = 0; i < count; i++) {
            if (list[i].formula_id == formula_id && list[i].record_id == record_id) {
                *out = list[i];
                return true;
            }
        }
    }

    if (record_id > 0U) {
        for (int i = 0; i < count; i++) {
            if (list[i].record_id == record_id) {
                *out = list[i];
                return true;
            }
        }
        return false;
    }

    if (formula_id > 0U) {
        for (int i = 0; i < count; i++) {
            if (list[i].formula_id == formula_id) {
                *out = list[i];
                return true;
            }
        }
    }

    return false;
}

bool formula_store_get_formula(uint32_t formula_id, uint32_t record_id, formula_info_t *out)
{
    if (!out || (!formula_id && !record_id) || !s_formula_store.loaded) {
        return false;
    }

    if (formula_store_find_in_list(s_formula_store.overall.formula_list,
                                   s_formula_store.overall.formula_list_count,
                                   formula_id,
                                   record_id,
                                   out)) {
        return true;
    }

    if (formula_store_find_in_list(s_formula_store.overall.formula_intel_list,
                                   s_formula_store.overall.formula_intel_list_count,
                                   formula_id,
                                   record_id,
                                   out)) {
        return true;
    }

    return false;
}

bool formula_store_get_formula_by_id(uint32_t formula_id, formula_info_t *out)
{
    return formula_store_get_formula(formula_id, 0U, out);
}

bool formula_store_set_force_update(bool force_update)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    if (s_formula_store.persisted_force_update == force_update &&
        (!s_formula_store.loaded || s_formula_store.overall.force_update == force_update)) {
        return true;
    }

    err = formula_store_open_namespace(false, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open formula namespace for forceUpdate=%d: %s", force_update, esp_err_to_name(err));
        return false;
    }

    err = formula_store_write_force_update_to_nvs(nvs_handle, &force_update);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        static const char *const keys[] = {
            FORMULA_STORE_KEY_FORCE_UPD,
        };
        err = nvs_retry_rewrite_after_erasing_keys(nvs_handle,
                                                   err,
                                                   TAG,
                                                   "formula forceUpdate",
                                                   keys,
                                                   sizeof(keys) / sizeof(keys[0]),
                                                   formula_store_write_force_update_to_nvs,
                                                   &force_update);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update stored forceUpdate=%d: %s", force_update, esp_err_to_name(err));
        return false;
    }

    s_formula_store.persisted_force_update = force_update;
    if (s_formula_store.loaded) {
        s_formula_store.overall.force_update = force_update;
    }

    ESP_LOGI(TAG, "Stored formula forceUpdate updated to %d", force_update);
    return true;
}

bool formula_store_get_force_update(void)
{
    return s_formula_store.persisted_force_update;
}

bool formula_store_apply_remote(const formula_overall_t *incoming, formula_store_result_t *result)
{
    char *incoming_payload = NULL;
    size_t incoming_len = 0;
    int local_version;
    bool ok = false;
    formula_overall_t normalized;

    if (!incoming || !result) {
        return false;
    }

    if (!formula_store_ensure_local_defaults()) {
        formula_store_set_result(result, 2, "formula init failed");
        return false;
    }

    normalized = *incoming;
    normalized.force_update = false;
    formula_store_normalize_formula_overall(&normalized);

    if (!formula_store_validate_formula_overall(&normalized)) {
        formula_store_set_result(result, 2, "formula invalid");
        return false;
    }

    local_version = formula_store_get_version();

    ESP_LOGI(TAG,
             "formulaOverall apply incomingVersion=%d incomingForce=%d localVersion=%d loaded=%d intelCount=%d formulaCount=%d",
             incoming->version,
             incoming->force_update,
             local_version,
             s_formula_store.loaded,
             incoming->formula_intel_list_count,
             incoming->formula_list_count);

    if (!s_formula_store.loaded || incoming->version != local_version) {
        ESP_LOGW(TAG, "formulaOverall version conflict: incoming=%d local=%d", incoming->version, local_version);
        formula_store_set_result(result, 1, "formula version conflict");
        goto cleanup;
    }

    normalized.version = local_version + 1;

    if (!formula_store_build_payload(&normalized, &incoming_payload, &incoming_len)) {
        ESP_LOGE(TAG, "formulaOverall serialize failed incomingVersion=%d", incoming->version);
        formula_store_set_result(result, 2, "formula serialize failed");
        return false;
    }

    if (!formula_store_write_payload_to_nvs(incoming_payload, incoming_len, normalized.version, normalized.force_update)) {
        ESP_LOGE(TAG, "formulaOverall persist failed version=%d", normalized.version);
        formula_store_set_result(result, 2, "formula persist failed");
        goto cleanup;
    }

    if (!formula_store_update_cache_from_payload(incoming_payload, incoming_len)) {
        ESP_LOGE(TAG, "formulaOverall cache update failed version=%d", normalized.version);
        formula_store_set_result(result, 2, "formula cache update failed");
        goto cleanup;
    }

    sp_pro_app_reload_beverage_settings_from_formula_store();
    mqtt_set_formula_force_update_pending(false);
    ESP_LOGI(TAG, "formulaOverall stored successfully version=%d", normalized.version);
    formula_store_set_result(result, 0, "OK");
    ok = true;

cleanup:
    free(incoming_payload);
    return ok;
}

static bool formula_store_apply_remote_mqtt(const formula_overall_t *incoming, mqtt_ack_result_t *ack_result)
{
    formula_store_result_t result = {0};
    bool ok = formula_store_apply_remote(incoming, &result);

    if (ack_result) {
        ack_result->ack_code = result.ack_code;
        snprintf(ack_result->msg, sizeof(ack_result->msg), "%s", result.msg);
    }

    return ok;
}

bool formula_store_init(void)
{
    if (s_formula_store.initialized) {
        return true;
    }

    formula_store_load_from_nvs();
    formula_store_migrate_legacy_local_defaults();
    mqtt_register_formula_overall_handler(formula_store_apply_remote_mqtt);
    s_formula_store.initialized = true;
    ESP_LOGI(TAG, "Formula store initialized");
    return true;
}

void formula_store_deinit(void)
{
    formula_store_clear_cache();
    s_formula_store.initialized = false;
}


