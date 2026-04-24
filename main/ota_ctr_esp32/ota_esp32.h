#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H
#include <stddef.h>
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_err.h"
#include "ota_bundle_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP event handler for OTA update.
 * 
 * Handles various events that occur during the OTA update process.
 * 
 * @param evt HTTP client event
 * @return esp_err_t ESP_OK on success, otherwise an error code indicating the cause of the failure
 */
esp_err_t _http_event_handler(esp_http_client_event_t *evt);

/**
 * @brief Perform OTA update.
 * 
 * Initiates an OTA update by downloading the firmware from the specified URL.
 * 
 * @param url URL of the firmware to download
 * @return esp_err_t ESP_OK on success, otherwise an error code indicating the cause of the failure
 */
esp_err_t perform_ota(const char *url);
esp_err_t perform_ota_from_bundle_url(const char *url,
                                      const ota_bundle_entry_t *entry,
                                      const char *expected_package_md5,
                                      char *package_md5_str,
                                      char *payload_md5_str);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_H */


