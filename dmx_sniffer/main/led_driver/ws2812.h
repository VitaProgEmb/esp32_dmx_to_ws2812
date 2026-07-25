#pragma once

/**
 * @file ws2812.h
 * @brief Драйвер WS2812B через RMT — двойная буферизация
 */

#include <stdint.h>
#include <stdbool.h>
#include "driver/rmt_tx.h"
#include "settings.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ws2812_color_t;

typedef struct {
    ws2812_color_t  bank_a[LED_STRIP_MAX_LEDS];
    ws2812_color_t  bank_b[LED_STRIP_MAX_LEDS];
    ws2812_color_t *active;
    uint16_t        count;
    rmt_channel_handle_t rmt_chan;
    rmt_encoder_handle_t encoder;
    uint8_t         gpio;
} ws2812_strip_t;

extern ws2812_strip_t g_strip1;
extern ws2812_strip_t g_strip2;

void     ws2812_init(ws2812_strip_t *strip, uint8_t gpio, uint16_t num_leds);
void     ws2812_set_pixel(ws2812_strip_t *strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
void     ws2812_set_all(ws2812_strip_t *strip, uint8_t r, uint8_t g, uint8_t b);
void     ws2812_clear(ws2812_strip_t *strip);
void     ws2812_swap(ws2812_strip_t *strip);
void     ws2812_refresh(ws2812_strip_t *strip);
