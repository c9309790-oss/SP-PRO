#ifndef UART_CTR_H
#define UART_CTR_H

#include <stdbool.h>
#include <stddef.h>
#include "controller_status_types.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/queue.h"

typedef enum {
    CSV_INT,
    CSV_UINT,
    CSV_FLOAT,
    CSV_STRING,
} csv_field_type_t;

typedef struct {
    csv_field_type_t type;
    void *dst;
    uint16_t size;   /* Valid only for CSV_STRING. */
} csv_field_desc_t;

typedef void (*ctr_version_update_handler_t)(const char *version);

void ctr_uart_init(int baud_rate, int rx_pin, int tx_pin);
void ctr_uart_send_data(const char *data, int len);
void ctr_register_version_update_handler(ctr_version_update_handler_t handler);
uint32_t ctr_uart_get_last_status_tick(void);
void ctr_factory_data_persist(void);
bool ctr_factory_data_is_valid(const FLASH_FACTORY_DATA *data, char *reason, size_t reason_size);

#endif /* UART_CTR_H */
