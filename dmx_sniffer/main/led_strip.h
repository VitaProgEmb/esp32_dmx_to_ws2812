#pragma once

#include <stdint.h>
#include "settings.h"
#include "driver/rmt_tx.h"

#define LED_COLORS_PER_LED    3

#define LED_T0H_NS   400
#define LED_T0L_NS   850
#define LED_T1H_NS   800
#define LED_T1L_NS   450
#define LED_RESET_US 280

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

typedef struct {
    led_color_t *colors;                     ///< Points to active bank
    led_color_t  bank_a[LED_STRIP_MAX_LEDS]; ///< Double buffer A
    led_color_t  bank_b[LED_STRIP_MAX_LEDS]; ///< Double buffer B
    uint16_t     count;
    rmt_channel_handle_t rmt_chan;
} led_strip_t;

extern led_strip_t g_led_strip;

void     led_strip_init(uint16_t num_leds);
void     led_strip_deinit(void);
void     led_strip_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
void     led_strip_set_all(uint8_t r, uint8_t g, uint8_t b);
void     led_strip_set_pixels(led_color_t *colors, uint16_t count);
void     led_strip_refresh(void);
void     led_strip_clear(void);
void     led_strip_lock(void);
void     led_strip_unlock(void);

void     led_strip_set_pixel_front(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
void     led_strip_swap(void);

void     led_strip_swap_banks(void);

extern volatile int g_led_test_px;
extern volatile int g_led_test_count;
