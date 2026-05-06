#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_protocol_core.h"

static const char *TAG = "MQTT_PROTOCOL";
static mqtt_resource_response_handler_t s_resource_response_handler = NULL;
static mqtt_resource_retry_handler_t s_resource_retry_handler = NULL;

static uint16_t mqtt_resource_prebrew_water_volume_for_report(const formula_info_t *formula)
{
    if (!formula) {
        return 0U;
    }

    if (formula->drink_id == DRINK_ID_MASTER && formula->prebrew.water_volume < 5U) {
        return 5U;
    }

    return formula->prebrew.water_volume;
}

static cJSON *mqtt_create_formula_json(const formula_info_t *formula)
{
    cJSON *semi_formula;
    cJSON *prebrew;
    cJSON *pressure_stage_array;
    cJSON *velocity_stage_array;

    if (!formula) {
        return NULL;
    }

    semi_formula = cJSON_CreateObject();
    cJSON_AddNumberToObject(semi_formula, "recordId", formula->record_id);
    cJSON_AddNumberToObject(semi_formula, "formulaId", formula->formula_id);
    cJSON_AddStringToObject(semi_formula, "formulaName", formula->formula_name);
    cJSON_AddStringToObject(semi_formula, "formulaRemark", formula->formula_remark);
    cJSON_AddStringToObject(semi_formula, "supportMode", formula->support_mode);
    cJSON_AddNumberToObject(semi_formula, "drinkId", formula->drink_id);
    cJSON_AddStringToObject(semi_formula, "drinkName", formula->drink_name);
    cJSON_AddNumberToObject(semi_formula, "labelId", formula->label_id);
    cJSON_AddStringToObject(semi_formula, "label", formula->label);
    cJSON_AddNumberToObject(semi_formula, "grindRange", formula->grind_range);
    cJSON_AddNumberToObject(semi_formula, "grindWeight", formula->grind_weight);
    cJSON_AddNumberToObject(semi_formula, "presetTemperature", formula->preset_temperature);
    cJSON_AddNumberToObject(semi_formula, "presetLiquidWeight", formula->preset_liquid_weight);
    cJSON_AddNumberToObject(semi_formula, "waterTemperature", formula->water_temperature);
    cJSON_AddNumberToObject(semi_formula, "waterWeight", formula->water_weight);
    cJSON_AddNumberToObject(semi_formula, "milkTemperature", formula->milk_temperature);
    cJSON_AddNumberToObject(semi_formula, "stagePriority", formula->stage_priority);

    prebrew = cJSON_CreateObject();
    cJSON_AddNumberToObject(prebrew, "status", formula->prebrew.status);
    cJSON_AddNumberToObject(prebrew, "flowVelocity", formula->prebrew.flow_velocity);
    cJSON_AddNumberToObject(prebrew, "waitTime", formula->prebrew.wait_time);
    cJSON_AddNumberToObject(prebrew,
                            "waterVolume",
                            mqtt_resource_prebrew_water_volume_for_report(formula));
    cJSON_AddItemToObject(semi_formula, "preBrew", prebrew);

    pressure_stage_array = cJSON_CreateArray();
    for (int i = 0; i < formula->pressure_stage_cnt; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "pressure", formula->pressure_stage[i].pressure);
        cJSON_AddNumberToObject(item, "waitTime", formula->pressure_stage[i].wait_time);
        cJSON_AddItemToArray(pressure_stage_array, item);
    }
    cJSON_AddItemToObject(semi_formula, "pressureStage", pressure_stage_array);

    velocity_stage_array = cJSON_CreateArray();
    for (int i = 0; i < formula->velocity_stage_cnt; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "flowVelocity", formula->velocity_stage[i].flow_velocity);
        cJSON_AddNumberToObject(item, "waitTime", formula->velocity_stage[i].wait_time);
        cJSON_AddItemToArray(velocity_stage_array, item);
    }
    cJSON_AddItemToObject(semi_formula, "velocityStage", velocity_stage_array);

    return semi_formula;
}

static char *mqtt_build_curve_profile_string(const extraction_curve_record_t *record, bool pressure_profile)
{
    size_t estimated_len;
    size_t used = 0U;
    char *buffer;

    if (!record || record->point_count == 0U) {
        return mqtt_strdup("");
    }

    estimated_len = ((size_t)record->point_count * 32U) + 1U;
    buffer = calloc(estimated_len, sizeof(char));
    if (!buffer) {
        return NULL;
    }

    for (uint16_t i = 0U; i < record->point_count; i++) {
        const extraction_curve_point_t *point = &record->points[i];
        int written = snprintf(buffer + used,
                               estimated_len - used,
                               "%sx%.3fy%.2f",
                               (i == 0U) ? "" : ",",
                               (double)point->elapsed_ms / 1000.0,
                               pressure_profile ? (double)point->pressure
                                                : (double)point->flow_rate);
        if (written < 0 || (size_t)written >= (estimated_len - used)) {
            break;
        }
        used += (size_t)written;
    }

    return buffer;
}

void mqtt_register_resource_response_handler(mqtt_resource_response_handler_t handler)
{
    s_resource_response_handler = handler;
}

void mqtt_register_resource_retry_handler(mqtt_resource_retry_handler_t handler)
{
    s_resource_retry_handler = handler;
}

void mqtt_free_resource_response(mqtt_resource_response_t *response)
{
    if (!response)
        return;

    free(response->resource_item_list);
    memset(response, 0, sizeof(*response));
}

void mqtt_free_resource_retry_response(mqtt_resource_retry_response_t *response)
{
    if (!response)
        return;

    if (response->resource_url_list)
    {
        for (int i = 0; i < response->resource_url_count; i++)
            free(response->resource_url_list[i]);
    }

    free(response->resource_url_list);
    memset(response, 0, sizeof(*response));
}

static bool mqtt_parse_resource_response_from_mqtt(cJSON *data, mqtt_resource_response_t *response)
{
    if (!data || !response)
        return false;

    cJSON *resource_item_list = json_get_array(data, "resourceItemList");
    if (!resource_item_list)
        return false;

    memset(response, 0, sizeof(*response));
    response->resource_type = json_get_int(data, "resourceType", 0);
    mqtt_copy_string(response->device_type, sizeof(response->device_type), json_get_string(data, "deviceType", ""), "");
    response->latest_update_time = json_get_int64(data, "latestUpdateTime", 0);

    int count = cJSON_GetArraySize(resource_item_list);
    if (count <= 0)
        return true;

    response->resource_item_list = calloc(count, sizeof(mqtt_resource_item_t));
    if (!response->resource_item_list)
        return false;

    response->resource_item_list_count = count;
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(resource_item_list, i);
        mqtt_resource_item_t *resource_item = &response->resource_item_list[i];
        mqtt_copy_string(resource_item->item_type, sizeof(resource_item->item_type), json_get_string(item, "itemType", ""), "");
        mqtt_copy_string(resource_item->item_language, sizeof(resource_item->item_language), json_get_string(item, "itemLanguage", ""), "");
        mqtt_copy_string(resource_item->item_type_name, sizeof(resource_item->item_type_name), json_get_string(item, "itemTypeName", ""), "");
        mqtt_copy_string(resource_item->item_title, sizeof(resource_item->item_title), json_get_string(item, "itemTitle", ""), "");
        mqtt_copy_string(resource_item->item_cover, sizeof(resource_item->item_cover), json_get_string(item, "itemCover", ""), "");
        resource_item->item_cover_size = json_get_int64(item, "itemCoverSize", 0);
        mqtt_copy_string(resource_item->item_url, sizeof(resource_item->item_url), json_get_string(item, "itemUrl", ""), "");
        resource_item->item_size = json_get_int64(item, "itemSize", 0);
        mqtt_copy_string(resource_item->item_md5, sizeof(resource_item->item_md5), json_get_string(item, "itemMd5", ""), "");
    }

    return true;
}

static bool mqtt_parse_resource_retry_from_mqtt(cJSON *data, mqtt_resource_retry_response_t *response)
{
    if (!data || !response)
        return false;

    cJSON *resource_url = json_get_array(data, "resourceUrl");
    cJSON *ack_code = cJSON_GetObjectItem(data, "ackCode");
    if (!resource_url && !cJSON_IsString(ack_code))
        return false;

    memset(response, 0, sizeof(*response));
    mqtt_copy_string(response->ack_code, sizeof(response->ack_code), json_get_string(data, "ackCode", ""), "");

    if (!resource_url)
        return true;

    int count = cJSON_GetArraySize(resource_url);
    if (count <= 0)
        return true;

    response->resource_url_list = calloc(count, sizeof(char *));
    if (!response->resource_url_list)
        return false;

    response->resource_url_count = count;
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(resource_url, i);
        if (cJSON_IsString(item))
            response->resource_url_list[i] = mqtt_strdup(item->valuestring);
    }

    return true;
}

static bool mqtt_handle_resource_message_from_mqtt(cJSON *data)
{
    mqtt_resource_response_t resource_response = {0};
    if (mqtt_parse_resource_response_from_mqtt(data, &resource_response))
    {
        if (s_resource_response_handler)
            s_resource_response_handler(&resource_response);
        else
            ESP_LOGW(TAG, "resource response received without handler");

        mqtt_free_resource_response(&resource_response);
        return true;
    }

    mqtt_resource_retry_response_t resource_retry = {0};
    if (mqtt_parse_resource_retry_from_mqtt(data, &resource_retry))
    {
        if (s_resource_retry_handler)
            s_resource_retry_handler(&resource_retry);
        else
            ESP_LOGW(TAG, "resource retry response received without handler");

        mqtt_free_resource_retry_response(&resource_retry);
        return true;
    }

    return false;
}

bool mqtt_handle_resource_data_from_mqtt(cJSON *data)
{
    return mqtt_handle_resource_message_from_mqtt(data);
}

void publish_log_info_to_mqtt(const char *log_detail)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(NULL, NULL, &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "logserver/kalerm/iot/log/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *log_info = cJSON_CreateObject();
    cJSON_AddStringToObject(log_info, "logDetail", log_detail ? log_detail : "");
    cJSON_AddItemToObject(data, "logInfo", log_info);
    mqtt_publish_json_root(root);
}

void publish_resource_request_to_mqtt(int resource_type, const char *language)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(NULL, NULL, &data);
    char sub_topic[160];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "dataserver/kalerm/iot/requestResource/%s");
    mqtt_add_sub_topic(data, sub_topic);
    cJSON_AddNumberToObject(data, "resourceType", resource_type);
    cJSON_AddStringToObject(data, "language", language ? language : "");
    mqtt_publish_json_root(root);
}

void publish_resource_fail_to_mqtt(const char *const *resource_fail_list, int resource_fail_count)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(NULL, NULL, &data);
    char sub_topic[160];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "dataserver/kalerm/iot/requestResourceFail/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *resource_fail = cJSON_CreateArray();
    for (int i = 0; i < resource_fail_count; i++)
    {
        if (resource_fail_list && resource_fail_list[i])
            cJSON_AddItemToArray(resource_fail, cJSON_CreateString(resource_fail_list[i]));
    }
    cJSON_AddItemToObject(data, "resourceFail", resource_fail);
    mqtt_publish_json_root(root);
}

void publish_version_info_to_mqtt(version_info_t *ver)
{
    version_info_t *version_info = ver ? ver : &g_device_version;
    char hmi_version[16] = "";
    char ctr_code[sizeof(version_info->current_ctr_fw_version)] = "";
    size_t src_idx = 0;
    size_t dst_idx = 0;
    mqtt_get_hmi_version_from_app(hmi_version, sizeof(hmi_version));

    while (version_info->current_ctr_fw_version[src_idx] != '\0' &&
           dst_idx + 1 < sizeof(ctr_code)) {
        char ch = version_info->current_ctr_fw_version[src_idx++];
        if (ch == '.') {
            continue;
        }
        ctr_code[dst_idx++] = ch;
    }
    ctr_code[dst_idx] = '\0';

    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root("0", NULL, &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "updateserver/kalerm/iot/checklatestversion/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "deviceType", version_info->device_type);
    cJSON_AddStringToObject(info, "ctrVersion", hmi_version);
    cJSON_AddStringToObject(info, "ctrCode", ctr_code);
    cJSON_AddStringToObject(info, "iotFunctionVersion", version_info->iot_fun_version);
    cJSON_AddStringToObject(info, "iotConfigFunctionVersion", version_info->iot_cfg_func_version);
    cJSON_AddStringToObject(info, "grinderGroupVersion", version_info->grinder_group_version);
    cJSON_AddItemToObject(data, "checklatestversionInfo", info);
    mqtt_publish_json_root(root);
}

void publish_drink_record_to_mqtt(drink_record_t *rec)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root("0", NULL, &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "dataserver/kalerm/iot/drink/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *drink_record = cJSON_CreateObject();
    cJSON_AddNumberToObject(drink_record, "drinkNameIndex", rec->drink_name_index);
    cJSON_AddNumberToObject(drink_record, "produceTime", rec->produce_time);
    cJSON_AddNumberToObject(drink_record, "result", rec->result);
    cJSON_AddNumberToObject(drink_record, "dataType", rec->data_type);

    formula_info_t *formula = &rec->semi_formula;
    cJSON_AddItemToObject(drink_record, "semiFormula", mqtt_create_formula_json(formula));

    cJSON *materials_array = cJSON_CreateArray();
    for (int i = 0; i < rec->material_cnt; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "type", rec->materials[i].type);
        cJSON_AddNumberToObject(item, "location", rec->materials[i].location);
        cJSON_AddNumberToObject(item, "value", rec->materials[i].value);
        cJSON_AddItemToArray(materials_array, item);
    }
    cJSON_AddItemToObject(drink_record, "materials", materials_array);

    cJSON_AddItemToObject(data, "drinkRecord", drink_record);
    mqtt_publish_json_root(root);
}

void publish_extraction_curve_records_to_mqtt(extraction_curve_record_t *records,
                                              int record_count,
                                              bool curve_update)
{
#if !EXTRACTION_CURVE_FEATURE_ENABLE
    (void)records;
    (void)record_count;
    (void)curve_update;
    return;
#else
    cJSON *data = NULL;
    cJSON *root;
    cJSON *curve_info_array;
    char sub_topic[160];

    if (!records || record_count <= 0) {
        return;
    }

    root = mqtt_create_message_root("0", NULL, &data);
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "dataserver/kalerm/iot/extractionCurve/%s");
    mqtt_add_sub_topic(data, sub_topic);
    cJSON_AddStringToObject(data, "curveUpdate", curve_update ? "true" : "false");

    curve_info_array = cJSON_CreateArray();
    for (int i = 0; i < record_count; i++) {
        cJSON *curve_info;
        cJSON *curve;
        char *pressure_profile;
        char *flow_profile;

        pressure_profile = mqtt_build_curve_profile_string(&records[i], true);
        flow_profile = mqtt_build_curve_profile_string(&records[i], false);

        curve_info = cJSON_CreateObject();
        curve = cJSON_CreateObject();
        cJSON_AddNumberToObject(curve, "id", records[i].id);
        cJSON_AddStringToObject(curve, "formulaName", records[i].semi_formula.formula_name);
        cJSON_AddNumberToObject(curve, "time", records[i].produce_time);
        cJSON_AddStringToObject(curve, "pressureProfile", pressure_profile ? pressure_profile : "");
        cJSON_AddStringToObject(curve, "flowProfile", flow_profile ? flow_profile : "");
        cJSON_AddItemToObject(curve_info, "curve", curve);
        cJSON_AddItemToObject(curve_info, "semiFormula", mqtt_create_formula_json(&records[i].semi_formula));
        cJSON_AddItemToArray(curve_info_array, curve_info);

        free(pressure_profile);
        free(flow_profile);
    }

    cJSON_AddItemToObject(data, "extractionCurveInfo", curve_info_array);
    mqtt_publish_json_root(root);
#endif
}

void publish_event_record_to_mqtt(event_record_t *rec)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root("0", NULL, &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "dataserver/kalerm/iot/event/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *event_array = cJSON_CreateArray();
    for (int i = 0; i < rec->event_cnt; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", rec->events[i].key);
        cJSON_AddStringToObject(item, "value", rec->events[i].value);
        cJSON_AddItemToArray(event_array, item);
    }
    cJSON_AddItemToObject(data, "event", event_array);
    mqtt_publish_json_root(root);
}


