#ifndef BLE_PAIRING_H
#define BLE_PAIRING_H

#include "cJSON.h"
#include "esp_err.h"

typedef struct BlePairing BlePairing;

struct BlePairing {
    esp_err_t (*handle)(BlePairing *self, char *json_str);
};

BlePairing* BlePairing_GetInstance(void);
void BlePairing_Poll(void);

#endif /* BLE_PAIRING_H */
