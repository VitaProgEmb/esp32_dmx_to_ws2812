#include "uart_bypass.h"
#include "settings.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "dmx_hal.h"
#include "dmx.h"
#include <string.h>

static const char *TAG = "UART_SW";

#define UART_BAUD_RATE  250000
#define BIT_PERIOD_US   4
#define BREAK_MIN_US    88
#define FRAME_GAP_US    100

/* ======================================================================
 * ОБЩАЯ СТРУКТУРА СОСТОЯНИЯ ДЛЯ КАЖДОГО DMX ПОРТА
 * ====================================================================== */

typedef struct {
    /* Frame buffer — double buffering */
    uint8_t rx_active[BYPASS_DMX_SIZE];
    uint8_t rx_done[BYPASS_DMX_SIZE];
    volatile uint32_t rx_head;
    volatile uint32_t last_frame_len;
    volatile bool frame_ready;

    /* Diagnostics */
    volatile uint32_t frame_count;
    volatile uint32_t break_count;
    volatile uint32_t err_count;
    volatile uint32_t isr_count;

    /* Task notification */
    TaskHandle_t notify_task;

    /* Hardware mapping */
    int gpio_rx;
    uart_port_t uart_tx;

#if DMX_SW_UART_MODE == 1

    /* RMT RX specific */
    rmt_channel_handle_t rmt_rx_chan;
    QueueHandle_t rmt_rx_queue;
    TaskHandle_t rmt_rx_task;
    volatile bool in_frame;
    uint32_t timeout_thresh;
    /* Decoder state (меж-batch) */
    uint8_t nibble_phase;        /* 0=low nibble, 1=high nibble */
    uint8_t current_byte;        /* собранный байт */
    int byte_count;
    /* Carry buffer — остаток items между batch'ами */
    uint8_t carry_pulses[8];
    bool    carry_levels[8];
    uint8_t carry_count;
#endif

#if DMX_SW_UART_MODE == 0
    /* GPIO edge timing specific */
    int64_t last_edge_us;
    int64_t last_falling_us;
    uint8_t rx_byte;
    int bitcount;
    bool in_frame;
    bool seen_break;
    portMUX_TYPE mux;
#endif
} sw_uart_ctx_t;

static sw_uart_ctx_t s_ctx[2];

/* ======================================================================
 *  RMT RX РЕЖИМ (DMX_SW_UART_MODE == 1)
 * ====================================================================== */

#if DMX_SW_UART_MODE == 1

/* Буфер RMT RX: 64 rmt_symbol_word_t = 128 rmt_item16_t */
#define RMT_RX_BUF_SYMBOLS  64
/* Максимальное количество rmt_item16_t в одном блоке */
#define RMT_RX_BUF_ITEMS   (RMT_RX_BUF_SYMBOLS * 2)

/* Буфер для копирования RMT символов из ISR */
typedef struct {
    rmt_symbol_word_t symbols[RMT_RX_BUF_SYMBOLS];
    size_t num_symbols;
    uint32_t is_last;
} rmt_rx_msg_t;

/*
 * Callback RMT RX — вызывается из ISR когда DMA заполнил буфер.
 * Копируем symbols в очередь для обработки в task.
 * ВАЖНО: received_symbols указывает на наш буфер — копируем до re-arm.
 */
static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t channel,
                                      const rmt_rx_done_event_data_t *edata,
                                      void *user_data)
{
    int port = (int)user_data;
    sw_uart_ctx_t *ctx = &s_ctx[port];

    rmt_rx_msg_t msg;
    msg.num_symbols = edata->num_symbols;
    msg.is_last = edata->flags.is_last;
    memcpy(msg.symbols, edata->received_symbols,
           edata->num_symbols * sizeof(rmt_symbol_word_t));

    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(ctx->rmt_rx_queue, &msg, &wake);
    ctx->isr_count++;
    return wake == pdTRUE;
}

/* ======================================================================
 * NIBBLE LUT: RMT импульсы → 4-битный ниббл (0x0..0xF)
 * ======================================================================
 *
 * Каждый ниббл DMX = стартовый бит (LOW 4т) + 4 данных бита (LSB first).
 * RMT items: level + duration в тиках (1MHz клок, 1 тик = 1мкс).
 *
 * Вход: массив {pulse_ticks, level} из 1-4 items
 * Выход: 4 бита (0x0..0xF) или -1 если не матчит
 * consumed: сколько items съедено
 *
 * Паттерны (старт=LOW всегда):
 *   0x0: [12L]           0x8: [4L, 12H]
 *   0x1: [8L, 4H]        0x9: [4L, 8H, 4L]
 *   0x2: [8L, 4H, 4L]    0xA: [4L, 4H, 4L, 4H]
 *   0x3: [8L, 8H]        0xB: [4L, 4H, 8L]
 *   0x4: [4L, 4H, 8L]    0xC: [8L, 8H]
 *   0x5: [4L, 4H, 4L, 4H] 0xD: [8L, 4H, 4L]
 *   0x6: [4L, 8H, 4L]    0xE: [12L, 4H]
 *   0x7: [4L, 12H]       0xF: [16H]
 * ====================================================================== */

#define NIBBLE_MATCH(val, target) ((val) >= ((target) - 1) && (val) <= ((target) + 1))

typedef struct {
    uint8_t pulses[4];
    bool    levels[4];
    uint8_t count;
} nibble_item_t;

static int8_t IRAM_ATTR decode_nibble_lut(const nibble_item_t *n, uint8_t *consumed) {
    if (n->count == 0) return -1;

    uint8_t p0 = n->pulses[0];
    bool hi0 = n->levels[0];

    if (!hi0) {
        /* === Группа начинающаяся с LOW (0x0..0x7) === */
        if (NIBBLE_MATCH(p0, 12) && n->count >= 1) {
            /* Проверяем что ВЕСЬ ниббл в одном item (нет后续 HIGH) */
            if (n->count == 1 || !n->levels[1]) {
                *consumed = 1; return 0x0;
            }
        }
        if (NIBBLE_MATCH(p0, 8) && n->count >= 2 && NIBBLE_MATCH(n->pulses[1], 4) && n->levels[1]) {
            if (n->count == 2 || !n->levels[2]) {
                *consumed = 2; return 0x1;
            }
        }
        if (NIBBLE_MATCH(p0, 8) && n->count >= 3 && NIBBLE_MATCH(n->pulses[1], 4) && n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 4) && !n->levels[2]) {
            *consumed = 3; return 0x2;
        }
        if (NIBBLE_MATCH(p0, 8) && n->count >= 2 && NIBBLE_MATCH(n->pulses[1], 8) && n->levels[1]) {
            *consumed = 2; return 0x3;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 3 && NIBBLE_MATCH(n->pulses[1], 4) && n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 8) && !n->levels[2]) {
            *consumed = 3; return 0x4;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 4 && NIBBLE_MATCH(n->pulses[1], 4) && n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 4) && !n->levels[2]
            && NIBBLE_MATCH(n->pulses[3], 4) && n->levels[3]) {
            *consumed = 4; return 0x5;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 3 && NIBBLE_MATCH(n->pulses[1], 8) && n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 4) && !n->levels[2]) {
            *consumed = 3; return 0x6;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 2 && NIBBLE_MATCH(n->pulses[1], 12) && n->levels[1]) {
            *consumed = 2; return 0x7;
        }
    } else {
        /* === Группа начинающаяся с HIGH (0x8..0xF) === */
        if (NIBBLE_MATCH(p0, 4) && n->count >= 2 && NIBBLE_MATCH(n->pulses[1], 12) && !n->levels[1]) {
            *consumed = 2; return 0x8;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 3 && NIBBLE_MATCH(n->pulses[1], 8) && !n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 4) && n->levels[2]) {
            *consumed = 3; return 0x9;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 4 && NIBBLE_MATCH(n->pulses[1], 4) && !n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 4) && n->levels[2]
            && NIBBLE_MATCH(n->pulses[3], 4) && !n->levels[3]) {
            *consumed = 4; return 0xA;
        }
        if (NIBBLE_MATCH(p0, 4) && n->count >= 3 && NIBBLE_MATCH(n->pulses[1], 4) && !n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 8) && n->levels[2]) {
            *consumed = 3; return 0xB;
        }
        if (NIBBLE_MATCH(p0, 8) && n->count >= 2 && NIBBLE_MATCH(n->pulses[1], 8) && !n->levels[1]) {
            *consumed = 2; return 0xC;
        }
        if (NIBBLE_MATCH(p0, 8) && n->count >= 3 && NIBBLE_MATCH(n->pulses[1], 4) && !n->levels[1]
            && NIBBLE_MATCH(n->pulses[2], 4) && n->levels[2]) {
            *consumed = 3; return 0xD;
        }
        if (NIBBLE_MATCH(p0, 12) && n->count >= 2 && NIBBLE_MATCH(n->pulses[1], 4) && !n->levels[1]) {
            *consumed = 2; return 0xE;
        }
        if (NIBBLE_MATCH(p0, 16) && n->count >= 1) {
            *consumed = 1; return 0xF;
        }
    }

    return -1;
}

/* ======================================================================
 * CARRY BUFFER + NIBBLE DECODE С PHASE TRACKING
 * ======================================================================
 *
 * Phase:
 *   0 = ожидаем ниббл (нижний или верхний — фаза неизвестна)
 *   1 = ожидаем стартовый бит (LOW) — начало нибbla
 *   2 = внутри нибbla — данные
 *   3 = стоп-биты нибbla — ожидаем HIGH
 *
 * Carry: остаток items от предыдущего batch, не полностью обработанных
 * ====================================================================== */

/* Decode одного нибbla из потока RMT items с учётом carry */
static int8_t IRAM_ATTR decode_nibble_from_stream(
    const uint8_t *pulses, const bool *levels, uint8_t count,
    uint8_t *consumed)
{
    nibble_item_t n;
    n.count = (count > 4) ? 4 : count;
    for (int i = 0; i < n.count; i++) {
        n.pulses[i] = pulses[i];
        n.levels[i] = levels[i];
    }
    return decode_nibble_lut(&n, consumed);
}

/*
 * Обработка потока RMT items с carry buffer и phase tracking.
 *
 * @param stream_idx   Индекс DMX порта (0 или 1)
 * @param rmt_items    Массив {duration_ticks, level} из RMT callback
 * @param rmt_count    Количество items
 */
static void decode_stream_with_carry(int stream_idx,
                                     const uint8_t *rmt_pulses,
                                     const bool *rmt_levels,
                                     uint8_t rmt_count,
                                     sw_uart_ctx_t *ctx)
{
    /* Рабочий буфер: carry + новые items */
    uint8_t work_pulses[8 + 64];
    bool    work_levels[8 + 64];
    uint8_t work_count = 0;

    /* Шаг 1: carry из прошлого batch */
    if (ctx->carry_count > 0) {
        for (int i = 0; i < ctx->carry_count; i++) {
            work_pulses[i] = ctx->carry_pulses[i];
            work_levels[i] = ctx->carry_levels[i];
        }
        work_count = ctx->carry_count;
        ctx->carry_count = 0;
    }

    /* Шаг 2: новые items */
    for (int i = 0; i < rmt_count && work_count < sizeof(work_pulses); i++) {
        work_pulses[work_count] = rmt_pulses[i];
        work_levels[work_count] = rmt_levels[i];
        work_count++;
    }

    /* Шаг 3: декод нибблов */
    uint8_t idx = 0;

    while (idx < work_count) {
        uint8_t consumed = 0;
        int8_t nibble = decode_nibble_from_stream(
            &work_pulses[idx], &work_levels[idx], work_count - idx, &consumed);

        if (nibble < 0) {
            /* LUT не матчит — сохраняем остаток в carry */
            break;
        }

        if (ctx->nibble_phase == 0) {
            ctx->current_byte = nibble;
            ctx->nibble_phase = 1;
        } else {
            ctx->current_byte |= (nibble << 4);
            if (ctx->byte_count < DMX_CHANNELS) {
                ctx->rx_active[ctx->byte_count] = ctx->current_byte;
                ctx->byte_count++;
            }
            ctx->current_byte = 0;
            ctx->nibble_phase = 0;
        }

        idx += consumed;
    }

    /* Шаг 4: остаток в carry */
    uint8_t remaining = work_count - idx;
    ctx->carry_count = 0;
    if (remaining > 0 && remaining <= 8) {
        for (int i = 0; i < remaining; i++) {
            ctx->carry_pulses[i] = work_pulses[idx + i];
            ctx->carry_levels[i] = work_levels[idx + i];
        }
        ctx->carry_count = remaining;
    }
}

/*
 * Task приёма RMT RX — обрабатывает символы из очереди.
 */
static void rmt_rx_task(void *arg)
{
    int port = (int)arg;
    sw_uart_ctx_t *ctx = &s_ctx[port];
    rmt_rx_msg_t msg;

    /* Буфер приёма RMT */
    rmt_symbol_word_t rx_buf[RMT_RX_BUF_SYMBOLS];

    /* Буферы для извлечения items из RMT символов */
    uint8_t item_pulses[128];
    bool    item_levels[128];

    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = 1000,   /* Мин. 1мкс */
        .signal_range_max_ns = 100000, /* Макс. 100мкс — BREAK=88мкс */
    };

    ESP_LOGI(TAG, "RMT RX task started for port %d (GPIO%d)", port, ctx->gpio_rx);

    while (1) {
        ESP_ERROR_CHECK(rmt_receive(ctx->rmt_rx_chan, rx_buf,
                                     sizeof(rx_buf), &rx_cfg));

        while (xQueueReceive(ctx->rmt_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            /* Извлекаем items: каждый rmt_symbol_word_t = 2 items */
            const uint16_t *raw = (const uint16_t *)msg.symbols;
            int total_items = msg.num_symbols * 2;
            int item_count = 0;

            for (int i = 0; i < total_items && item_count < 128; i++) {
                uint16_t val = raw[i];
                int lvl = (val >> 15) & 1;
                uint32_t dur = val & 0x7FFF;
                if (dur == 0) continue;

                /* BREAK detection: LOW > 88 тиков (88мкс при 1MHz) */
                if (lvl == 0 && dur >= 88) {
                    /* Завершить предыдущий кадр */
                    if (ctx->byte_count > 0) {
                        memcpy(ctx->rx_done, ctx->rx_active, ctx->byte_count);
                        ctx->last_frame_len = ctx->byte_count;
                        ctx->frame_count++;
                        ctx->frame_ready = true;
                        if (ctx->notify_task) {
                            BaseType_t wake = pdFALSE;
                            vTaskNotifyGiveFromISR(ctx->notify_task, &wake);
                        }
                    }
                    ctx->byte_count = 0;
                    ctx->nibble_phase = 0;
                    ctx->current_byte = 0;
                    ctx->carry_count = 0;
                    ctx->in_frame = true;
                    ctx->break_count++;
                    item_count = 0;
                    continue;
                }

                item_pulses[item_count] = (uint8_t)(dur > 255 ? 255 : dur);
                item_levels[item_count] = (lvl != 0);
                item_count++;
            }

            if (item_count > 0 && ctx->in_frame) {
                decode_stream_with_carry(port, item_pulses, item_levels, item_count, ctx);
            }

            /* is_last = таймаут → кадр завершён */
            if (msg.is_last && ctx->in_frame && ctx->byte_count > 0) {
                memcpy(ctx->rx_done, ctx->rx_active, ctx->byte_count);
                ctx->last_frame_len = ctx->byte_count;
                ctx->frame_count++;
                ctx->frame_ready = true;
                ctx->in_frame = false;

                if (ctx->notify_task) {
                    BaseType_t wake = pdFALSE;
                    vTaskNotifyGiveFromISR(ctx->notify_task, &wake);
                }
            }

            /* Re-arm */
            if (!msg.is_last) {
                ESP_ERROR_CHECK(rmt_receive(ctx->rmt_rx_chan, rx_buf,
                                             sizeof(rx_buf), &rx_cfg));
            }
        }
    }
}

#endif /* DMX_SW_UART_MODE == 1 */

/* ======================================================================
 *  GPIO EDGE TIMING РЕЖИМ (DMX_SW_UART_MODE == 0)
 * ====================================================================== */

#if DMX_SW_UART_MODE == 0

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    int port = (int)arg;
    sw_uart_ctx_t *ctx = &s_ctx[port];

    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(ctx->gpio_rx);
    int64_t delta = now - ctx->last_edge_us;
    ctx->last_edge_us = now;
    ctx->isr_count++;

    if (level == 0) {
        int64_t falling_gap = now - ctx->last_falling_us;
        ctx->last_falling_us = now;

        if (falling_gap > FRAME_GAP_US) {
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

            ctx->rx_head = 0;
            ctx->in_frame = true;
            ctx->rx_byte = 0;
            ctx->bitcount = 0;
            ctx->seen_break = true;
            ctx->break_count++;
            return;
        }

        goto process_bits;
    }

process_bits:
    if (!ctx->in_frame || ctx->bitcount >= 10) return;
    if (delta == 0) return;

    int prev_level = !level;
    int bits = (int)((delta + BIT_PERIOD_US / 2) / BIT_PERIOD_US);
    if (bits <= 0) bits = 1;

    for (int i = 0; i < bits && ctx->bitcount < 10; i++) {
        if (ctx->bitcount == 0) {
            if (prev_level != 0) {
                ctx->err_count++;
                ctx->in_frame = false;
                return;
            }
        } else if (ctx->bitcount <= 8) {
            if (prev_level) {
                ctx->rx_byte |= (1 << (ctx->bitcount - 1));
            }
        }
        ctx->bitcount++;
    }

    if (ctx->bitcount >= 10) {
        if (prev_level == 0 && bits >= 1) {
            ctx->err_count++;
        }
        if (ctx->rx_head < DMX_CHANNELS) {
            ctx->rx_active[ctx->rx_head++] = ctx->rx_byte;
        }
        ctx->rx_byte = 0;
        ctx->bitcount = 0;
    }
}

#endif /* DMX_SW_UART_MODE == 0 */

/* ======================================================================
 *  ОБЩИЙ API: init / start / get_frame / set_tx_mode
 * ====================================================================== */

void uart_bypass_init(int port) {
    sw_uart_ctx_t *ctx = &s_ctx[port];

    ctx->rx_head = 0;
    ctx->last_frame_len = 0;
    ctx->frame_ready = false;
    ctx->frame_count = 0;
    ctx->break_count = 0;
    ctx->err_count = 0;
    ctx->isr_count = 0;
    ctx->notify_task = NULL;

    if (port == 0) {
        ctx->gpio_rx = DMX_GPIO_RX1;
        ctx->uart_tx = UART_NUM_1;
    } else {
        ctx->gpio_rx = DMX_GPIO_RX2;
        ctx->uart_tx = UART_NUM_2;
    }

#if DMX_SW_UART_MODE == 0
    ctx->last_edge_us = 0;
    ctx->last_falling_us = 0;
    ctx->rx_byte = 0;
    ctx->bitcount = 0;
    ctx->in_frame = false;
    ctx->seen_break = false;
    portMUX_INITIALIZE(&ctx->mux);
#endif

#if DMX_SW_UART_MODE == 1
    ctx->in_frame = false;
    ctx->rmt_rx_chan = NULL;
    ctx->rmt_rx_queue = NULL;
    ctx->rmt_rx_task = NULL;
    ctx->timeout_thresh = DMX_RMT_IDLE_US;
    ctx->nibble_phase = 0;
    ctx->current_byte = 0;
    ctx->byte_count = 0;
    ctx->carry_count = 0;
#endif
}

void uart_bypass_init_all(void) {
    uart_bypass_init(0);
    uart_bypass_init(1);

#if DMX_SW_UART_MODE == 0
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << DMX_GPIO_RX1) | (1ULL << DMX_GPIO_RX2),
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(DMX_GPIO_RX1, gpio_isr_handler, (void *)0);
    gpio_isr_handler_add(DMX_GPIO_RX2, gpio_isr_handler, (void *)1);

    ESP_LOGI(TAG, "GPIO edge RX on GPIO%d (port0) and GPIO%d (port1)",
             DMX_GPIO_RX1, DMX_GPIO_RX2);
#endif

#if DMX_SW_UART_MODE == 1
    /* RMT RX для каждого порта */
    for (int port = 0; port < 2; port++) {
        sw_uart_ctx_t *ctx = &s_ctx[port];

        /* Очередь RMT callback → task */
        ctx->rmt_rx_queue = xQueueCreate(8, sizeof(rmt_rx_msg_t));
        assert(ctx->rmt_rx_queue);

        /* RMT RX channel */
        rmt_rx_channel_config_t rx_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = DMX_RMT_CLK_HZ,
            .mem_block_symbols = RMT_RX_BUF_SYMBOLS,
            .gpio_num = ctx->gpio_rx,
            .flags.invert_in = false,
            .flags.with_dma = false,
        };
        ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &ctx->rmt_rx_chan));

        /* Callback */
        rmt_rx_event_callbacks_t cbs = {
            .on_recv_done = rmt_rx_done_cb,
        };
        ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(ctx->rmt_rx_chan, &cbs, (void *)port));
        ESP_ERROR_CHECK(rmt_enable(ctx->rmt_rx_chan));

        /* RX task */
        xTaskCreate(rmt_rx_task, port ? "rmt_rx1" : "rmt_rx0",
                    4096, (void *)port, 5, &ctx->rmt_rx_task);

        ESP_LOGI(TAG, "RMT RX port%d on GPIO%d (ch%d)",
                 port, ctx->gpio_rx,
                 port == 0 ? DMX_RMT_RX_CH_PORT0 : DMX_RMT_RX_CH_PORT1);
    }
#endif

    /* GPIO DIR для второго порта */
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
    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, DMX_GPIO_TX1, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    QueueHandle_t q0;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 0, 1024, 0, &q0, 0));

    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, DMX_GPIO_TX2, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    QueueHandle_t q1;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 0, 1024, 0, &q1, 0));

    ESP_LOGI(TAG, "TX UARTs on UART1=GPIO%d, UART2=GPIO%d",
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

#if DMX_SW_UART_MODE == 0
    if (tx_mode) {
        gpio_intr_disable(ctx->gpio_rx);
    } else {
        ctx->rx_head = 0;
        ctx->in_frame = false;
        ctx->seen_break = false;
        ctx->frame_ready = false;
        ctx->bitcount = 0;
        ctx->rx_byte = 0;
        gpio_intr_enable(ctx->gpio_rx);
    }
#endif

#if DMX_SW_UART_MODE == 1
    if (tx_mode) {
        rmt_disable(ctx->rmt_rx_chan);
    } else {
        ctx->rx_head = 0;
        ctx->in_frame = false;
        ctx->frame_ready = false;
        rmt_enable(ctx->rmt_rx_chan);
    }
#endif
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
