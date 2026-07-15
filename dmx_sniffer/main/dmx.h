#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "dmx_hal.h"

#define DMX_MAX_CHANNELS   512
#define DMX_FIXTURE_CH     3

typedef enum {
    DMX_MODE_SNIFFER = 0,
    DMX_MODE_TESTER  = 1,
    DMX_MODE_PATCH   = 2
} dmx_mode_t;

typedef enum {
    TX_MODE_POINT = 0,
    TX_MODE_FILL  = 1
} tx_mode_t;

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
    volatile dmx_mode_t  mode;
    volatile uint8_t     tx_port;
    volatile uint16_t    tx_channel;
    volatile uint8_t     tx_r, tx_g, tx_b;
    volatile tx_mode_t   tx_mode;
    volatile uint16_t    tx_count;

    volatile bool        led_reverse;
    volatile uint16_t    led_shift;
    volatile bool        interpolate;

    volatile channel_order_t  channel_order;

    volatile uint8_t     fallback_r, fallback_g, fallback_b;
    volatile uint16_t    fallback_timeout_ms;

    volatile uint32_t    last_rx_ms[DMX_PORT_COUNT];

    volatile int         patch_cursor;
    volatile int         patch_universe;
} dmx_state_t;

extern dmx_state_t g_dmx;

void dmx_init(void);
void dmx_set_mode(dmx_mode_t m);
void dmx_start_rx_task(void);
void dmx_start_tx_task(void);
void dmx_recompute_lookups(void);
void dmx_lock(void);
void dmx_unlock(void);
void dmx_get_channel_data(int port, uint8_t *out, int max_count);

extern volatile uint32_t s_rx_frame_count[];
extern volatile uint32_t s_rx_last_frame_size[];
void dmx_store_frame(int port, const uint8_t *slots, int num_slots);
void dmx_process_frame(int port, const uint8_t *slots, int num_slots);
void dmx_process_leds(int port);
const uint8_t (*dmx_get_fixture_colors(void))[3];
void dmx_apply_color_order(uint8_t ch_order, uint8_t *r, uint8_t *g, uint8_t *b);
void dmx_apply_fallback(void);
