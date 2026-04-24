#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "nvs.h"
#include "mqtt_protocol_core.h"
#include "ota_ctr.h"
#include "uart_ctr.h"

static const char *TAG = "MQTT_PROTOCOL";
static const char *OTA_LAST_RESULT_NVS = "ota_last_res";

typedef struct {
    char msg_id[100];
    char device_type[100];
    char software_type[100];
    char resource_name[100];
    char md5[33];
    char task_id[100];
    char result_msg[64];
    uint8_t result;
    bool valid;
    bool reboot_replay_pending;
} ota_last_result_report_t;

static esp_err_t ota_last_result_store(ota_result_t result, const char *msg)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_LAST_RESULT_NVS, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, "valid", 1);
    if (err == ESP_OK) err = nvs_set_u8(nvs_handle, "result", (uint8_t)result);
    if (err == ESP_OK) err = nvs_set_u8(nvs_handle, "replay", 0);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "msgid", g_ota_info.ota_msgid);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "dtype", g_ota_info.ota_dtype);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "stype", g_ota_info.ota_stype);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "fname", g_ota_info.ota_file_name);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "md5", g_ota_info.ota_md5);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "tkid", g_ota_info.ota_tkid);
    if (err == ESP_OK) err = nvs_set_str(nvs_handle, "rmsg", msg ? msg : "Update_Fail");
    if (err == ESP_OK) err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Persisted last OTA result: taskId=%s file=%s result=%u msg=%s replayPending=0",
                 g_ota_info.ota_tkid,
                 g_ota_info.ota_file_name,
                 (unsigned)result,
                 msg ? msg : "Update_Fail");
    } else {
        ESP_LOGW(TAG,
                 "Persist last OTA result failed: taskId=%s file=%s result=%u msg=%s err=%s",
                 g_ota_info.ota_tkid,
                 g_ota_info.ota_file_name,
                 (unsigned)result,
                 msg ? msg : "Update_Fail",
                 esp_err_to_name(err));
    }
    return err;
}

static bool ota_last_result_load(ota_last_result_report_t *report)
{
    nvs_handle_t nvs_handle;
    uint8_t valid = 0;
    uint8_t replay = 0;
    size_t len;
    esp_err_t err;

    if (!report) {
        return false;
    }

    memset(report, 0, sizeof(*report));

    err = nvs_open(OTA_LAST_RESULT_NVS, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_get_u8(nvs_handle, "valid", &valid);
    if (err != ESP_OK || valid == 0) {
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_u8(nvs_handle, "result", &report->result);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }

    if (nvs_get_u8(nvs_handle, "replay", &replay) != ESP_OK) {
        replay = 0;
    }

    len = sizeof(report->msg_id);
    if (nvs_get_str(nvs_handle, "msgid", report->msg_id, &len) != ESP_OK) report->msg_id[0] = '\0';
    len = sizeof(report->device_type);
    if (nvs_get_str(nvs_handle, "dtype", report->device_type, &len) != ESP_OK) report->device_type[0] = '\0';
    len = sizeof(report->software_type);
    if (nvs_get_str(nvs_handle, "stype", report->software_type, &len) != ESP_OK) report->software_type[0] = '\0';
    len = sizeof(report->resource_name);
    if (nvs_get_str(nvs_handle, "fname", report->resource_name, &len) != ESP_OK) report->resource_name[0] = '\0';
    len = sizeof(report->md5);
    if (nvs_get_str(nvs_handle, "md5", report->md5, &len) != ESP_OK) report->md5[0] = '\0';
    len = sizeof(report->task_id);
    if (nvs_get_str(nvs_handle, "tkid", report->task_id, &len) != ESP_OK) report->task_id[0] = '\0';
    len = sizeof(report->result_msg);
    if (nvs_get_str(nvs_handle, "rmsg", report->result_msg, &len) != ESP_OK) report->result_msg[0] = '\0';

    report->valid = true;
    report->reboot_replay_pending = replay != 0;
    nvs_close(nvs_handle);
    ESP_LOGI(TAG,
             "Loaded persisted last OTA result: taskId=%s file=%s result=%u msg=%s replayPending=%u",
             report->task_id,
             report->resource_name,
             (unsigned)report->result,
             report->result_msg,
             report->reboot_replay_pending ? 1U : 0U);
    return true;
}

static void ota_last_result_set_reboot_replay_pending(bool pending, const char *reason)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_LAST_RESULT_NVS, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Set persisted OTA reboot replay flag skipped: open failed, reason=%s err=%s",
                 reason ? reason : "",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs_handle, "replay", pending ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Set persisted OTA reboot replay flag=%u, reason=%s",
                 pending ? 1U : 0U,
                 reason ? reason : "");
    } else {
        ESP_LOGW(TAG,
                 "Set persisted OTA reboot replay flag failed, reason=%s err=%s",
                 reason ? reason : "",
                 esp_err_to_name(err));
    }
}

static void ota_last_result_clear(const char *reason)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(OTA_LAST_RESULT_NVS, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Clear persisted last OTA result skipped: open namespace failed, reason=%s err=%s",
                 reason ? reason : "",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Cleared persisted last OTA result, reason=%s",
                 reason ? reason : "");
    } else {
        ESP_LOGW(TAG,
                 "Clear persisted last OTA result failed, reason=%s err=%s",
                 reason ? reason : "",
                 esp_err_to_name(err));
    }
}

static void publish_ota_result_payload(const char *msg_id,
                                       const char *device_type,
                                       const char *software_type,
                                       const char *resource_name,
                                       const char *md5,
                                       const char *task_id,
                                       ota_result_t result,
                                       const char *msg)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id, NULL, &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "update/kalerm/iot/software/result/feedback/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "deviceType", device_type ? device_type : "");
    cJSON_AddNumberToObject(res, "softwareType", atoi(software_type ? software_type : "0"));
    cJSON_AddStringToObject(res, "resourceName", resource_name ? resource_name : "");
    cJSON_AddStringToObject(res, "md5", md5 ? md5 : "");

    if (task_id && task_id[0] != 0)
    {
        cJSON_AddStringToObject(res, "sign", md5 ? md5 : "");
        cJSON_AddStringToObject(res, "signMethod", "MD5");
        cJSON_AddStringToObject(res, "upgradeTaskId", task_id);
    }

    cJSON_AddNumberToObject(res, "updateResult", result);
    if (msg != 0)
        cJSON_AddStringToObject(res, "updateResultMsg", msg);
    else
        cJSON_AddStringToObject(res, "updateResultMsg", "Update_Fail");

    cJSON_AddItemToObject(data, "softwareUpdateResult", res);
    mqtt_publish_json_root(root);
}

static int ota_parse_ack_code(cJSON *obj)
{
    cJSON *item = cJSON_GetObjectItem(obj, "ackCode");

    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }

    if (item && cJSON_IsString(item) && item->valuestring) {
        char *endptr = NULL;

        if (strcmp(item->valuestring, "OK") == 0) {
            return 0;
        }
        if (strcmp(item->valuestring, "ERR") == 0) {
            return -1;
        }

        {
            long parsed = strtol(item->valuestring, &endptr, 10);
            if (endptr != item->valuestring && endptr && *endptr == '\0') {
                return (int)parsed;
            }
        }
        return -1;
    }

    return -1;
}

static bool ota_task_matches_current(const char *task_id)
{
    if (!task_id || task_id[0] == 0 || g_ota_info.ota_tkid[0] == 0) {
        return true;
    }

    return strcmp(task_id, g_ota_info.ota_tkid) == 0;
}

static int ota_get_ready_status(void)
{
    /* New-version machines follow the protocol: readyStatus=0 means the popup should wait until package validation is complete. */
    return 0;
}
static bool ota_task_is_active(void)
{
    extern uint8_t ctr_ota_run_flag;

    return ctr_ota_run_flag &&
           (g_ota_info.ota_sta == IOT_SIMPLE_OTA_URL_GOT ||
            g_ota_info.ota_sta == IOT_SIMPLE_OTA_URL_AUTH_FAIL ||
            g_ota_info.ota_sta == IOT_SIMPLE_OTA_CRC_OK ||
            g_ota_info.ota_sta == IOT_SIMPLE_OTA_PACK_CHECK_END ||
            g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM ||
            g_ota_info.ota_sta == IOT_SIMPLE_OTA_YMODEM);
}

static bool ota_is_duplicate_task_payload(const char *task_id,
                                          const char *url,
                                          const char *md5,
                                          const char *name,
                                          int sw_type)
{
    char sw_type_buf[16];
    bool same_payload;

    if (!task_id || task_id[0] == 0 || g_ota_info.ota_tkid[0] == 0) {
        return false;
    }

    if (strcmp(task_id, g_ota_info.ota_tkid) != 0) {
        return false;
    }

    snprintf(sw_type_buf, sizeof(sw_type_buf), "%d", sw_type);
    same_payload = strcmp(url ? url : "", g_ota_info.ota_url) == 0 &&
                   strcmp(md5 ? md5 : "", g_ota_info.ota_md5) == 0 &&
                   strcmp(name ? name : "", g_ota_info.ota_file_name) == 0 &&
                   strcmp(sw_type_buf, g_ota_info.ota_stype) == 0;

    if (!same_payload) {
        ESP_LOGW(TAG,
                 "Treat OTA info as duplicate by taskId while payload changed taskId=%s currentState=%d currentUrl=%s incomingUrl=%s",
                 task_id,
                 g_ota_info.ota_sta,
                 g_ota_info.ota_url,
                 url ? url : "");
    }

    return same_payload;
}

void update_ctr_version(const char *version)
{
    snprintf(g_device_version.current_ctr_fw_version,
             sizeof(g_device_version.current_ctr_fw_version),
             "%s",
             version ? version : "");
}

void publish_ctr_ota_check_status_to_mqtt(void)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(g_ota_info.ota_msgid, NULL, &data);
    char sub_topic[200];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "update/kalerm/iot/checkSoftwareStatus/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *status_ack = cJSON_CreateObject();
    cJSON_AddStringToObject(status_ack, "resourceName", g_ota_info.ota_file_name);
    cJSON_AddStringToObject(status_ack, "md5", g_ota_info.ota_md5);
    if (g_ota_info.ota_tkid[0] != 0)
    {
        cJSON_AddStringToObject(status_ack, "sign", g_ota_info.ota_md5);
        cJSON_AddStringToObject(status_ack, "signMethod", "MD5");
        cJSON_AddStringToObject(status_ack, "upgradeTaskId", g_ota_info.ota_tkid);
    }
    cJSON_AddItemToObject(data, "checkSoftwareStatus", status_ack);
    mqtt_publish_json_root(root);
}

void publish_ota_info_ack_to_mqtt(const char *msg_id, const char *task_id)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id, "0", &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "update/kalerm/iot/software/result/ack/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(ack, "ackCode", 0);
    cJSON_AddStringToObject(ack, "msg", "OK");
    cJSON_AddStringToObject(ack, "upgradeTaskId", task_id ? task_id : "");
    cJSON_AddNumberToObject(ack, "readyStatus", ota_get_ready_status());

    cJSON_AddItemToObject(data, "softwareUpdateInfoAck", ack);
    mqtt_publish_json_root(root);
}

static void parse_ota_info_from_mqtt(const char *msg_id, cJSON *info)
{
    const char *url = json_get_string(info, "resourceUrl", "");
    const char *md5 = json_get_string(info, "md5", "");
    const char *name = json_get_string(info, "resourceName", "");
    const char *task = json_get_string(info, "upgradeTaskId", "");
    const char *dev_type = json_get_string(info, "deviceType", "");
    bool ota_task_active = ota_task_is_active();
    bool duplicate_task_payload;
    int auto_flag = json_get_int(info, "autoUpdateFlag", 0);
    int sw_type = json_get_int(info, "softwareType", 0);

    duplicate_task_payload = ota_task_active &&
                             ota_is_duplicate_task_payload(task, url, md5, name, sw_type);

    ESP_LOGI(TAG,
             "OTA info received msgId=%s taskId=%s auto=%d swType=%d state=%d active=%d duplicate=%d url=%s",
             msg_id ? msg_id : "",
             task,
             auto_flag,
             sw_type,
             g_ota_info.ota_sta,
             ota_task_active,
             duplicate_task_payload,
             url);

    ota_last_result_clear("new_software_update_info");
    publish_ota_info_ack_to_mqtt(msg_id, task);

    if (duplicate_task_payload)
    {
        ESP_LOGW(TAG,
                 "Ignore duplicate OTA info for active taskId=%s currentMsgId=%s incomingMsgId=%s state=%d",
                 task,
                 g_ota_info.ota_msgid,
                 msg_id ? msg_id : "",
                 g_ota_info.ota_sta);
        return;
    }

    ctr_ota_clear_result_ack();

    {
        char buf[16];
        ota_params_edit(OTA_INFO_MSGID, msg_id ? msg_id : "");
        ota_params_edit(OTA_INFO_URL, url);
        ota_params_edit(OTA_INFO_MD5, md5);
        ota_params_edit(OTA_INFO_FILE_NAME, name);
        ota_params_edit(OTA_INFO_TKID, task);
        ota_params_edit(OTA_INFO_DTYPE, dev_type);
        ota_params_edit(OTA_INFO_FILE_PATH, "");
        sprintf(buf, "%d", auto_flag);
        ota_params_edit(OTA_INFO_AUTOUP, buf);
        sprintf(buf, "%d", sw_type);
        ota_params_edit(OTA_INFO_STYPE, buf);
    }

    if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM)
    {
        ctr_uart_send_data("+EVENT=NEW_OTA_URL\r\n", strlen("+EVENT=NEW_OTA_URL\r\n"));
    }

    {
        extern uint8_t g_ota_new_url_pending;

        if (ota_task_active)
        {
            ESP_LOGW(TAG,
                     "OTA task active, mark pending new task taskId=%s incomingMsgId=%s currentState=%d",
                     task,
                     msg_id ? msg_id : "",
                     g_ota_info.ota_sta);
            g_ota_new_url_pending = 1;
        }
        else
        {
            ESP_LOGI(TAG, "OTA task idle, accept new task immediately taskId=%s msgId=%s", task, msg_id ? msg_id : "");
            g_ota_new_url_pending = 0;
            {
                uint8_t sta = IOT_SIMPLE_OTA_URL_GOT;
                ota_params_edit(OTA_INFO_STA, (char *)&sta);
            }
        }
    }

    {
        uint8_t error_none = OTA_ERROR_NONE;
        ota_params_edit(OTA_INFO_ERROR_INFO, (char *)&error_none);
    }

    if (auto_flag == 1)
    {
        ESP_LOGI(TAG, "auto OTA start");
    }

    ctr_ota_task_run();
}

static void parse_ota_check_result_from_mqtt(cJSON *update)
{
    int ack_code = ota_parse_ack_code(update);

    if (g_ota_info.ota_sta != IOT_SIMPLE_OTA_CRC_OK) {
        ESP_LOGW(TAG,
                 "Ignore OTA server check result ack=%d because state=%d is no longer CRC_OK",
                 ack_code,
                 g_ota_info.ota_sta);
        return;
    }

    if (ack_code == 0)
    {
        ESP_LOGI(TAG, "OTA server check OK taskId=%s state=%d", g_ota_info.ota_tkid, g_ota_info.ota_sta);

        uint8_t sta = IOT_SIMPLE_OTA_PACK_CHECK_END;
        ota_params_edit(OTA_INFO_STA, (char *)&sta);
    }
    else
    {
        ESP_LOGE(TAG, "OTA server check fail ack=%d taskId=%s state=%d", ack_code, g_ota_info.ota_tkid, g_ota_info.ota_sta);

        uint8_t sta = IOT_SIMPLE_OTA_FAIL;
        uint8_t error_server = OTA_ERROR_SERVER_CHECK_FAIL;
        ota_params_edit(OTA_INFO_ERROR_INFO, (char *)&error_server);
        ota_params_edit(OTA_INFO_STA, (char *)&sta);
    }
}

static void parse_ota_result_ack_from_mqtt(cJSON *ack)
{
    const char *task = json_get_string(ack, "upgradeTaskId", "");
    int code = ota_parse_ack_code(ack);

    if (!ota_task_matches_current(task)) {
        ESP_LOGW(TAG, "Ignore OTA result ack for other task: current=%s incoming=%s",
                 g_ota_info.ota_tkid,
                 task ? task : "");
        return;
    }

    ESP_LOGI(TAG, "OTA result ack=%d taskId=%s state=%d", code, g_ota_info.ota_tkid, g_ota_info.ota_sta);
    if (code == 0) {
        if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_FAIL ||
            g_ota_info.ota_sta == IOT_SIMPLE_OTA_SUCCESS) {
            if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_SUCCESS) {
                ota_last_result_set_reboot_replay_pending(true, "software_update_result_ack_success");
            } else {
                ota_last_result_clear("software_update_result_ack");
            }
            ctr_ota_mark_result_ack_received();
        } else {
            ota_last_result_clear("software_update_result_ack_non_terminal");
            ESP_LOGI(TAG, "Ignore non-terminal OTA result ack while state=%d", g_ota_info.ota_sta);
        }
    }
}

void publish_ota_result_to_mqtt(ota_result_t result, const char *msg)
{
    esp_err_t store_err = ota_last_result_store(result, msg);
    if (store_err != ESP_OK) {
        ESP_LOGW(TAG, "Persist last OTA result failed: %s", esp_err_to_name(store_err));
    }

    publish_ota_result_payload(g_ota_info.ota_msgid,
                               g_ota_info.ota_dtype,
                               g_ota_info.ota_stype,
                               g_ota_info.ota_file_name,
                               g_ota_info.ota_md5,
                               g_ota_info.ota_tkid,
                               result,
                               msg);
}

bool publish_last_ota_result_to_mqtt(void)
{
    ota_last_result_report_t report;

    if (!ota_last_result_load(&report)) {
        ESP_LOGI(TAG, "No persisted last OTA result available for MQTT upload");
        return false;
    }

    if (report.result == OTA_RESULT_SUCCESS && !report.reboot_replay_pending) {
        ESP_LOGI(TAG,
                 "Skip persisted last OTA success result upload because reboot replay is not pending");
        return false;
    }

    ESP_LOGI(TAG,
             "Publish persisted last OTA result: taskId=%s file=%s result=%u msg=%s replayPending=%u",
             report.task_id,
             report.resource_name,
             (unsigned)report.result,
             report.result_msg,
             report.reboot_replay_pending ? 1U : 0U);

    publish_ota_result_payload(report.msg_id,
                               report.device_type,
                               report.software_type,
                               report.resource_name,
                               report.md5,
                               report.task_id,
                               (ota_result_t)report.result,
                               report.result_msg);
    if (report.result == OTA_RESULT_SUCCESS && report.reboot_replay_pending) {
        ota_last_result_set_reboot_replay_pending(false, "publish_last_ota_result_to_mqtt");
    }
    return true;
}

static void parse_ota_new_url_from_mqtt(const char *msg_id, cJSON *new_url)
{
    const char *task = json_get_string(new_url, "upgradeTaskId", "");
    const char *url = json_get_string(new_url, "resourceUrl", "");
    if (!url || url[0] == 0) {
        return;
    }

    if (!ota_task_matches_current(task)) {
        ESP_LOGW(TAG, "Ignore OTA new url for other task: current=%s incoming=%s",
                 g_ota_info.ota_tkid,
                 task ? task : "");
        return;
    }

    ESP_LOGI(TAG, "OTA new url received msgId=%s taskId=%s state=%d url=%s", msg_id ? msg_id : "", task ? task : "", g_ota_info.ota_sta, url);

    ctr_ota_clear_result_ack();
    ota_params_edit(OTA_INFO_MSGID, msg_id ? msg_id : g_ota_info.ota_msgid);
    ota_params_edit(OTA_INFO_URL, url);

    {
        uint8_t error_none = OTA_ERROR_NONE;
        uint8_t sta = IOT_SIMPLE_OTA_URL_GOT;
        ota_params_edit(OTA_INFO_ERROR_INFO, (char *)&error_none);
        ota_params_edit(OTA_INFO_STA, (char *)&sta);
    }

    ctr_ota_task_run();
}

void publish_confirm_ack_to_mqtt(const char *msg_id, int ack_code)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(msg_id, "0", &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "update/kalerm/iot/software/confirm/ack/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(ack, "ackCode", ack_code);

    cJSON_AddItemToObject(data, "remoteUpdateConfirm", ack);
    mqtt_publish_json_root(root);
}

static void parse_ota_confirm_from_mqtt(const char *msg_id, cJSON *confirm)
{
    const char *task = json_get_string(confirm, "upgradeTaskId", "");

    ESP_LOGI(TAG, "OTA confirm start msgId=%s taskId=%s currentTaskId=%s state=%d", msg_id ? msg_id : "", task ? task : "", g_ota_info.ota_tkid, g_ota_info.ota_sta);

    if (!ota_task_matches_current(task))
    {
        ESP_LOGW(TAG, "Ignore OTA confirm for other task: current=%s incoming=%s",
                 g_ota_info.ota_tkid,
                 task ? task : "");
        publish_confirm_ack_to_mqtt(msg_id, 1);
        return;
    }

    if (g_ota_info.ota_sta == IOT_SIMPLE_OTA_WAIT_CONFIRM)
    {
        ESP_LOGI(TAG, "OTA CONFIRM accepted taskId=%s, mark OTA start pending", task);
        publish_confirm_ack_to_mqtt(msg_id, 0);
        publish_ota_status_to_mqtt(OTA_YES_CONFIRM);
        ota_params_edit(OTA_INFO_AUTOUP, "9");
    }
    else
    {
        ESP_LOGE(TAG, "OTA CONFIRM rejected state=%d taskId=%s currentTaskId=%s", g_ota_info.ota_sta, task, g_ota_info.ota_tkid);
        publish_confirm_ack_to_mqtt(msg_id, 1);
    }
    ctr_ota_task_run();
}

void publish_ota_status_to_mqtt(ota_status_t status)
{
    cJSON *data = NULL;
    cJSON *root = mqtt_create_message_root(g_ota_info.ota_msgid, "0", &data);
    char sub_topic[128];
    mqtt_build_client_topic(sub_topic, sizeof(sub_topic), "update/kalerm/iot/software/status/feedback/%s");
    mqtt_add_sub_topic(data, sub_topic);

    cJSON *software_update_status = cJSON_CreateObject();
    cJSON_AddNumberToObject(software_update_status, "updateStatus", status);
    cJSON_AddItemToObject(data, "softwareUpdateStatus", software_update_status);
    mqtt_publish_json_root(root);
}

bool mqtt_handle_ota_data_from_mqtt(const char *msg_id, cJSON *data)
{
    cJSON *remote_update = json_get_object(data, "remoteUpdate");
    cJSON *confirm_update = json_get_object(data, "confirmUpdate");
    cJSON *software_update_result_ack = json_get_object(data, "softwareUpdateResultAck");

    if (remote_update)
    {
        cJSON *info = json_get_object(remote_update, "softwareUpdateInfo");
        cJSON *update = json_get_object(remote_update, "softwareUpdate");
        cJSON *new_url = json_get_object(remote_update, "newResourceUrl");

        if (info)
            parse_ota_info_from_mqtt(msg_id, info);
        else if (update)
            parse_ota_check_result_from_mqtt(update);
        else if (new_url)
            parse_ota_new_url_from_mqtt(msg_id, new_url);
        else
            return false;

        return true;
    }

    if (confirm_update)
    {
        parse_ota_confirm_from_mqtt(msg_id, confirm_update);
        return true;
    }

    if (software_update_result_ack)
    {
        parse_ota_result_ack_from_mqtt(software_update_result_ack);
        return true;
    }

    return false;
}








