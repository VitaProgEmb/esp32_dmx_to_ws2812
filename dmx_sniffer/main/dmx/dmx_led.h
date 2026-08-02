/**
 * @file dmx_led.h
 * @brief DMX → LED мост (чистый API, без signal/event_bus)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ===== Callback API ===== */

typedef struct {
    void (*on_mode)(uint8_t mode);              /* 0=sniffer 1=tester 2=patch */
    void (*on_led_test)(uint8_t mode, uint8_t r, uint8_t g, uint8_t b,
                        uint8_t speed, uint16_t pixel, uint16_t count);
    void (*on_led_clear)(void);
    void (*on_led_count)(uint8_t strip, uint16_t count);
    void (*on_led_reverse)(bool on);
    void (*on_led_mode)(bool sequential);
    void (*on_led_shift)(uint8_t shift);
    void (*on_led_interpolate)(bool on);
    void (*on_dmx_test)(uint8_t port, uint8_t channel,
                        uint8_t r, uint8_t g, uint8_t b,
                        bool fill, uint16_t count);
    void (*on_fixture_settings)(uint8_t channel_order,
                                uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b,
                                uint16_t fallback_timeout_ms);
} dmx_led_cbs_t;

/* ===== LED Settings ===== */

typedef enum {
    CH_ORDER_RGB = 0,
    CH_ORDER_RBG,
    CH_ORDER_GRB,
    CH_ORDER_GBR,
    CH_ORDER_BRG,
    CH_ORDER_BGR,
    CH_ORDER_COUNT
} channel_order_t;

typedef struct {
    volatile bool    reverse;
    volatile uint16_t shift;
    volatile bool    interpolate;
    volatile channel_order_t channel_order;
    volatile uint8_t  fallback_r, fallback_g, fallback_b;
    volatile uint16_t fallback_timeout_ms;
} dmx_led_settings_t;

extern dmx_led_settings_t g_led_settings;

/* ===== API ===== */

void dmx_led_init(const dmx_led_cbs_t *cbs);
void dmx_led_recompute(void);
void dmx_led_apply_fallback(void);
const uint8_t (*dmx_led_get_fixture_colors(void))[3];
