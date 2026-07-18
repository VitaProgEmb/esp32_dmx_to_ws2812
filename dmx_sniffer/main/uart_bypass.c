#include "uart_bypass.h"
#include "settings.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "dmx_hal.h"
#include "dmx.h"
#include <string.h>

static const char *TAG = "UART_SW";

#define UART_BAUD_RATE  250000
#define BIT_PERIOD_US   4       /* 1 / 250000 = 4 мкс на бит */
#define BREAK_MIN_US    88      /* BREAK ≥ 88 мкс по DMX512 */
#define FRAME_GAP_US    100     /* Пауза между кадрами > 100 мкс */

/* ======================================================================
 * СОСТОЯНИЕ СОФТОВОГО UART ДЛЯ КАЖДОГО ПОРТА
 * ====================================================================== */

typedef struct {
    /* Edge timing */
    int64_t last_edge_us;
    int64_t last_falling_us;

    /* UART decoder state */
    uint8_t rx_byte;
    int bitcount;

    /* Frame buffer */
    uint8_t rx_active[BYPASS_DMX_SIZE];
    uint32_t rx_head;

    /* Double buffer for completed frames */
    uint8_t rx_done[BYPASS_DMX_SIZE];
    volatile uint32_t last_frame_len;
    volatile bool frame_ready;

    /* Flags */
    bool in_frame;
    bool seen_break;

    /* Diagnostics */
    volatile uint32_t frame_count;
    volatile uint32_t isr_count;
    volatile uint32_t break_count;
    volatile uint32_t err_count;

    /* Task notification */
    TaskHandle_t notify_task;

    /* GPIO pin */
    int gpio_rx;

    /* TX UART (hardware) */
    uart_port_t uart_tx;

    portMUX_TYPE mux;
} sw_uart_ctx_t;

static sw_uart_ctx_t s_ctx[2];

/* ======================================================================
 * GPIO ISR — ДЕКОДИРОВАНИЕ UART ПО EDGE TIMING
 * ======================================================================
 *
 * Алгоритм:
 *   1. На каждом edge (FALLING/RISING) запоминаем timestamp
 *   2. Вычисляем delta = now - last_edge_us
 *   3. bits = delta / 4 мкс (количество бит на предыдущем уровне)
 *   4. level_before = !current_level (уровень ДО этого edge)
 *   5. Сдвигаем bits копий level_before в rx_byte
 *
 * Пример: 0x55 (10101010 LSB)
 *   FALLING T0:     стартовый бит (LOW 4мкс)
 *   RISING  T0+4:   delta=4, bits=1, prev=LOW → старт ✓
 *   FALLING T0+8:   delta=4, bits=1, prev=HIGH → bit0=1
 *   RISING  T0+12:  delta=4, bits=1, prev=LOW  → bit1=0
 *   ...
 *
 * BREAK обнаружение: пауза между FALLING edges > 100 мкс
 * ====================================================================== */

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    int port = (int)arg;
    sw_uart_ctx_t *ctx = &s_ctx[port];

    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(ctx->gpio_rx);
    int64_t delta = now - ctx->last_edge_us;
    ctx->last_edge_us = now;
    ctx->isr_count++;

    /* === FALLING EDGE (уровень стал LOW) === */
    if (level == 0) {
        int64_t falling_gap = now - ctx->last_falling_us;
        ctx->last_falling_us = now;

        /* BREAK/WIDTH检测: пауза между FALLING edges > 100 мкс
         * означает что был BREAK + MAB → новый кадр */
        if (falling_gap > FRAME_GAP_US) {
            /* Завершить предыдущий кадр если есть */
            if (ctx->in_frame && ctx->rx_head > 0) {
                memcpy(ctx->rx_done, ctx->rx_active, ctx->rx_head);
                ctx->last_frame_len = ctx->rx_head;
                ctx->frame_count++;
                ctx->frame_ready = true;

                if (ctx->notify_task) {
                    BaseType_t wake = pdFALSE;
                    vTaskNotifyGiveFromISR(ctx->notify_task, &wake);
                }
            }

            /* Начать новый кадр */
            ctx->rx_head = 0;
            ctx->in_frame = true;
            ctx->rx_byte = 0;
            ctx->bitcount = 0;
            ctx->seen_break = true;
            ctx->break_count++;
            return;  /* Стартовый бит — биты считаем на следующем edge */
        }

        /* Обычный FALLING edge внутри кадра — обработать биты */
        goto process_bits;
    }

    /* === RISING EDGE (уровень стал HIGH) === */
    /* Обработать биты которые были на LOW уровне до этого edge */

process_bits:
    if (!ctx->in_frame || ctx->bitcount >= 10) return;
    if (delta == 0) return;

    int prev_level = !level;
    int bits = (int)((delta + BIT_PERIOD_US / 2) / BIT_PERIOD_US);
    if (bits <= 0) bits = 1;

    for (int i = 0; i < bits && ctx->bitcount < 10; i++) {
        if (ctx->bitcount == 0) {
            /* Стартовый бит: должен быть LOW */
            if (prev_level != 0) {
                ctx->err_count++;
                ctx->in_frame = false;
                return;
            }
        } else if (ctx->bitcount <= 8) {
            /* Данные биты (LSB first) */
            if (prev_level) {
                ctx->rx_byte |= (1 << (ctx->bitcount - 1));
            }
        }
        /* Стоп-биты (9-10): просто считаем, проверим позже */
        ctx->bitcount++;
    }

    /* Байт готов */
    if (ctx->bitcount >= 10) {
        /* Проверяем стоп-биты (биты 9-10 должны быть HIGH) */
        if (prev_level == 0 && bits >= 1) {
            /* Последний обработанный бит был LOW — стоп-бит невалиден */
            ctx->err_count++;
        }

        if (ctx->rx_head < DMX_CHANNELS) {
            ctx->rx_active[ctx->rx_head++] = ctx->rx_byte;
        }
        ctx->rx_byte = 0;
        ctx->bitcount = 0;
    }
}

/* ======================================================================
 * ИНИЦИАЛИЗАЦИЯ
 * ====================================================================== */

void uart_bypass_init(int port) {
    sw_uart_ctx_t *ctx = &s_ctx[port];

    ctx->last_edge_us = 0;
    ctx->last_falling_us = 0;
    ctx->rx_byte = 0;
    ctx->bitcount = 0;
    ctx->rx_head = 0;
    ctx->last_frame_len = 0;
    ctx->frame_ready = false;
    ctx->in_frame = false;
    ctx->seen_break = false;
    ctx->frame_count = 0;
    ctx->isr_count = 0;
    ctx->break_count = 0;
    ctx->err_count = 0;
    ctx->notify_task = NULL;

    if (port == 0) {
        ctx->gpio_rx = DMX_GPIO_RX1;
        ctx->uart_tx = UART_NUM_1;
    } else {
        ctx->gpio_rx = DMX_GPIO_RX2;
        ctx->uart_tx = UART_NUM_2;
    }

    portMUX_INITIALIZE(&ctx->mux);
}

void uart_bypass_init_all(void) {
    uart_bypass_init(0);
    uart_bypass_init(1);

    /* Настраиваем GPIO RX как входы с прерыванием по обоим фронтам */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << DMX_GPIO_RX1) | (1ULL << DMX_GPIO_RX2),
    };
    gpio_config(&io_conf);

    /* Устанавливаем ISR для GPIO */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(DMX_GPIO_RX1, gpio_isr_handler, (void *)0);
    gpio_isr_handler_add(DMX_GPIO_RX2, gpio_isr_handler, (void *)1);

    /* GPIO DIR для второго порта (RX direction) */
    gpio_config_t dir_conf = {
        .pin_bit_mask = (1ULL << DMX_GPIO_DIR2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dir_conf);
    gpio_set_level(DMX_GPIO_DIR2, 0);

    ESP_LOGI(TAG, "Software UART RX on GPIO%d (port0) and GPIO%d (port1)",
             DMX_GPIO_RX1, DMX_GPIO_RX2);
}

void uart_bypass_start_timer(void) {
    /* UART драйвер нужен ТОЛЬКО для TX (тестер/патч).
     * RX работает через GPIO прерывания — аппаратный UART не используется. */
    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* UART1 TX (порт 0) */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, DMX_GPIO_TX1, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    QueueHandle_t q0;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 0, 1024, 0, &q0, 0));

    /* UART2 TX (порт 1) */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, DMX_GPIO_TX2, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    QueueHandle_t q1;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 0, 1024, 0, &q1, 0));

    ESP_LOGI(TAG, "TX UARTs initialized (UART1=GPIO%d, UART2=GPIO%d)",
             DMX_GPIO_TX1, DMX_GPIO_TX2);
}

void uart_bypass_set_notify_task(int port, TaskHandle_t handle) {
    s_ctx[port].notify_task = handle;
}

void uart_bypass_set_dir(int port, int level) {
    gpio_set_level(DMX_GPIO_DIR2, level);
    ESP_LOGI(TAG, "DIR2(GPIO%d)=%d", DMX_GPIO_DIR2, level);
}

void uart_bypass_set_tx_mode(int port, bool tx_mode) {
    sw_uart_ctx_t *ctx = &s_ctx[port];
    if (tx_mode) {
        /* Отключаем GPIO прерывания для этого порта */
        gpio_intr_disable(ctx->gpio_rx);
    } else {
        /* Сбрасываем state machine */
        ctx->rx_head = 0;
        ctx->in_frame = false;
        ctx->seen_break = false;
        ctx->frame_ready = false;
        ctx->bitcount = 0;
        ctx->rx_byte = 0;
        /* Включаем GPIO прерывания обратно */
        gpio_intr_enable(ctx->gpio_rx);
    }
}

bool uart_bypass_get_frame(int port, uint8_t *out, int max_len, uint32_t *frame_len) {
    sw_uart_ctx_t *ctx = &s_ctx[port];
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
    return s_ctx[port].err_count;
}
