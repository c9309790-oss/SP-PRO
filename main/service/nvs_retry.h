#ifndef NVS_RETRY_H
#define NVS_RETRY_H

#include <stddef.h>
#include "esp_err.h"
#include "nvs.h"

typedef esp_err_t (*nvs_retry_rewrite_fn_t)(nvs_handle_t nvs_handle, void *ctx);

esp_err_t nvs_retry_rewrite_after_erasing_keys(nvs_handle_t nvs_handle,
                                               esp_err_t err,
                                               const char *tag,
                                               const char *label,
                                               const char *const *keys,
                                               size_t key_count,
                                               nvs_retry_rewrite_fn_t rewrite_fn,
                                               void *ctx);

#endif
