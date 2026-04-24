#ifndef WIFI_H
#define WIFI_H

#include "esp_wifi.h"
#define WIFI_SCAN_RESULT_MAX 10
#define DEFAULT_SCAN_LIST_SIZE WIFI_SCAN_RESULT_MAX

typedef enum {
    WIFI_SCAN_STATE_IDLE = 0,
    WIFI_SCAN_STATE_SCANNING = 1,
    WIFI_SCAN_STATE_READY = 2,
    WIFI_SCAN_STATE_FAILED = 3,
} wifi_scan_state_t;

extern uint16_t number;
extern wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
extern char wifi_ssid[32];
extern char wifi_password[64];
extern char wifi_ip[64],wifi_mask[64],wifi_gw[64];
void wifi_scan(void);
void wifi_init(void);
void save_wifi_info_to_nvs(const char *ssid, const char *password);
void wifi_ssid_set(char *ssid);
void wifi_password_set(char *password);
void wifi_credentials_set(const char *ssid, const char *password);
int wifi_transform_rssi(int rssi_dbm);
bool wifi_is_runtime_online(void);
bool wifi_has_saved_credentials(void);
void wifi_enter_reprovision_mode(void);
void wifi_exit_reprovision_mode(bool reconnect_saved);
bool wifi_is_reprovision_mode(void);

extern char *WIFI_scan_ssid[WIFI_SCAN_RESULT_MAX];
extern int WIFI_CNT;
extern uint8_t wifi_scan_flag;
wifi_scan_state_t WIFI_scan_get_state(void);
void WIFI_scan_thread_start(void);
void WIFI_scan_free(void);
esp_err_t WIFI_scan_request(void);
#endif 
