#include "led_service.h"
#include "led.h"
#include "esp_log.h"

#define TAG "led_service"

typedef struct {
    uint8_t r, g, b;
    bool    breathing;
} led_effect_t;

static led_sys_state_t s_state = LED_EFFECT_OFF;
static led_effect_t s_effect;

/* 呼吸灯运行状态 */
static uint8_t s_breath_val = 0;
static int8_t  s_breath_dir = 1;

/* ========= 系统状态到灯效的映射表 ========= */
static led_effect_t effect_table[] = {
    [LED_EFFECT_OFF]             = {  0,   0,   0, false},
    [LED_EFFECT_WHITE_BREATH]    = { 80,  80,  80, true },
    [LED_EFFECT_WHITE_HIGHLIGHT] = {100, 100, 100, false},
    [LED_EFFECT_WHITE_HALF]      = { 40,  40,  40, false},
    [LED_EFFECT_BLUE_BREATH]     = {  0,  40, 100, true },
    [LED_EFFECT_RED_BREATH]      = {100,   0,   0, true },
    [LED_EFFECT_ORANGE_BREATH]   = {100,  40,   0, true },
    [LED_EFFECT_WHITE_SOLID]     = { 70,  70,  70, false},
    [LED_EFFECT_RED_SOLID]       = {100,   0,   0, false},
};

void led_service_set_state(led_sys_state_t state)
{
    if (state == s_state) return;

    s_state  = state;
    s_effect = effect_table[state];

    s_breath_val = 0;
    s_breath_dir = 1;

    ESP_LOGE(TAG, "LED state -> %d", state);
}

void led_service_tick(void)
{
    if (!s_effect.breathing) {
        led_hw_set_all(s_effect.r, s_effect.g, s_effect.b);
        return;
    }

    /* 更新呼吸亮度 */
    s_breath_val += s_breath_dir * 2;
    if (s_breath_val >= 60) {
        s_breath_val = 60;
        s_breath_dir = -1;
    } else if (s_breath_val == 0) {
        s_breath_dir = 1;
    }

    uint8_t r = (s_effect.r * s_breath_val) / 60;
    uint8_t g = (s_effect.g * s_breath_val) / 60;
    uint8_t b = (s_effect.b * s_breath_val) / 60;

    led_hw_set_all(r, g, b);
}

void led_service_init(void)
{
    led_hw_init();
    led_hw_power(true);
    led_hw_clear();

    led_service_set_state(LED_EFFECT_OFF);
    ESP_LOGI(TAG, "led service init");
}


