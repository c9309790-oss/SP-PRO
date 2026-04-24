#include "led.h"
#include "led_strip.h"

#include "esp_log.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"

#define TAG "led_drv"

/* ========= 静态资源 ========= */
static led_strip_handle_t s_strip = NULL;
static rmt_channel_handle_t s_rmt_chan = NULL;
static bool s_inited = false;

/* ========= 内部函数 ========= */

static void led_gpio_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_STRIP_PWR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void led_rmt_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = PIN_LED_STRIP_DAT,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_rmt_chan));
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));
}

static void led_strip_init_internal(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED_STRIP_DAT,
        .max_leds = LED_STRIP_NUM,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
}

/* ========= 对外接口 ========= */

void led_hw_init(void)
{
    if (s_inited) {
        ESP_LOGW(TAG, "led already inited");
        return;
    }

    led_gpio_init();
    led_rmt_init();
    led_strip_init_internal();

    s_inited = true;
    ESP_LOGI(TAG, "led init done");
}

void led_hw_power(bool on)
{
    gpio_set_level(PIN_LED_STRIP_PWR, on ? 1 : 0);
}

void led_hw_clear(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
}

void led_hw_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;

    for (int i = 0; i < LED_STRIP_NUM; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}

void led_hw_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    if (index >= LED_STRIP_NUM) return;

    led_strip_set_pixel(s_strip, index, r, g, b);
    led_strip_refresh(s_strip);
}
