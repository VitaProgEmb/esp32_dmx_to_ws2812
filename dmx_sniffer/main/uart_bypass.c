#include "uart_bypass.h"
#include "settings.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "dmx_hal.h"
#include "dmx.h"
#include <string.h>

static const char *TAG = "UART_BYPASS";

#define UART_BAUD_RATE  250000
#define UART_RX_BUF     4096
#define UART_TX_BUF     1024

/* ======================================================================
 * АДРЕСА РЕГИСТРОВ UART (ESP32)
 * ======================================================================
 * Используются для безопасного сброса FIFO через portENTER_CRITICAL.
 *
 * [CPU-3.21]: прямое чтение FIFO-регистра во время прерывания на другом
 * UART вызывает зависание APB-шины. Решение — блокировать прерывания
 * на ТЕКУЩЕМ ядре (portENTER_CRITICAL) при обращении к FIFO.
 * ====================================================================== */

#define UART1_BASE_ADDR 0x3FF50000
#define UART2_BASE_ADDR 0x3FF6E000

#define UART_STATUS_REG_OFF  0x1C
#define UART_FIFO_REG_OFF    0x00
#define UART_RXFIFO_CNT_MASK 0x1F

#define UART_GET_BASE(uart_num) \
    ((uart_num) == UART_NUM_1 ? UART1_BASE_ADDR : UART2_BASE_ADDR)

#define UART_READ_REG(base, off) (*(volatile uint32_t *)((base) + (off)))

/* ======================================================================
 * КОНТЕКСТ UART
 * ====================================================================== */

typedef struct {
    uart_port_t uart_num;
    int gpio_rx;
    int gpio_tx;

    uint8_t rx_active[BYPASS_DMX_SIZE];
    volatile uint32_t rx_head;
    bool in_frame;

    uint8_t rx_done[BYPASS_DMX_SIZE];
    volatile bool frame_ready;

    bool seen_break;

    volatile uint32_t frame_count;
    volatile uint32_t last_frame_len;

    volatile uint32_t isr_count;
    volatile uint32_t break_count;
    volatile uint32_t ovf_count;

    int64_t last_break_us;

    TaskHandle_t notify_task;
    TaskHandle_t event_task;

    QueueHandle_t uart_queue;

    uint32_t base_addr;             /* Базовый адрес регистров UART */
    portMUX_TYPE fifo_mux;          /* Spinlock для безопасного сброса FIFO */
} hw_uart_ctx_t;

static hw_uart_ctx_t s_ctx[2];

/* ======================================================================
 * БЕЗОПАСНЫЙ СБРОС FIFO
 * ======================================================================
 *
 * Замена uart_flush_input() которая сама читает FIFO без критической
 * секции (см. uart_ll_rxfifo_rst в uart_ll.h:370).
 *
 * Наша версия:
 *   1. portENTER_CRITICAL — блокирует прерывания на текущем ядре
 *   2. Читает байты из FIFO-регистра (пока fifo_cnt > 0)
 *   3. portEXIT_CRITICAL — восстанавливает прерывания
 *
 * Время выполнения: ~1-5 мкс (128 байт × ~40 нс за чтение)
 * ====================================================================== */

static void safe_fifo_drain(hw_uart_ctx_t *ctx) {
    uint32_t base = ctx->base_addr;

    portENTER_CRITICAL(&ctx->fifo_mux);
    while (1) {
        uint32_t status = UART_READ_REG(base, UART_STATUS_REG_OFF);
        uint32_t fifo_cnt = status & UART_RXFIFO_CNT_MASK;
        if (fifo_cnt == 0) break;
        (void)UART_READ_REG(base, UART_FIFO_REG_OFF);
    }
    portEXIT_CRITICAL(&ctx->fifo_mux);
}

/* ======================================================================
 * EVENT TASK (ESP-IDF UART driver)
 * ======================================================================
 *
 * Используем стандартный event-driven подход ESP-IDF, но:
 *   - uart_flush_input() заменён на safe_fifo_drain()
 *   - Вместо uart_flush_input() + xQueueReset используем
 *     safe_fifo_drain() + xQueueReset()
 * ====================================================================== */

static void uart_event_task(void *arg) {
    int port = (int)arg;
    hw_uart_ctx_t *ctx = &s_ctx[port];
    uart_event_t event;
    uint8_t tmp[256];

    while (1) {
        if (xQueueReceive(ctx->uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA: {
                int len = uart_read_bytes(ctx->uart_num, tmp, event.size, pdMS_TO_TICKS(10));
                ctx->isr_count++;
                for (int i = 0; i < len; i++) {
                    uint8_t b = tmp[i];
                    if (!ctx->in_frame) {
                        if (b == 0 && ctx->seen_break) {
                            ctx->in_frame = true;
                            ctx->rx_head = 0;
                            ctx->seen_break = false;
                        }
                        continue;
                    }
                    if (ctx->rx_head < DMX_CHANNELS) {
                        ctx->rx_active[ctx->rx_head] = b;
                        ctx->rx_head++;
                    }
                }
                break;
            }
            case UART_BREAK: {
                int64_t now = esp_timer_get_time();
                if (now - ctx->last_break_us < 1000) {
                    safe_fifo_drain(ctx);
                    xQueueReset(ctx->uart_queue);
                    break;
                }
                ctx->last_break_us = now;
                ctx->break_count++;
                if (ctx->in_frame && ctx->rx_head > 0) {
                    dmx_store_frame(port, ctx->rx_active, ctx->rx_head);
                    if (ctx->notify_task)
                        xTaskNotifyGive(ctx->notify_task);
                }
                ctx->rx_head = 0;
                ctx->in_frame = false;
                ctx->seen_break = true;
                safe_fifo_drain(ctx);
                xQueueReset(ctx->uart_queue);
                break;
            }
            case UART_FRAME_ERR:
                ctx->ovf_count++;
                break;
            case UART_FIFO_OVF:
                ctx->ovf_count++;
                safe_fifo_drain(ctx);
                xQueueReset(ctx->uart_queue);
                ctx->rx_head = 0;
                ctx->in_frame = false;
                break;
            default:
                break;
            }
        }
    }
}

/* ======================================================================
 * НАСТРОЙКА UART
 * ====================================================================== */

static void configure_uart(int port) {
    hw_uart_ctx_t *ctx = &s_ctx[port];

    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(ctx->uart_num, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(ctx->uart_num, ctx->gpio_tx, ctx->gpio_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(ctx->uart_num, UART_RX_BUF, UART_TX_BUF, 64,
                                         &ctx->uart_queue, 0));

    /* pull-up/pull-down настраиваем ПОСЛЕ установки драйвера,
     * иначе uart_driver_install может перезаписать конфигурацию GPIO */
    gpio_set_pull_mode(ctx->gpio_rx, GPIO_FLOATING);
}

/* ======================================================================
 * ПУБЛИЧНЫЕ ФУНКЦИИ
 * ====================================================================== */

void uart_bypass_init(int port) {
    hw_uart_ctx_t *ctx = &s_ctx[port];

    if (port == 0) {
        ctx->uart_num = UART_NUM_1;
        ctx->gpio_rx = DMX_GPIO_RX1;
        ctx->gpio_tx = DMX_GPIO_TX1;
        ctx->base_addr = UART1_BASE_ADDR;
    } else {
        ctx->uart_num = UART_NUM_2;
        ctx->gpio_rx = DMX_GPIO_RX2;
        ctx->gpio_tx = DMX_GPIO_TX2;
        ctx->base_addr = UART2_BASE_ADDR;
    }

    ctx->rx_head = 0;
    ctx->in_frame = false;
    ctx->frame_ready = false;
    ctx->frame_count = 0;
    ctx->last_frame_len = 0;
    ctx->isr_count = 0;
    ctx->break_count = 0;
    ctx->ovf_count = 0;
    ctx->last_break_us = 0;
    ctx->seen_break = false;
    ctx->notify_task = NULL;
    ctx->event_task = NULL;
    ctx->uart_queue = NULL;

    portMUX_INITIALIZE(&ctx->fifo_mux);
}

void uart_bypass_init_all(void) {
    uart_bypass_init(0);
    uart_bypass_init(1);

    gpio_config_t dir_conf = {
        .pin_bit_mask = (1ULL << DMX_GPIO_DIR2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dir_conf);
    gpio_set_level(DMX_GPIO_DIR2, 0);
}

void uart_bypass_start_timer(void) {
    configure_uart(0);
    configure_uart(1);

    TaskHandle_t evt0 = NULL, evt1 = NULL;
    xTaskCreatePinnedToCore(uart_event_task, "uart0_evt", 8192, (void*)0, 5, &evt0, 0);
    xTaskCreatePinnedToCore(uart_event_task, "uart1_evt", 8192, (void*)1, 5, &evt1, 1);
    s_ctx[0].event_task = evt0;
    s_ctx[1].event_task = evt1;

    ESP_LOGI(TAG, "UART event tasks started on different cores (0,1)");
}

void uart_bypass_set_notify_task(int port, TaskHandle_t handle) {
    s_ctx[port].notify_task = handle;
}

void uart_bypass_set_dir(int port, int level) {
    gpio_set_level(DMX_GPIO_DIR2, level);
    ESP_LOGI(TAG, "DIR2(GPIO%d)=%d", DMX_GPIO_DIR2, gpio_get_level(DMX_GPIO_DIR2));
}

void uart_bypass_set_tx_mode(int port, bool tx_mode) {
    hw_uart_ctx_t *ctx = &s_ctx[port];
    if (tx_mode) {
        if (ctx->event_task) vTaskSuspend(ctx->event_task);
        safe_fifo_drain(ctx);
        xQueueReset(ctx->uart_queue);
    } else {
        safe_fifo_drain(ctx);
        xQueueReset(ctx->uart_queue);
        ctx->rx_head = 0;
        ctx->in_frame = false;
        ctx->seen_break = false;
        if (ctx->event_task) vTaskResume(ctx->event_task);
    }
}

bool uart_bypass_get_frame(int port, uint8_t *out, int max_len, uint32_t *frame_len) {
    hw_uart_ctx_t *ctx = &s_ctx[port];
    if (!ctx->frame_ready) return false;

    int len = (max_len < (int)ctx->last_frame_len) ? max_len : (int)ctx->last_frame_len;
    memcpy(out, ctx->rx_done, len);
    *frame_len = ctx->last_frame_len;
    ctx->frame_ready = false;
    return true;
}

uint32_t uart_bypass_get_isr_count(int port) {
    return s_ctx[port].isr_count;
}

uint32_t uart_bypass_get_break_count(int port) {
    return s_ctx[port].break_count;
}

uint32_t uart_bypass_get_ovf_count(int port) {
    return s_ctx[port].ovf_count;
}
