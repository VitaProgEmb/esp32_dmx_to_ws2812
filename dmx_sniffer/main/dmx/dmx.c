/**
 * @file dmx.c
 * @brief DMX512 UART HAL — приём, передача, переключение режимов
 *
 * Чистый модуль: UART RX ISR + TX FIFO. Никакой логики с LED, fallback, effects.
 */

#include "dmx/dmx.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_cpu.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "soc/uart_struct.h"
#include "soc/uart_reg.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "DMX";

/* ===== UART константы ===== */

#define UART_BAUD_RATE       250000
#define BREAK_DEBOUNCE_CYCLES 160000
#define RX_ISR_FLAGS (UART_BRK_DET_INT_ENA | UART_FRM_ERR_INT_ENA | \
                      UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_OVF_INT_ENA)

static IRAM_ATTR uart_dev_t *uart_hw[] = { &UART1, &UART2 };

/* ===== RX context ===== */

typedef struct {
    uint8_t           rx_active[DMX_FRAME_LEN];
    uint8_t           rx_done[DMX_FRAME_LEN];
    volatile uint16_t frame_len;
    volatile uint16_t frame_len_saved;
    volatile bool     enabled;
    volatile bool     sync;
    bool              in_frame;
    uint32_t          last_break_cyc;
    intr_handle_t     intr_handle;
    TaskHandle_t      notify_task;
    int               uart_num;
    int               rx_pin;
    int               tx_pin;
    int               dir_pin;
    int               fifo_thr;
    portMUX_TYPE      mux;
} dmx_rx_ctx_t;

static dmx_rx_ctx_t s_ctx[2];

/* ===== RX: ISR ===== */

static void IRAM_ATTR uart_rx_isr(void *arg) {
    int port = (int)arg;
    dmx_rx_ctx_t *ctx = &s_ctx[port];
    uart_dev_t *hw = uart_hw[port];

    if (!ctx->enabled) return;
    g_isr_count[port]++;

    uint32_t int_st;
    while ((int_st = hw->int_st.val) != 0) {

        if (int_st & UART_RXFIFO_FULL_INT_ST) {
            hw->int_clr.val = UART_RXFIFO_FULL_INT_CLR;
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                if (ctx->in_frame && ctx->frame_len < DMX_FRAME_LEN)
                    ctx->rx_active[ctx->frame_len++] = hw->fifo.rw_byte;
                else
                    (void)hw->fifo.rw_byte;
            }
        }

        if (int_st & UART_BRK_DET_INT_ST) {
            hw->int_clr.val = UART_BRK_DET_INT_CLR;
            uint32_t now = esp_cpu_get_cycle_count();
            if (ctx->last_break_cyc != 0 &&
                (now - ctx->last_break_cyc) < BREAK_DEBOUNCE_CYCLES) {
                while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0)
                    (void)hw->fifo.rw_byte;
                continue;
            }
            ctx->last_break_cyc = now;
            g_break_count[port]++;

            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0 &&
                   ctx->frame_len < DMX_FRAME_LEN)
                ctx->rx_active[ctx->frame_len++] = hw->fifo.rw_byte;
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0)
                (void)hw->fifo.rw_byte;

            if (ctx->in_frame && ctx->frame_len > 0) {
                if (!ctx->sync) {
                    ctx->sync = true;
                } else {
                    ctx->frame_len_saved = ctx->frame_len;
                    memcpy(ctx->rx_done, ctx->rx_active, ctx->frame_len);
                    if (ctx->notify_task) {
                        BaseType_t wake = pdFALSE;
                        vTaskNotifyGiveFromISR(ctx->notify_task, &wake);
                    }
                }
            }
            ctx->frame_len = 0;
            ctx->in_frame = true;
        }

        if (int_st & UART_FRM_ERR_INT_ST)
            hw->int_clr.val = UART_FRM_ERR_INT_CLR;

        if (int_st & UART_RXFIFO_OVF_INT_ST) {
            hw->int_clr.val = UART_RXFIFO_OVF_INT_CLR;
            ctx->in_frame = false;
            ctx->frame_len = 0;
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0)
                (void)hw->fifo.rw_byte;
        }
    }
}

/* ===== RX: задача ===== */

static dmx_frame_cb_t s_frame_callback = NULL;

static void dmx_rx_task(void *arg) {
    int port = (int)arg;
    dmx_rx_ctx_t *ctx = &s_ctx[port];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!ctx->enabled) continue;

        uint16_t len = ctx->frame_len_saved;
        if (len < 2) continue;

        portENTER_CRITICAL(&ctx->mux);
        uint8_t local[DMX_FRAME_LEN];
        memcpy(local, ctx->rx_done, len);
        portEXIT_CRITICAL(&ctx->mux);

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        memcpy(g_raw_frames[port].data, local + 1, len - 1);
        g_raw_frames[port].len = len - 1;
        g_raw_frames[port].timestamp_ms = now;

        if (len >= 100) {
            uint16_t ck_xor = 0, ck_sum = 0;
            for (int i = 1; i < len; i++) { ck_xor ^= local[i]; ck_sum += local[i]; }
            dmx_ck_ring_t *ring = &g_ck_rings[port];
            uint32_t idx = ring->count & (CK_RING_SIZE - 1);
            ring->buf[idx].xor_val = ck_xor;
            ring->buf[idx].sum_val = ck_sum;
            ring->buf[idx].frame_len = len;
            ring->count++;
        }

        xEventGroupSetBits(g_dmx_events, port == 0 ? DMX_EVT_FRAME0 : DMX_EVT_FRAME1);

        if (s_frame_callback) s_frame_callback(port);
    }
}

/* ===== RX: enable/disable ===== */

static void dmx_rx_enable(int port, bool enable) {
    dmx_rx_ctx_t *ctx = &s_ctx[port];
    uart_dev_t *hw = uart_hw[port];
    ctx->enabled = enable;
    if (enable) {
        uart_ll_rxfifo_rst(hw);
        ctx->in_frame = false;
        ctx->frame_len = 0;
        ctx->last_break_cyc = 0;
        ctx->sync = false;
        uart_ll_ena_intr_mask(hw, RX_ISR_FLAGS);
    } else {
        uart_ll_disable_intr_mask(hw, RX_ISR_FLAGS);
        uart_ll_rxfifo_rst(hw);
    }
}

/* ===== RX: init ===== */

static void dmx_rx_init_port(int port, int rx_pin, int tx_pin, int dir_pin,
                              int uart_num, int fifo_thr) {
    memset(&s_ctx[port], 0, sizeof(dmx_rx_ctx_t));
    s_ctx[port].rx_pin   = rx_pin;
    s_ctx[port].tx_pin   = tx_pin;
    s_ctx[port].dir_pin  = dir_pin;
    s_ctx[port].uart_num = uart_num;
    s_ctx[port].fifo_thr = fifo_thr;
    s_ctx[port].enabled  = true;
    portMUX_INITIALIZE(&s_ctx[port].mux);
}

static void dmx_rx_start(void) {
    g_dmx_events = xEventGroupCreate();

    uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_2,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    for (int port = 0; port < 2; port++) {
        dmx_rx_ctx_t *ctx = &s_ctx[port];
        uart_dev_t *hw = uart_hw[port];

        ESP_ERROR_CHECK(uart_param_config(ctx->uart_num, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(ctx->uart_num, ctx->tx_pin, ctx->rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        uart_ll_set_rxfifo_full_thr(hw, ctx->fifo_thr);
        uart_ll_rxfifo_rst(hw);

        ESP_ERROR_CHECK(esp_intr_alloc(
            port == 0 ? ETS_UART1_INTR_SOURCE : ETS_UART2_INTR_SOURCE,
            ESP_INTR_FLAG_IRAM, uart_rx_isr, (void *)port, &ctx->intr_handle));
        uart_ll_ena_intr_mask(hw, RX_ISR_FLAGS);

        if (ctx->dir_pin >= 0) {
            gpio_config_t dir_conf = {
                .pin_bit_mask = (1ULL << ctx->dir_pin),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&dir_conf);
            gpio_set_level(ctx->dir_pin, 0);
        }

        xTaskCreatePinnedToCore(dmx_rx_task, port ? "dmx_rx1" : "dmx_rx0",
                                4096, (void *)(intptr_t)port, 4,
                                &ctx->notify_task, port);

        ESP_LOGI(TAG, "port%d: RX=GPIO%d TX=GPIO%d DIR=%s uart%d",
                 port, ctx->rx_pin, ctx->tx_pin,
                 ctx->dir_pin >= 0 ? "ON" : "OFF", ctx->uart_num);
    }
}

/* ===== TX: HAL ===== */

static bool dmx_hal_send(int port, const uint8_t *data, int len) {
    uart_dev_t *hw = (port == 0) ? (&UART1) : (&UART2);

    hw->conf0.txd_inv = 1;
    ets_delay_us(176);
    hw->conf0.txd_inv = 0;
    ets_delay_us(16);

    int sent = 0;
    while (sent < len) {
        uint32_t fifo_free = 128 - uart_ll_get_txfifo_len(hw);
        if (fifo_free > 0) {
            int chunk = (len - sent) < (int)fifo_free ? (len - sent) : (int)fifo_free;
            uart_ll_write_txfifo(hw, data + sent, chunk);
            sent += chunk;
        }
        if (sent < len) ets_delay_us(10);
    }

    while (!(hw->int_st.val & UART_TX_DONE_INT_ST)) {}
    hw->int_clr.val = UART_TX_DONE_INT_CLR;
    return true;
}

/* ===== TX: задача ===== */

static void dmx_tx_task(void *arg) {
    uint8_t frame[DMX_CHANNELS + 1];
    dmx_mode_t prev_mode = DMX_MODE_SNIFFER;

    while (1) {
        dmx_lock();
        dmx_mode_t mode = g_dmx.mode;
        dmx_unlock();

        if (mode != prev_mode) {
            if (mode == DMX_MODE_TESTER || mode == DMX_MODE_PATCH)
                vTaskDelay(pdMS_TO_TICKS(100));
            prev_mode = mode;
        }

        if (mode == DMX_MODE_TESTER) {
            dmx_lock();
            int txp = g_dmx.tx_port;
            uint16_t txch = g_dmx.tx_channel;
            uint8_t tr = g_dmx.tx_r, tg = g_dmx.tx_g, tb = g_dmx.tx_b;
            tx_mode_t txm = g_dmx.tx_mode;
            uint16_t txc = g_dmx.tx_count;
            dmx_unlock();

            if (txp < 0 || txp > DMX_PORT_COUNT) txp = 0;
            memset(frame, 0, sizeof(frame));
            frame[0] = 0;

            if (txm == TX_MODE_POINT) {
                if (txch >= 1 && txch + 2 <= DMX_CHANNELS) {
                    frame[txch] = tr; frame[txch + 1] = tg; frame[txch + 2] = tb;
                }
            } else {
                for (uint16_t i = 0; i < txc && i * 3 + 3 <= DMX_CHANNELS; i++) {
                    uint16_t addr = i * 3 + 1;
                    frame[addr] = tr; frame[addr + 1] = tg; frame[addr + 2] = tb;
                }
            }

            int64_t t_start = esp_timer_get_time();
            if (txp >= DMX_PORT_COUNT) {
                dmx_hal_send(0, frame, DMX_CHANNELS + 1);
                dmx_hal_send(1, frame, DMX_CHANNELS + 1);
            } else {
                dmx_hal_send(txp, frame, DMX_CHANNELS + 1);
            }
            int64_t remain = 33333 - (esp_timer_get_time() - t_start);
            if (remain > 2000) vTaskDelay(pdMS_TO_TICKS((remain / 1000) - 1));
            while (esp_timer_get_time() - t_start < 33333) {}

        } else if (mode == DMX_MODE_PATCH) {
            dmx_lock();
            int uni = g_dmx.patch_universe;
            int cursor = g_dmx.patch_cursor;
            dmx_unlock();

            int port = (uni == 2) ? DMX_PORT_2 : DMX_PORT_1;
            memset(frame, 0, sizeof(frame));
            frame[0] = 0;

            /* PATCH-режим: данные приходят через dmx_write() из dmx_led.c */
            int64_t t_start = esp_timer_get_time();
            dmx_hal_send(port, frame, DMX_CHANNELS + 1);
            int64_t remain = 33333 - (esp_timer_get_time() - t_start);
            if (remain > 2000) vTaskDelay(pdMS_TO_TICKS((remain / 1000) - 1));
            while (esp_timer_get_time() - t_start < 33333) {}

        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ===== Глобалы ===== */

dmx_state_t g_dmx = {
    .mode                = DMX_MODE_SNIFFER,
    .tx_port             = 0,
    .tx_channel          = 1,
    .tx_r                = 255, .tx_g = 255, .tx_b = 255,
    .tx_mode             = TX_MODE_POINT,
    .tx_count            = 3,
    .channel_order       = CH_ORDER_RGB,
    .fallback_r          = 0, .fallback_g = 0, .fallback_b = 255,
    .fallback_timeout_ms = 100,
    .last_rx_ms          = { 0, 0 },
    .patch_cursor        = 0,
    .patch_universe      = 1,
};

EventGroupHandle_t g_dmx_events;
dmx_raw_frame_t    g_raw_frames[2];
dmx_ck_ring_t      g_ck_rings[2];
volatile uint32_t  g_isr_count[2]   = {0, 0};
volatile uint32_t  g_break_count[2] = {0, 0};

static SemaphoreHandle_t g_dmx_mutex = NULL;

/* ===== Lock / Unlock ===== */

void dmx_lock(void) {
    if (g_dmx_mutex) xSemaphoreTake(g_dmx_mutex, portMAX_DELAY);
}

void dmx_unlock(void) {
    if (g_dmx_mutex) xSemaphoreGive(g_dmx_mutex);
}

/* ===== Публичный API ===== */

void dmx_read(int port, uint8_t *buf, int len) {
    if (port < 0 || port >= DMX_PORT_COUNT) return;
    int n = len < DMX_CHANNELS ? len : DMX_CHANNELS;
    dmx_lock();
    memcpy(buf, g_raw_frames[port].data, n);
    dmx_unlock();
}

void dmx_write(int port, const uint8_t *frame, int len) {
    if (port < 0 || port >= DMX_PORT_COUNT) return;
    dmx_hal_send(port, frame, len);
}

void dmx_on_frame(dmx_frame_cb_t cb) {
    s_frame_callback = cb;
}

void dmx_set_mode(dmx_mode_t m) {
    bool tx_mode = (m == DMX_MODE_TESTER || m == DMX_MODE_PATCH);

    dmx_lock();
    g_dmx.mode = m;
    dmx_unlock();

    if (tx_mode) {
        dmx_rx_enable(0, false);
        dmx_rx_enable(1, false);
    }

    int dir = tx_mode ? 1 : 0;
    gpio_set_level(DMX_GPIO_DIR2, dir);

    if (!tx_mode) {
        dmx_rx_enable(0, true);
        dmx_rx_enable(1, true);
    }

    ESP_LOGI(TAG, "Mode: %s, DIR=%d",
             m == DMX_MODE_SNIFFER ? "SNIFFER" :
             m == DMX_MODE_TESTER  ? "TESTER"  : "PATCH", dir);
}

void dmx_init(int tx1, int tx2, int rx1, int rx2, int dir) {
    g_dmx_mutex = xSemaphoreCreateMutex();

    dmx_rx_init_port(0, rx1, tx1, -1,  UART_NUM_1, 8);
    dmx_rx_init_port(1, rx2, tx2, dir, UART_NUM_2, 16);
    dmx_rx_start();

    xTaskCreatePinnedToCore(dmx_tx_task, "dmx_tx", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Init: TX1=GPIO%d TX2=GPIO%d RX1=GPIO%d RX2=GPIO%d DIR=GPIO%d",
             tx1, tx2, rx1, rx2, dir);
}
