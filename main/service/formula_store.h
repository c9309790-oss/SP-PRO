#ifndef FORMULA_STORE_H
#define FORMULA_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "service_domain_types.h"

typedef struct {
    int ack_code;
    char msg[128];
} formula_store_result_t;

bool formula_store_init(void);
void formula_store_deinit(void);

bool formula_store_load_from_nvs(void);
bool formula_store_save_to_nvs(const formula_overall_t *overall);

int formula_store_get_version(void);
bool formula_store_has_data(void);

bool formula_store_apply_remote(const formula_overall_t *incoming, formula_store_result_t *result);
bool formula_store_get_overall_snapshot(formula_overall_t *overall);
bool formula_store_set_force_update(bool force_update);
bool formula_store_get_force_update(void);
bool formula_store_get_formula(uint32_t formula_id, uint32_t record_id, formula_info_t *out);
bool formula_store_get_formula_by_id(uint32_t formula_id, formula_info_t *out);
bool formula_store_factory_reset(void);
bool formula_store_ensure_local_defaults(void);
bool formula_store_sync_local_setting(uint8_t drink_type);

void formula_store_clear_cache(void);

#endif /* FORMULA_STORE_H */
