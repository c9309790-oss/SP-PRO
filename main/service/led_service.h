#ifndef __LED_SERVICE_H__
#define __LED_SERVICE_H__

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LED_EFFECT_OFF = 0,
    LED_EFFECT_WHITE_BREATH,
    LED_EFFECT_WHITE_HIGHLIGHT,
    LED_EFFECT_WHITE_HALF,
    LED_EFFECT_BLUE_BREATH,
    LED_EFFECT_RED_BREATH,
    LED_EFFECT_ORANGE_BREATH,
    LED_EFFECT_WHITE_SOLID,
    LED_EFFECT_RED_SOLID,
} led_sys_state_t;

void led_service_init(void);
void led_service_set_state(led_sys_state_t state);

/* 每 20ms 调用一次灯效更新 */
void led_service_tick(void);

#endif /* __LED_SERVICE_H__ */
