#include "driver/uart.h"
#include "esp_log.h"
#include "Ymodem_esp.h"
#include <string.h>
#include "esp_ota_ops.h"
#include "board_config.h"
#include "uart_ctr.h"

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define C   0x43

static const char *TAG = "YMODEM";

/* Calculate the CRC16 checksum for a YMODEM payload. */
static uint16_t crc16(const uint8_t *data, uint16_t size)
{
    uint16_t crc = 0;

    while (size--) {
        crc ^= (*data++ << 8);
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/* Send raw YMODEM bytes through the controller RS485 UART. */
static int ymodem_uart_send_data(const uint8_t *data, int length)
{
    ESP_LOGD(TAG, "Sending %d bytes via CTR UART", length);
    uart_flush_input(CTR_UART_NUM);
    ctr_uart_send_data((const char *)data, length);
    uart_flush_input(CTR_UART_NUM);
    return length;
}

/* Read raw bytes from the controller RS485 UART. */
static int ymodem_uart_receive_data(uint8_t *data, int length, int timeout_ms)
{
    int bytes_received = uart_read_bytes(CTR_UART_NUM, data, length, timeout_ms / portTICK_PERIOD_MS);
    ESP_LOGD(TAG, "Received %d bytes via CTR UART", bytes_received);
    return bytes_received;
}

static void ymodem_send_packet(const uint8_t *data, int length)
{
    ymodem_uart_send_data(data, length);
}

static int ymodem_receive_control_response(const char *phase, int packet_number)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);

    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t response = 0;
        TickType_t remain = deadline - xTaskGetTickCount();
        int timeout_ms = (int)(remain * portTICK_PERIOD_MS);
        int length;

        if (timeout_ms <= 0) {
            break;
        }

        length = ymodem_uart_receive_data(&response, 1, timeout_ms);
        if (length <= 0) {
            continue;
        }

        if (response == ACK || response == NAK || response == CAN || response == C || response == EOT) {
            ESP_LOGD(TAG, "Received control response: 0x%02X", response);
            return response;
        }

        ESP_LOGD(TAG, "Ignore echoed/non-control byte during YMODEM wait: 0x%02X", response);
    }

    if (packet_number > 0) {
        ESP_LOGW(TAG,
                 "YMODEM phase '%s' packet=%d: no control response received (timeout)",
                 phase ? phase : "unknown",
                 packet_number);
    } else {
        ESP_LOGW(TAG,
                 "YMODEM phase '%s': no control response received (timeout)",
                 phase ? phase : "unknown");
    }
    return -1;
}

static const esp_partition_t *get_unused_ota_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *configured = esp_ota_get_boot_partition();

    if (configured != running) {
        return configured;
    }

    const esp_partition_t *ota_0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota_1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);

    if (ota_0 != running) {
        ESP_LOGI(TAG, "Using OTA partition %p", ota_0);
        return ota_0;
    }

    ESP_LOGI(TAG, "Using OTA partition %p", ota_1);
    return ota_1;
}

/* Send the selected OTA partition image with the YMODEM protocol. */
int ymodem_send_file(const char *filename, int file_size)
{
    uint8_t packet[1024 + 5];
    int sequence = 1;
    int attempts = 0;
    int name_length = 0;
    int retry_count = 0;

    ESP_LOGI(TAG, "Starting YMODEM file transfer: %s, size: %d bytes", filename, file_size);

    while (1) {
        int response = ymodem_receive_control_response("wait-initial-C", 0);
        if (response == C) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        if (retry_count > 100) {
            ESP_LOGE(TAG, "Failed to receive 'C' from receiver after %d attempts", retry_count);
            return -1;
        }
        retry_count++;
    }

    memset(packet, 0, sizeof(packet));
    packet[0] = SOH;
    packet[1] = 0x00;
    packet[2] = 0xFF;
    name_length = snprintf((char *)&packet[3], 128, "%s", filename);
    snprintf((char *)&packet[3 + name_length + 1], 128 - name_length - 1, "%d", file_size);

    uint16_t crc = crc16(&packet[3], 128);
    packet[131] = (crc >> 8) & 0xFF;
    packet[132] = crc & 0xFF;

    attempts = 0;
    do {
        int response = 0;
        int wait_c_attempts = 0;

        ymodem_send_packet(packet, 133);
        response = ymodem_receive_control_response("wait-header-ack", 0);
        if (response == ACK) {
            while (wait_c_attempts < 3) {
                response = ymodem_receive_control_response("wait-data-start-C", 0);
                if (response == C) {
                    break;
                }

                ESP_LOGW(TAG, "YMODEM phase 'wait-data-start-C': retry attempt %d", wait_c_attempts + 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_c_attempts++;
            }

            if (response == C) {
                break;
            }

            ESP_LOGE(TAG, "Receiver did not request data packets");
            return -2;
        }

        if (response == CAN) {
            ESP_LOGE(TAG, "Transmission canceled by receiver");
            return 1;
        }

        attempts++;
    } while (attempts < 10);

    if (attempts == 10) {
        ESP_LOGE(TAG, "Failed to send header packet");
        return 2;
    }

    const esp_partition_t *partition = get_unused_ota_partition();
    if (partition == NULL) {
        ESP_LOGE(TAG, "No unused OTA partition found");
        return ESP_ERR_NOT_FOUND;
    }

    for (int offset = 0; offset < file_size; offset += 1024) {
        int packet_size = (file_size - offset > 1024) ? 1024 : (file_size - offset);

        packet[0] = STX;
        packet[1] = sequence & 0xFF;
        packet[2] = ~sequence & 0xFF;
        esp_partition_read(partition, offset, &packet[3], packet_size);

        if (packet_size < 1024) {
            if (packet_size < 128) {
                packet[0] = SOH;
                memset(&packet[3 + packet_size], 0x1A, 128 - packet_size);
            } else {
                memset(&packet[3 + packet_size], 0x1A, 1024 - packet_size);
            }
        }

        if (packet_size < 128) {
            crc = crc16(&packet[3], 128);
            packet[131] = (crc >> 8) & 0xFF;
            packet[132] = crc & 0xFF;
        } else {
            crc = crc16(&packet[3], 1024);
            packet[1027] = (crc >> 8) & 0xFF;
            packet[1028] = crc & 0xFF;
        }

        attempts = 0;
        do {
            int response = 0;

            ymodem_send_packet(packet, packet_size < 128 ? 133 : 1029);
            response = ymodem_receive_control_response("wait-data-ack", sequence);
            if (response == ACK) {
                sequence++;
                break;
            }
            if (response == NAK) {
                ESP_LOGW(TAG, "YMODEM phase 'wait-data-ack': receiver requested retransmission for packet %d", sequence);
            } else if (response == CAN) {
                ESP_LOGE(TAG, "Transmission canceled by receiver");
                return 1;
            }
            attempts++;
        } while (attempts < 10);

        if (attempts == 10) {
            ESP_LOGE(TAG, "Failed to send data packet %d", sequence);
            return 3;
        }
    }

    uint8_t eot = EOT;
    attempts = 0;
    do {
        int response = 0;

        ymodem_uart_send_data(&eot, 1);
        response = ymodem_receive_control_response("wait-eot-ack", 0);
        if (response == ACK) {
            response = ymodem_receive_control_response("wait-final-C", 0);
            if (response == C) {
                break;
            }
        }
        attempts++;
    } while (attempts < 10);

    if (attempts == 10) {
        ESP_LOGE(TAG, "Failed to complete YMODEM transmission");
        return 4;
    }

    packet[0] = SOH;
    packet[1] = 0x00;
    packet[2] = 0xFF;
    memset(&packet[3], 0, 1024);

    crc = crc16(&packet[3], 128);
    packet[131] = (crc >> 8) & 0xFF;
    packet[132] = crc & 0xFF;

    ymodem_send_packet(packet, 133);
    (void)ymodem_receive_control_response("wait-empty-header-ack", 0);

    ESP_LOGI(TAG, "YMODEM transfer finished successfully");
    return 0;
}

