#ifndef FACTORY_CFG_H
#define FACTORY_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define FACTORY_CFG_PARTITION_LABEL "factory_cfg"
#define FACTORY_CFG_NAMESPACE       "factory"
#define FACTORY_CFG_KEY_SN          "sn"
#define FACTORY_CFG_KEY_MAINS_FREQ  "mains_frequency"
#define FACTORY_CFG_SN_MAX_LEN      64

esp_err_t factory_cfg_init(void);
bool factory_cfg_has_sn(void);
bool factory_cfg_get_sn(char *sn, size_t size);
const char *factory_cfg_get_sn_ptr(void);
bool factory_cfg_get_mains_frequency(int *value);
void factory_cfg_log_current_sn(void);

#endif /* FACTORY_CFG_H */
