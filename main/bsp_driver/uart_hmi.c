#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <string.h>
#include <freertos/semphr.h>
#include "board_config.h"
#include "system_runtime.h"
#include "uart_hmi.h"
#include "sp_pro_app.h"
#include "esp_log.h"

#define HMI_UART_PACKET_TIMEOUT_TICKS 2
#define HMI_UART_RX_BUF_SIZE          KLM_UART_BUF_SIZE
#define HMI_UART_DISPLAY_FRAME_SIZE   BF7613_DISPLAY_FRAME_LEN
#define HMI_UART_REFRESH_MS           100
#define HMI_UART_TASK_STACK_SIZE_DEFAULT 4096
#define HMI_UART_TASK_STACK_SIZE_TEST    3072
#define HMI_UART_TASK_STACK_SIZE         HMI_UART_TASK_STACK_SIZE_TEST
#define HMI_UART_QUEUE_LENGTH_DEFAULT    KLM_UART_QUEUE_LENGTH
#define HMI_UART_QUEUE_LENGTH_TEST       2
#define HMI_UART_QUEUE_LENGTH            HMI_UART_QUEUE_LENGTH_TEST
#define HMI_UART_QUEUE_ITEM_SIZE_DEFAULT KLM_UART_BUF_SIZE
#define HMI_UART_QUEUE_ITEM_SIZE_TEST    128
#define HMI_UART_QUEUE_ITEM_SIZE         HMI_UART_QUEUE_ITEM_SIZE_TEST

static const char *TAG = "uart_hmi";

typedef struct {
    int uart_num;
    bool initialized;
    long last_data_tick;
    char rx_buf[HMI_UART_RX_BUF_SIZE];
    int rx_buf_idx;
} uart_info_t;

static uart_info_t s_hmi_uart = {
    .uart_num = HMI_UART_NUM,
    .initialized = false,
    .last_data_tick = 0,
    .rx_buf = {0},
    .rx_buf_idx = 0,
};

static SemaphoreHandle_t s_hmi_tx_mutex;

QueueHandle_t uart_rx_queue[1];

static void uart_receive_task(void *task_arg);
static void uart_tx_task(void *task_arg);
static bool hmi_uart_write_bytes(const uint8_t *data, size_t len);
static void hmi_discard_rx_data(void);
static void hmi_handle_key_event(const bf7613_key_event_t *event);
static void hmi_queue_rx_packet(size_t packet_len);
static bool hmi_should_queue_rx_packet(void);

static bool hmi_uart_write_bytes(const uint8_t *data, size_t len)
{
    if (!data || len == 0U || !s_hmi_uart.initialized) {
        return false;
    }

    if (sys_pra.app_mode == APP_MODE_YMODEM) {
        return false;
    }

    if (xSemaphoreTake(s_hmi_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "HMI TX mutex timeout");
        return false;
    }

    uart_write_bytes(HMI_UART_NUM, data, len);
    uart_wait_tx_done(HMI_UART_NUM, pdMS_TO_TICKS(100));
    xSemaphoreGive(s_hmi_tx_mutex);
    return true;
}

static void hmi_handle_key_event(const bf7613_key_event_t *event)
{
    if (event) {
        // ESP_LOGI(TAG,
        //          "[HMI][KEY] frame=%u mask=0x%04X",
        //          event->frame_id,
        //          event->key_mask);
    }

    sp_pro_handle_key_event(event);
}

void ctr_power_io_enable(void)
{
    /* Enable the controller power switch output. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_CTR_POWER_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
    gpio_set_level(PIN_CTR_POWER_EN, 1);
}

void hmi_uart_init(int baud_rate, int rx_pin, int tx_pin)
{
    if (s_hmi_uart.initialized) {
        ESP_LOGW(TAG, "HMI UART already initialized");
        return;
    }

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    uart_param_config(HMI_UART_NUM, &uart_config);
    uart_set_pin(HMI_UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(HMI_UART_NUM, HMI_UART_RX_BUF_SIZE * 2, 0, 0, NULL, 0);

    uart_rx_queue[0] = xQueueCreate(HMI_UART_QUEUE_LENGTH, HMI_UART_QUEUE_ITEM_SIZE);
    s_hmi_tx_mutex = xSemaphoreCreateMutex();
    if (!uart_rx_queue[0]) {
        ESP_LOGE(TAG,
                 "Failed to create HMI RX queue, len=%u item=%u",
                 (unsigned)HMI_UART_QUEUE_LENGTH,
                 (unsigned)HMI_UART_QUEUE_ITEM_SIZE);
        return;
    }
    ESP_LOGI(TAG,
             "HMI RX queue created, len=%u item=%u",
             (unsigned)HMI_UART_QUEUE_LENGTH,
             (unsigned)HMI_UART_QUEUE_ITEM_SIZE);

    if (xTaskCreate(uart_receive_task, "uart_receive_task", HMI_UART_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create UART receive task, stack=%u bytes",
                 (unsigned)HMI_UART_TASK_STACK_SIZE);
        return;
    }
    ESP_LOGI(TAG,
             "UART receive task created, stack=%u bytes",
             (unsigned)HMI_UART_TASK_STACK_SIZE);

    if (xTaskCreate(uart_tx_task, "uart_tx_task", HMI_UART_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create UART TX task, stack=%u bytes",
                 (unsigned)HMI_UART_TASK_STACK_SIZE);
        return;
    }
    ESP_LOGI(TAG,
             "UART TX task created, stack=%u bytes",
             (unsigned)HMI_UART_TASK_STACK_SIZE);

    s_hmi_uart.initialized = true;
}

void uart_send_data(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }

    hmi_uart_write_bytes((const uint8_t *)data, (size_t)len);
}

void uart_send_str(const char *data)
{
    if (!data) {
        return;
    }

    ESP_LOGI(TAG, "uart[%d] send: %s", HMI_UART_NUM, data);
    uart_send_data(data, (int)strlen(data));
}

void uart_send_data_direct(const char *data, int len)
{
    uart_send_data(data, len);
}

void uart_send_str_direct(const char *data)
{
    uart_send_str(data);
}

static void uart_tx_task(void *task_arg)
{
    uint8_t frame_buf[HMI_UART_DISPLAY_FRAME_SIZE];
    disp_model_t *disp_model;
    TickType_t last_refresh_tick = 0;
    UBaseType_t watermark;
    bool workload_logged = false;

    (void)task_arg;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "uart_tx_task start, stack=%u bytes, high_water=%u words",
             (unsigned)HMI_UART_TASK_STACK_SIZE,
             (unsigned)watermark);

    while (1) {
        if (s_hmi_uart.initialized && sys_pra.app_mode != APP_MODE_YMODEM) {
            disp_model = sp_pro_get_disp_model();
            if (disp_model) {
                bool need_refresh = disp_model->dirty;
                TickType_t now_tick = xTaskGetTickCount();

                if (!need_refresh &&
                    (now_tick - last_refresh_tick) >= pdMS_TO_TICKS(HMI_UART_REFRESH_MS)) {
                    disp_model->disp.frame_id = (uint8_t)(disp_model->disp.frame_id + 1U);
                    need_refresh = true;
                }

                if (need_refresh &&
                    bf7613_build_display_frame(&disp_model->disp,
                                               frame_buf,
                                               sizeof(frame_buf))) {
                    // ESP_LOGI(TAG,
                    //          "[HMI][UI] frame=%u q1=%u q2=%u voice=%u/%u",
                    //          disp_model->disp.frame_id,
                    //          disp_model->disp.q1_gauge,
                    //          disp_model->disp.q2_gauge,
                    //          disp_model->disp.voice_seq,
                    //          disp_model->disp.voice_data);
                    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame_buf, sizeof(frame_buf), ESP_LOG_INFO);
                    hmi_uart_write_bytes(frame_buf, sizeof(frame_buf));
                    disp_model->dirty = false;
                    last_refresh_tick = now_tick;
                    if (!workload_logged) {
                        workload_logged = true;
                        watermark = uxTaskGetStackHighWaterMark(NULL);
                        ESP_LOGI(TAG,
                                 "uart_tx_task first refresh complete, high_water=%u words",
                                 (unsigned)watermark);
                    }
                } else if (need_refresh) {
                    ESP_LOGE(TAG,
                             "Failed to build HMI display frame, buf_len=%u proto_frame_len=%u",
                             (unsigned)sizeof(frame_buf),
                             (unsigned)BF7613_DISPLAY_FRAME_LEN);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void hmi_discard_rx_data(void)
{
    int len;
    char temp_arr[100];

    while (1) {
        len = uart_read_bytes(HMI_UART_NUM, temp_arr, sizeof(temp_arr) - 1, 0);
        if (len <= 0) {
            break;
        }

        temp_arr[len] = '\0';
        ESP_LOGW(TAG, "Discarded HMI data: %s", temp_arr);
    }
}

static void hmi_queue_rx_packet(size_t packet_len)
{
    size_t copy_len = packet_len;
    static TickType_t s_last_drop_log_tick = 0;

    if (!uart_rx_queue[0]) {
        ESP_LOGW(TAG, "Skip HMI RX queue send because queue is null");
        return;
    }

    if (!hmi_should_queue_rx_packet()) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_drop_log_tick) >= pdMS_TO_TICKS(2000)) {
            s_last_drop_log_tick = now;
            // ESP_LOGI(TAG,
            //          "Drop HMI RX queue packet because app_mode=%d does not consume queued HMI text",
            //          sys_pra.app_mode);
        }
        return;
    }

    if (copy_len >= HMI_UART_QUEUE_ITEM_SIZE) {
        copy_len = HMI_UART_QUEUE_ITEM_SIZE - 1U;
        s_hmi_uart.rx_buf[copy_len] = '\0';
        ESP_LOGW(TAG,
                 "HMI RX packet truncated for queue: original=%u queued=%u",
                 (unsigned)packet_len,
                 (unsigned)copy_len);
    }

    if (xQueueSend(uart_rx_queue[0], s_hmi_uart.rx_buf, 10) != pdPASS) {
        ESP_LOGW(TAG,
                 "HMI RX queue send failed, len=%u item=%u",
                 (unsigned)packet_len,
                 (unsigned)HMI_UART_QUEUE_ITEM_SIZE);
    }
}

static bool hmi_should_queue_rx_packet(void)
{
    return sys_pra.app_mode == APP_MODE_RUNTIME_BRIDGE;
}

static void uart_receive_task(void *task_arg)
{
    static long time_tick = 0;
    int len;
    bf7613_key_event_t key_event;
    UBaseType_t watermark;
    bool workload_logged = false;

    (void)task_arg;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "uart_receive_task start, stack=%u bytes, high_water=%u words",
             (unsigned)HMI_UART_TASK_STACK_SIZE,
             (unsigned)watermark);

    while (1) {
        if (s_hmi_uart.initialized && sys_pra.app_mode != APP_MODE_YMODEM) {
            if (s_hmi_uart.rx_buf_idx + 1 < HMI_UART_RX_BUF_SIZE) {
                len = uart_read_bytes(HMI_UART_NUM,
                                      &s_hmi_uart.rx_buf[s_hmi_uart.rx_buf_idx],
                                      HMI_UART_RX_BUF_SIZE - s_hmi_uart.rx_buf_idx - 1,
                                      0);
                if (len > 0 && s_hmi_uart.rx_buf_idx + len >= 8) {
                    if (bf7613_parse_key_frame(
                            (uint8_t *)&s_hmi_uart.rx_buf[s_hmi_uart.rx_buf_idx + len - 8],
                            8,
                            &key_event)) {
                        if (!workload_logged) {
                            workload_logged = true;
                            watermark = uxTaskGetStackHighWaterMark(NULL);
                            ESP_LOGI(TAG,
                                     "uart_receive_task first key frame parsed, high_water=%u words",
                                     (unsigned)watermark);
                        }
                        hmi_handle_key_event(&key_event);
                    }
                }
            } else {
                hmi_discard_rx_data();
                len = 0;
            }

            if (s_hmi_uart.rx_buf_idx + len >= HMI_UART_RX_BUF_SIZE - 1) {
                s_hmi_uart.rx_buf[HMI_UART_RX_BUF_SIZE - 1] = '\0';
                ESP_LOGW(TAG, "HMI RX buffer full");
                watermark = uxTaskGetStackHighWaterMark(NULL);
                ESP_LOGW(TAG,
                         "uart_receive_task buffer_full, high_water=%u words",
                         (unsigned)watermark);
                hmi_discard_rx_data();
                hmi_queue_rx_packet(HMI_UART_RX_BUF_SIZE - 1U);
                s_hmi_uart.last_data_tick = 0;
                s_hmi_uart.rx_buf_idx = 0;
            } else if (time_tick > s_hmi_uart.last_data_tick + HMI_UART_PACKET_TIMEOUT_TICKS &&
                       len == 0 &&
                       s_hmi_uart.rx_buf_idx != 0) {
                s_hmi_uart.rx_buf[s_hmi_uart.rx_buf_idx] = '\0';
                hmi_queue_rx_packet((size_t)s_hmi_uart.rx_buf_idx);
                s_hmi_uart.last_data_tick = 0;
                s_hmi_uart.rx_buf_idx = 0;
            } else if (len > 0) {
                if (s_hmi_uart.last_data_tick == 0 || s_hmi_uart.rx_buf_idx != 0) {
                    s_hmi_uart.last_data_tick = time_tick;
                }
                s_hmi_uart.rx_buf_idx += len;
            }
        }

        time_tick++;
        vTaskDelay(1);
    }
}
