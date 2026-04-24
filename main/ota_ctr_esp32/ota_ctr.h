#ifndef CTR_OTA_H
#define CTR_OTA_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_event.h"
#include "ota_bundle_format.h"

typedef enum uint8_t{
    IOT_SIMPEL_OTA_NULL = 0,
    IOT_SIMPLE_OTA_URL_GOT,
    IOT_SIMPLE_OTA_URL_AUTH_FAIL,
    IOT_SIMPLE_OTA_CRC_OK,
    IOT_SIMPLE_OTA_PACK_CHECK_END,
    IOT_SIMPLE_OTA_WAIT_CONFIRM,
    IOT_SIMPLE_OTA_YMODEM,
    IOT_SIMPLE_OTA_FAIL,
    IOT_SIMPLE_OTA_SUCCESS,
} IOT_SIMPLE_OTA_STA_TYP;

typedef enum {
    OTA_ERROR_NONE = 0,
    OTA_ERROR_DOWNLOAD_FAIL,
    OTA_ERROR_CRC32_ERR,
    OTA_ERROR_SERVER_CHECK_FAIL,
    OTA_ERROR_MD5_ERR,
    OTA_ERROR_APPLY_FAIL,
} OTA_ERROR_TYPE_TYP;

typedef enum {
    OTA_PACKAGE_KIND_UNKNOWN = 0,
    OTA_PACKAGE_KIND_BUNDLE,
} ota_package_kind_t;

typedef struct {
    ota_package_kind_t kind;
    uint32_t content_length;
    bool has_ctr;
    bool has_esp32;
    bool ctr_upgrade_required;
    bool esp32_upgrade_required;
    bool esp32_staged;
    bool reboot_required;
    ota_bundle_entry_t ctr_entry;
    ota_bundle_entry_t esp32_entry;
} ota_package_probe_result_t;

typedef struct {
    volatile IOT_SIMPLE_OTA_STA_TYP ota_sta;
    char ota_url[300];
    char ota_md5[33];
    char ota_file_name[100];
    char ota_dtype[100];
    char ota_stype[100];
    char ota_tkid[100];
    char ota_file_path[50];
    char ota_auto_up[2];
    char ota_msgid[100];
    volatile OTA_ERROR_TYPE_TYP ota_error_info;
} ota_info_t;

extern ota_info_t g_ota_info;

typedef enum {
    OTA_INFO_URL = 0,
    OTA_INFO_MD5,
    OTA_INFO_FILE_NAME,
    OTA_INFO_DTYPE,
    OTA_INFO_STYPE,
    OTA_INFO_TKID,
    OTA_INFO_FILE_PATH,
    OTA_INFO_AUTOUP,
    OTA_INFO_MSGID,
    OTA_INFO_STA,
    OTA_INFO_ERROR_INFO,
} OTA_INFOIDX_TYP;

esp_err_t ota_params_edit(OTA_INFOIDX_TYP idx, const void *value);
void ctr_ota_init(void);
void ctr_ota_task_run(void);
void ctr_ota_notify_wifi_ready(void);
void ctr_ota_notify_mqtt_bootstrap_ready(void);
bool ctr_ota_is_waiting_for_mqtt_bootstrap(void);
bool ctr_ota_should_block_mqtt_autostart(void);
esp_err_t ota_params_reset(void);
bool upload_ctr_ota_info(void);
void ctr_ota_clear_result_ack(void);
void ctr_ota_mark_result_ack_received(void);
uint8_t ctr_ota_is_result_ack_received(void);
const ota_package_probe_result_t *ctr_ota_get_package_probe_result(void);
bool ctr_ota_is_local_http_test_active(void);

#endif /* CTR_OTA_H */
