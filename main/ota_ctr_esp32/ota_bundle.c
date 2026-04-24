#include "ota_bundle.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "OTA_BUNDLE";

typedef char ota_bundle_header_size_must_match[
    (sizeof(ota_bundle_header_t) == OTA_BUNDLE_HEADER_SIZE) ? 1 : -1];

static bool ota_bundle_payload_type_is_supported(uint32_t type)
{
    return type == OTA_BUNDLE_PAYLOAD_ESP32 || type == OTA_BUNDLE_PAYLOAD_CTR;
}

static bool ota_bundle_range_is_valid(uint32_t offset, uint32_t size, uint32_t package_size)
{
    uint64_t end = (uint64_t)offset + (uint64_t)size;

    if (size == 0U) {
        return false;
    }

    if (offset < OTA_BUNDLE_HEADER_SIZE) {
        return false;
    }

    if ((offset % OTA_BUNDLE_ALIGN) != 0U) {
        return false;
    }

    if (end > (uint64_t)package_size) {
        return false;
    }

    return true;
}

static bool ota_bundle_entries_overlap(const ota_bundle_entry_t *left, const ota_bundle_entry_t *right)
{
    uint64_t left_begin = left->offset;
    uint64_t left_end = (uint64_t)left->offset + (uint64_t)left->size;
    uint64_t right_begin = right->offset;
    uint64_t right_end = (uint64_t)right->offset + (uint64_t)right->size;

    return left_begin < right_end && right_begin < left_end;
}

static esp_err_t ota_bundle_validate_entries(const ota_bundle_header_t *header)
{
    size_t i;
    size_t j;

    for (i = 0; i < header->entry_count; ++i) {
        const ota_bundle_entry_t *entry = &header->entries[i];

        if (!ota_bundle_payload_type_is_supported(entry->type)) {
            ESP_LOGE(TAG, "Bundle entry[%u] has unsupported type=%lu",
                     (unsigned)i, (unsigned long)entry->type);
            return ESP_ERR_INVALID_ARG;
        }

        if (!ota_bundle_range_is_valid(entry->offset, entry->size, header->package_size)) {
            ESP_LOGE(TAG,
                     "Bundle entry[%u] has invalid range: offset=%lu size=%lu package=%lu",
                     (unsigned)i,
                     (unsigned long)entry->offset,
                     (unsigned long)entry->size,
                     (unsigned long)header->package_size);
            return ESP_ERR_INVALID_SIZE;
        }

        for (j = i + 1; j < header->entry_count; ++j) {
            const ota_bundle_entry_t *other = &header->entries[j];

            if (entry->type == other->type) {
                ESP_LOGE(TAG,
                         "Bundle entries[%u] and [%u] repeat payload type=%lu",
                         (unsigned)i,
                         (unsigned)j,
                         (unsigned long)entry->type);
                return ESP_ERR_INVALID_STATE;
            }

            if (ota_bundle_entries_overlap(entry, other)) {
                ESP_LOGE(TAG,
                         "Bundle entries[%u] and [%u] overlap",
                         (unsigned)i,
                         (unsigned)j);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    return ESP_OK;
}

size_t ota_bundle_header_bytes(void)
{
    return sizeof(ota_bundle_header_t);
}

esp_err_t ota_bundle_validate_header(const ota_bundle_header_t *header)
{
    if (header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (header->magic != OTA_BUNDLE_MAGIC) {
        ESP_LOGE(TAG, "Bundle magic mismatch: 0x%08lX", (unsigned long)header->magic);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (header->format_version != OTA_BUNDLE_VERSION) {
        ESP_LOGE(TAG,
                 "Bundle format_version mismatch: got=%u expect=%u",
                 (unsigned)header->format_version,
                 (unsigned)OTA_BUNDLE_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    if (header->header_size != OTA_BUNDLE_HEADER_SIZE) {
        ESP_LOGE(TAG,
                 "Bundle header_size mismatch: got=%u expect=%u",
                 (unsigned)header->header_size,
                 (unsigned)OTA_BUNDLE_HEADER_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    if (header->entry_count == 0U || header->entry_count > OTA_BUNDLE_MAX_ENTRIES) {
        ESP_LOGE(TAG, "Bundle entry_count invalid: %u", (unsigned)header->entry_count);
        return ESP_ERR_INVALID_SIZE;
    }

    if (header->package_size < OTA_BUNDLE_HEADER_SIZE) {
        ESP_LOGE(TAG,
                 "Bundle package_size too small: %lu",
                 (unsigned long)header->package_size);
        return ESP_ERR_INVALID_SIZE;
    }

    return ota_bundle_validate_entries(header);
}

esp_err_t ota_bundle_parse_header(const void *data, size_t len, ota_bundle_header_t *out_header)
{
    esp_err_t err;

    if (data == NULL || out_header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len < sizeof(ota_bundle_header_t)) {
        ESP_LOGE(TAG,
                 "Bundle header buffer too small: got=%u need=%u",
                 (unsigned)len,
                 (unsigned)sizeof(ota_bundle_header_t));
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_header, data, sizeof(ota_bundle_header_t));
    err = ota_bundle_validate_header(out_header);
    if (err != ESP_OK) {
        memset(out_header, 0, sizeof(*out_header));
        return err;
    }

    return ESP_OK;
}

bool ota_bundle_entry_is_valid(const ota_bundle_header_t *header, const ota_bundle_entry_t *entry)
{
    if (header == NULL || entry == NULL) {
        return false;
    }

    return ota_bundle_payload_type_is_supported(entry->type) &&
           ota_bundle_range_is_valid(entry->offset, entry->size, header->package_size);
}

const ota_bundle_entry_t *ota_bundle_find_entry(const ota_bundle_header_t *header,
                                                ota_bundle_payload_type_t type)
{
    size_t i;

    if (header == NULL || !ota_bundle_payload_type_is_supported(type)) {
        return NULL;
    }

    for (i = 0; i < header->entry_count; ++i) {
        const ota_bundle_entry_t *entry = &header->entries[i];
        if (entry->type == (uint32_t)type && ota_bundle_entry_is_valid(header, entry)) {
            return entry;
        }
    }

    return NULL;
}
