#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ===== Константы ===== */

#define DMX_CHANNELS     512
#define DMX_FRAME_LEN    513
#define DMX_PORT_1       0
#define DMX_PORT_2       1
#define DMX_PORT_COUNT   2
#define DMX_EVT_FRAME0   BIT0
#define DMX_EVT_FRAME1   BIT1

/* ===== Типы ===== */

typedef enum {
    DMX_MODE_SNIFFER = 0,
    DMX_MODE_TESTER  = 1,
    DMX_MODE_PATCH   = 2
} dmx_mode_t;

typedef enum {
    TX_MODE_POINT = 0,
    TX_MODE_FILL  = 1
} tx_mode_t;

typedef struct {
    uint8_t  data[512];
    uint16_t len;
    uint32_t timestamp_ms;
} dmx_raw_frame_t;

#define CK_RING_SIZE 1024

typedef struct {
    uint16_t xor_val;
    uint16_t sum_val;
    uint16_t frame_len;
} dmx_ck_entry_t;

typedef struct {
    dmx_ck_entry_t buf[CK_RING_SIZE];
    volatile uint32_t count;
} dmx_ck_ring_t;

typedef struct {
    volatile dmx_mode_t  mode;
    volatile uint8_t     tx_port;
    volatile uint16_t    tx_channel;
    volatile uint8_t     tx_r, tx_g, tx_b;
    volatile tx_mode_t   tx_mode;
    volatile uint16_t    tx_count;
    volatile uint32_t    last_rx_ms[DMX_PORT_COUNT];
    volatile int         patch_cursor;
    volatile int         patch_universe;
} dmx_state_t;

/* ===== Дефолты ===== */

#define DMX_DEFAULT_MODE  DMX_MODE_SNIFFER

/* ===== Глобалы ===== */

extern dmx_state_t       g_dmx;
extern EventGroupHandle_t g_dmx_events;
extern dmx_raw_frame_t    g_raw_frames[2];
extern dmx_ck_ring_t      g_ck_rings[2];
extern volatile uint32_t  g_isr_count[2];
extern volatile uint32_t  g_break_count[2];

/* ===== API ===== */

typedef void (*dmx_frame_cb_t)(int port);

void dmx_init(int tx1, int tx2, int rx1, int rx2, int dir);
void dmx_read(int port, uint8_t *buf, int len);
void dmx_write(int port, const uint8_t *frame, int len);
void dmx_set_mode(dmx_mode_t m);
void dmx_on_frame(dmx_frame_cb_t cb);
void dmx_lock(void);
void dmx_unlock(void);
