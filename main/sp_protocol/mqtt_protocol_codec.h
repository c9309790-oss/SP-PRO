#ifndef IOT_MQTT_PROTOCOL_CODEC_H
#define IOT_MQTT_PROTOCOL_CODEC_H

#include "cJSON.h"
#include "service_domain_types.h"

bool mqtt_parse_formula_overall_json(cJSON *formula_overall_json, formula_overall_t *formula_overall);
bool mqtt_parse_formula_from_json(cJSON *param, formula_info_t *formula);
cJSON *mqtt_create_formula_json(const formula_info_t *formula);
cJSON *mqtt_create_formula_list_json(const formula_info_t *formula);

#endif /* IOT_MQTT_PROTOCOL_CODEC_H */
