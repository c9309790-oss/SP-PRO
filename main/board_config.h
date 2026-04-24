#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/uart.h"

#define TAOREN_BRAND

/* BSP pin mapping. */
#ifdef TAOREN_BRAND /* TaoRen hardware variant */
/* BF7613 screen UART pins. */
#define PIN_SCREEN_TTL_TX (6)
#define PIN_SCREEN_TTL_RX (7)

/* Controller RS485 pins. */
#define PIN_CTR_POWER_EN  (40)
#define PIN_CTR_RS485_DIR (41)
#define PIN_CTR_RS485_TX  (16)
#define PIN_CTR_RS485_RX  (15)

#define UART_HMI_BAUDRATE 9600
#else /* Reference hardware variant */
/* BF7613 screen UART pins. */
#define PIN_SCREEN_TTL_TX (5)
#define PIN_SCREEN_TTL_RX (6)
/* Controller RS485 pins. */
#define PIN_CTR_POWER_EN  (7)
#define PIN_CTR_RS485_DIR (46)
#define PIN_CTR_RS485_TX  (17)
#define PIN_CTR_RS485_RX  (18)

#define UART_HMI_BAUDRATE 115200
#endif

/* LED strip pins. */
#define PIN_LED_STRIP_PWR   (2)
#define PIN_LED_STRIP_DAT   (1)
#define LED_STRIP_NUM       (34)
/* LED strip pins. */
#define PIN_POWER_INDICATOR (10)
#define PIN_POWER_KEY       (12)
#define PIN_POWER_OFF       (11)

#define CTR_UART_NUM  UART_NUM_1   /* UART1 is connected to the controller. */
#define HMI_UART_NUM  UART_NUM_2   /* UART2 is connected to the HMI screen. */

#define KLM_MQTT_TEST
//#define KLM_FLASH_LOG_ENABLE
#define KLM_MSG_HEAD_LEN_MAX 10

#define MAX_OF_FOUR(a, b, c, d) ((a > b ? a : b) > (c > d ? c : d) ? (a > b ? a : b) : (c > d ? c : d))

#define KLM_WELCOME_NAME    "Here is SP1Pro\r\n"
#define KLM_UART_BUF_SIZE (1500)
#define KLM_UART_QUEUE_LENGTH (3)

#define KLM_MQTT_RX_BUF_SIZE (1500)
#define KLM_MQTT_QUEUE_LENGTH (3)

#define KLM_BLE_RX_BUF_SIZE (500+6)
#define KLM_BLE_QUEUE_LENGTH (2)

#define KLM_TCP_RX_BUF_SIZE (1024)
#define KLM_TCP_QUEUE_LENGTH (3)

#define HF270_VER "99.11.00 (2025-10-16 08:00 4M)"
#define MODULE_VER  "ESP32-S3-V011"

#endif /* BOARD_CONFIG_H */
