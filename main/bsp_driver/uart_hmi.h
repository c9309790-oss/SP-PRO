#ifndef UART_HMI_H
#define UART_HMI_H

#include "driver/uart.h"
#include "freertos/queue.h"
#include "sp_pro_proto.h"

/* Queue carrying raw text packets received from the HMI UART. */
extern QueueHandle_t uart_rx_queue[1];

/* Initialize the HMI UART interface. */
void hmi_uart_init(int baud_rate, int rx_pin, int tx_pin);
void ctr_power_io_enable(void);

/* Send text to the HMI UART. */
void uart_send_str(const char *data);
void uart_send_str_direct(const char *data);

#endif /* UART_HMI_H */

