#ifndef YMODEM_ESP_H
#define YMODEM_ESP_H

#include "esp_event.h"

/* Send the selected OTA image over the controller UART with YMODEM. */
int ymodem_send_file(const char *filename, int file_size);
int ymodem_receive_ack(void);

#endif /* YMODEM_ESP_H */

