#ifndef LED_STRIP_H
#define LED_STRIP_H
#include <stdbool.h>
#include <stdint.h>

void led_hw_init(void);
void led_hw_power(bool on);

void led_hw_clear(void);
void led_hw_set_all(uint8_t r, uint8_t g, uint8_t b);
void led_hw_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

#endif /* LED_H */