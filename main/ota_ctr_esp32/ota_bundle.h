#ifndef OTA_BUNDLE_H
#define OTA_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ota_bundle_format.h"

size_t ota_bundle_header_bytes(void);

esp_err_t ota_bundle_parse_header(const void *data, size_t len, ota_bundle_header_t *out_header);
esp_err_t ota_bundle_validate_header(const ota_bundle_header_t *header);

bool ota_bundle_entry_is_valid(const ota_bundle_header_t *header, const ota_bundle_entry_t *entry);
const ota_bundle_entry_t *ota_bundle_find_entry(const ota_bundle_header_t *header,
                                                ota_bundle_payload_type_t type);

#endif /* OTA_BUNDLE_H */
