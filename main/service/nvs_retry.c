#include "nvs_retry.h"

#include "esp_log.h"

esp_err_t nvs_retry_rewrite_after_erasing_keys(nvs_handle_t nvs_handle,
                                               esp_err_t err,
                                               const char *tag,
                                               const char *label,
                                               const char *const *keys,
                                               size_t key_count,
                                               nvs_retry_rewrite_fn_t rewrite_fn,
                                               void *ctx)
{
    if (err != ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return err;
    }

    if (nvs_handle == 0 || !tag || !label || !keys || key_count == 0 || !rewrite_fn) {
        return err;
    }

    ESP_LOGW(tag,
             "%s nvs full, erase %u key(s) and retry",
             label,
             (unsigned int)key_count);

    for (size_t i = 0; i < key_count; ++i) {
        esp_err_t erase_err;

        if (!keys[i] || keys[i][0] == '\0') {
            continue;
        }

        erase_err = nvs_erase_key(nvs_handle, keys[i]);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(tag,
                     "%s erase key[%u]=%s failed: %s",
                     label,
                     (unsigned int)i,
                     keys[i],
                     esp_err_to_name(erase_err));
            return erase_err;
        }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(tag, "%s commit after erase failed: %s", label, esp_err_to_name(err));
        return err;
    }

    err = rewrite_fn(nvs_handle, ctx);
    if (err != ESP_OK) {
        ESP_LOGW(tag, "%s rewrite failed after erase: %s", label, esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(tag, "%s commit after rewrite failed: %s", label, esp_err_to_name(err));
    }
    return err;
}
