/**
 * @file dmx_rx.c
 * @brief DMX512 приёмник — HW UART ISR + double-buffer + event notification
 *
 * Поток данных:
 *   ISR ──► rx_active[] ──swap──► rx_done[]
 *                                      │ notify
 *                                      ▼
 *                                dmx_rx_task
 *                                (копирует под spinlock)
 *                                      │
 *                            g_raw_frames[port]
 *                            xEventGroupSetBits()
 */

#include "dmx_rx.h"
#include "dmx_bus.h"
#include "settings.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "hal/uart_ll.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "DMX_RX";

#define UART_BAUD_RATE       250000
#define FIFO_FULL_THR        64
#define BREAK_DEBOUNCE_CYCLES 160000

#define RX_ISR_FLAGS (UART_BRK_DET_INT_ENA | UART_FRM_ERR_INT_ENA | \
                      UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_OVF_INT_ENA)

static IRAM_ATTR uart_dev_t *uart_hw[] = { &UART1, &UART2 };

typedef struct {
    uint8_t          rx_active[DMX_FRAME_LEN];
    uint8_t          rx_done[DMX_FRAME_LEN];
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
    portMUX_TYPE      mux;
} dmx_rx_ctx_t;

static dmx_rx_ctx_t s_ctx[2];

EventGroupHandle_t g_dmx_events;
dmx_raw_frame_t    g_raw_frames[2];

volatile uint32_t g_isr_count[2] = {0, 0};
volatile uint32_t g_break_count[2] = {0, 0};

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

            portENTER_CRITICAL_ISR(&ctx->mux);
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                if (ctx->in_frame && ctx->frame_len < DMX_FRAME_LEN) {
                    ctx->rx_active[ctx->frame_len++] = hw->fifo.rw_byte;
                } else {
                    (void)hw->fifo.rw_byte;
                }
            }
            portEXIT_CRITICAL_ISR(&ctx->mux);
        }

        if (int_st & UART_BRK_DET_INT_ST) {
            hw->int_clr.val = UART_BRK_DET_INT_CLR;

            uint32_t now = esp_cpu_get_cycle_count();
            if (ctx->last_break_cyc != 0 &&
                (now - ctx->last_break_cyc) < BREAK_DEBOUNCE_CYCLES) {
                portENTER_CRITICAL_ISR(&ctx->mux);
                while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                    (void)hw->fifo.rw_byte;
                }
                portEXIT_CRITICAL_ISR(&ctx->mux);
                continue;
            }
            ctx->last_break_cyc = now;
            g_break_count[port]++;

            portENTER_CRITICAL_ISR(&ctx->mux);
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0 &&
                   ctx->frame_len < DMX_FRAME_LEN) {
                ctx->rx_active[ctx->frame_len++] = hw->fifo.rw_byte;
            }
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                (void)hw->fifo.rw_byte;
            }
            portEXIT_CRITICAL_ISR(&ctx->mux);

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

        if (int_st & UART_FRM_ERR_INT_ST) {
            hw->int_clr.val = UART_FRM_ERR_INT_CLR;
        }

        if (int_st & UART_RXFIFO_OVF_INT_ST) {
            hw->int_clr.val = UART_RXFIFO_OVF_INT_CLR;
            ctx->in_frame = false;
            ctx->frame_len = 0;
            portENTER_CRITICAL_ISR(&ctx->mux);
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                (void)hw->fifo.rw_byte;
            }
            portEXIT_CRITICAL_ISR(&ctx->mux);
        }
    }
}

static void dmx_rx_task(void *arg) {
    int port = (int)arg;
    dmx_rx_ctx_t *ctx = &s_ctx[port];

    ESP_LOGD(TAG, "port%d task on core %d", port, esp_cpu_get_core_id());

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

        xEventGroupSetBits(g_dmx_events, port == 0 ? DMX_EVT_FRAME0 : DMX_EVT_FRAME1);
    }
}

bool dmx_rx_is_alive(int port) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (now - g_raw_frames[port].timestamp_ms) < 100;
}

void dmx_rx_init(int port, const dmx_rx_cfg_t *cfg) {
    memset(&s_ctx[port], 0, sizeof(dmx_rx_ctx_t));
    s_ctx[port].rx_pin   = cfg->rx_pin;
    s_ctx[port].tx_pin   = cfg->tx_pin;
    s_ctx[port].dir_pin  = cfg->dir_pin;
    s_ctx[port].uart_num = cfg->uart_num;
    s_ctx[port].enabled  = true;
    portMUX_INITIALIZE(&s_ctx[port].mux);
}

void dmx_rx_start(void) {
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

        uart_ll_set_rxfifo_full_thr(hw, FIFO_FULL_THR);
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

void dmx_rx_enable(int port, bool enable) {
    dmx_rx_ctx_t *ctx = &s_ctx[port];
    uart_dev_t *hw = uart_hw[port];

    ctx->enabled = enable;

    if (enable) {
        portENTER_CRITICAL_ISR(&ctx->mux);
        uart_ll_rxfifo_rst(hw);
        portEXIT_CRITICAL_ISR(&ctx->mux);
        ctx->in_frame = false;
        ctx->frame_len = 0;
        ctx->last_break_cyc = 0;
        ctx->sync = false;
        uart_ll_ena_intr_mask(hw, RX_ISR_FLAGS);
    } else {
        uart_ll_disable_intr_mask(hw, RX_ISR_FLAGS);
        portENTER_CRITICAL_ISR(&ctx->mux);
        uart_ll_rxfifo_rst(hw);
        portEXIT_CRITICAL_ISR(&ctx->mux);
    }

    ESP_LOGI(TAG, "port%d %s", port, enable ? "ENABLED" : "DISABLED");
}
