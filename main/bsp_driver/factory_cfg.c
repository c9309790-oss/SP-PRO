#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "factory_cfg.h"

static const char *TAG = "factory_cfg";

typedef struct {
    bool init_attempted;
    bool partition_ready;
    bool sn_valid;
    char sn[FACTORY_CFG_SN_MAX_LEN];
} factory_cfg_cache_t;

static factory_cfg_cache_t s_factory_cfg = {0};

static void factory_cfg_clear_cache(void)
{
    s_factory_cfg.sn_valid = false;
    s_factory_cfg.sn[0] = '\0';
}

esp_err_t factory_cfg_init(void)
{
    esp_err_t err;
    nvs_handle_t nvs_handle;
    size_t len;

    if (s_factory_cfg.init_attempted) {
        return s_factory_cfg.partition_ready ? ESP_OK : ESP_FAIL;
    }

    s_factory_cfg.init_attempted = true;
    factory_cfg_clear_cache();

    err = nvs_flash_init_partition(FACTORY_CFG_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to init factory_cfg partition '%s': %s",
                 FACTORY_CFG_PARTITION_LABEL,
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_open_from_partition(FACTORY_CFG_PARTITION_LABEL,
                                  FACTORY_CFG_NAMESPACE,
                                  NVS_READONLY,
                                  &nvs_handle);
    if (err != ESP_OK) {
        s_factory_cfg.partition_ready = true;
        ESP_LOGW(TAG,
                 "factory_cfg namespace '%s' is not ready in partition '%s': %s",
                 FACTORY_CFG_NAMESPACE,
                 FACTORY_CFG_PARTITION_LABEL,
                 esp_err_to_name(err));
        return err;
    }

    s_factory_cfg.partition_ready = true;
    len = sizeof(s_factory_cfg.sn);
    err = nvs_get_str(nvs_handle, FACTORY_CFG_KEY_SN, s_factory_cfg.sn, &len);
    if (err == ESP_OK && s_factory_cfg.sn[0] != '\0') {
        s_factory_cfg.sn_valid = true;
        ESP_LOGI(TAG,
                 "Loaded factory_cfg SN from partition '%s': %s",
                 FACTORY_CFG_PARTITION_LABEL,
                 s_factory_cfg.sn);
    } else {
        factory_cfg_clear_cache();
        ESP_LOGW(TAG,
                 "No valid SN found in factory_cfg partition '%s', key='%s': %s",
                 FACTORY_CFG_PARTITION_LABEL,
                 FACTORY_CFG_KEY_SN,
                 esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}

bool factory_cfg_has_sn(void)
{
    (void)factory_cfg_init();
    return s_factory_cfg.sn_valid;
}

bool factory_cfg_get_sn(char *sn, size_t size)
{
    if (!sn || size == 0U) {
        return false;
    }

    (void)factory_cfg_init();
    if (!s_factory_cfg.sn_valid) {
        sn[0] = '\0';
        return false;
    }

    snprintf(sn, size, "%s", s_factory_cfg.sn);
    return true;
}

const char *factory_cfg_get_sn_ptr(void)
{
    (void)factory_cfg_init();
    return s_factory_cfg.sn_valid ? s_factory_cfg.sn : NULL;
}

void factory_cfg_log_current_sn(void)
{
    (void)factory_cfg_init();

    if (s_factory_cfg.sn_valid) {
        ESP_LOGI(TAG,
                 "Current factory_cfg SN: %s",
                 s_factory_cfg.sn);
        return;
    }

    ESP_LOGW(TAG,
             "Current factory_cfg SN is not available");
}
