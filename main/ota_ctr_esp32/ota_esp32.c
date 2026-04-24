#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_esp32.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "mbedtls/md5.h"
#include <stddef.h>
#include <string.h>

static const char *TAG = "ota_update";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");

#define OTA_BUNDLE_CHUNK_SIZE 1024

esp_err_t _http_event_handler(esp_http_client_event_t *event)
{
    switch (event->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", event->header_key, event->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", event->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }

    return ESP_OK;
}

esp_err_t perform_ota(const char *url)
{
    ESP_LOGI(TAG, "Starting OTA update from %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        // .ciphersuites_list = klm_ETSI_EN_303645_ciphersuites_list,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t result = esp_https_ota(&ota_config);
    return result;
}

static void ota_format_md5(char *md5_str, size_t md5_str_size, const unsigned char digest[16])
{
    static const char hex[] = "0123456789abcdef";
    size_t idx;

    if (md5_str == NULL || md5_str_size < 33U) {
        return;
    }

    for (idx = 0; idx < 16U; ++idx) {
        md5_str[idx * 2U] = hex[(digest[idx] >> 4) & 0x0FU];
        md5_str[idx * 2U + 1U] = hex[digest[idx] & 0x0FU];
    }
    md5_str[32] = '\0';
}

static bool ota_md5_matches(const char *actual_md5, const char *expected_md5)
{
    if (expected_md5 == NULL || expected_md5[0] == '\0') {
        return true;
    }

    return strcasecmp(actual_md5, expected_md5) == 0;
}

esp_err_t perform_ota_from_bundle_url(const char *url,
                                      const ota_bundle_entry_t *entry,
                                      const char *expected_package_md5,
                                      char *package_md5_str,
                                      char *payload_md5_str)
{
    esp_err_t err = ESP_OK;
    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = NULL;
    const esp_partition_t *update_partition = NULL;
    esp_ota_handle_t ota_handle = 0;
    bool ota_begun = false;
    mbedtls_md5_context package_md5_ctx;
    mbedtls_md5_context payload_md5_ctx;
    unsigned char package_md5_digest[16] = {0};
    unsigned char payload_md5_digest[16] = {0};
    char buffer[OTA_BUNDLE_CHUNK_SIZE];
    size_t absolute_offset = 0;
    size_t written = 0;
    char expected_payload_md5[33] = {0};

    if (url == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (package_md5_str != NULL) {
        package_md5_str[0] = '\0';
    }
    if (payload_md5_str != NULL) {
        payload_md5_str[0] = '\0';
    }

    ota_format_md5(expected_payload_md5, sizeof(expected_payload_md5), entry->md5);

    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for merged ESP32 OTA");
        return ESP_FAIL;
    }

    mbedtls_md5_init(&package_md5_ctx);
    mbedtls_md5_init(&payload_md5_ctx);
    mbedtls_md5_starts(&package_md5_ctx);
    mbedtls_md5_starts(&payload_md5_ctx);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for merged ESP32 OTA: %s", esp_err_to_name(err));
        goto cleanup;
    }

    {
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);

        if (status_code != 200) {
            ESP_LOGE(TAG, "Unexpected HTTP status for merged ESP32 OTA: %d", status_code);
            err = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }

        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed for merged ESP32 OTA: %d", content_length);
            err = ESP_FAIL;
            goto cleanup;
        }
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No next OTA update partition found for merged ESP32 OTA");
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (entry->size >= update_partition->size) {
        ESP_LOGE(TAG,
                 "Merged ESP32 payload too large payload=%lu partition=%lu",
                 (unsigned long)entry->size,
                 (unsigned long)update_partition->size);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = esp_ota_begin(update_partition, entry->size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed for merged ESP32 OTA: %s", esp_err_to_name(err));
        goto cleanup;
    }
    ota_begun = true;

    while (1) {
        int data_read = esp_http_client_read(client, buffer, sizeof(buffer));
        if (data_read < 0) {
            ESP_LOGE(TAG, "Merged ESP32 OTA read failed");
            err = ESP_FAIL;
            goto cleanup;
        }

        if (data_read == 0) {
            break;
        }

        mbedtls_md5_update(&package_md5_ctx,
                           (const unsigned char *)buffer,
                           (size_t)data_read);

        {
            size_t chunk_begin = absolute_offset;
            size_t chunk_end = absolute_offset + (size_t)data_read;
            size_t payload_begin = entry->offset;
            size_t payload_end = entry->offset + entry->size;

            if (chunk_begin < payload_end && chunk_end > payload_begin) {
                size_t slice_begin = chunk_begin > payload_begin ? chunk_begin : payload_begin;
                size_t slice_end = chunk_end < payload_end ? chunk_end : payload_end;
                size_t slice_offset = slice_begin - chunk_begin;
                size_t slice_len = slice_end - slice_begin;

                err = esp_ota_write(ota_handle, buffer + slice_offset, slice_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_write failed for merged ESP32 OTA: %s", esp_err_to_name(err));
                    goto cleanup;
                }

                mbedtls_md5_update(&payload_md5_ctx, (const unsigned char *)(buffer + slice_offset), slice_len);
                written += slice_len;
            }
        }

        absolute_offset += (size_t)data_read;
    }

    if (written != entry->size) {
        ESP_LOGE(TAG,
                 "Merged ESP32 payload size mismatch written=%u expected=%lu",
                 (unsigned)written,
                 (unsigned long)entry->size);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    mbedtls_md5_finish(&package_md5_ctx, package_md5_digest);
    mbedtls_md5_finish(&payload_md5_ctx, payload_md5_digest);
    ota_format_md5(package_md5_str, 33, package_md5_digest);
    ota_format_md5(payload_md5_str, 33, payload_md5_digest);

    if (!ota_md5_matches(package_md5_str, expected_package_md5)) {
        ESP_LOGE(TAG,
                 "Merged ESP32 package md5 mismatch actual=%s expected=%s",
                 package_md5_str,
                 expected_package_md5 ? expected_package_md5 : "");
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    if (!ota_md5_matches(payload_md5_str, expected_payload_md5)) {
        ESP_LOGE(TAG,
                 "Merged ESP32 payload md5 mismatch actual=%s expected=%s",
                 payload_md5_str,
                 expected_payload_md5);
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed for merged ESP32 OTA: %s", esp_err_to_name(err));
        goto cleanup;
    }
    ota_begun = false;

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed for merged ESP32 OTA: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Merged ESP32 OTA staged successfully partition=%s packageMd5=%s payloadMd5=%s",
             update_partition->label,
             package_md5_str ? package_md5_str : "",
             payload_md5_str ? payload_md5_str : "");
    ESP_LOGW(TAG,
             "HMI OTA SUCCESS!!!!!! partition=%s packageMd5=%s payloadMd5=%s",
             update_partition->label,
             package_md5_str ? package_md5_str : "",
             payload_md5_str ? payload_md5_str : "");

cleanup:
    if (ota_begun) {
        esp_ota_abort(ota_handle);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    mbedtls_md5_free(&package_md5_ctx);
    mbedtls_md5_free(&payload_md5_ctx);
    return err;
}

