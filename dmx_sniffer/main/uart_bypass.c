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
    uint32_t byte_accum;
    int bit_idx;
    int byte_count;
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
 * LUT: RMT TICKS → КОЛИЧЕСТВО БИТ (для RMT RX режима)
 * ======================================================================
 *
 * RMT клок = 10MHz → 1 тик = 0.1мкс
 * DMX 250kbaud → 1 бит = 4 мкс = 40 тиков
 *
 * LUT индексируется: (duration_ticks >> 2) → биты
 * 1 тик = 0.1мкс, шаг LUT = 4 тика = 0.4мкс
 * Макс. покрытие: 256 × 4 = 1024 тика = 102.4мкс
 * Этого хватает для BREAK (880 тиков = 88мкс) и всех битовых паттернов
 * ====================================================================== */

#define LUT_SHIFT   2
#define LUT_ENTRIES 256
#define RMT_TICKS_PER_BIT 40

static const uint8_t duration_to_bits_lut[LUT_ENTRIES] = {
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /* 0-15   =  0-60 ticks  = 0 бит (шум/мусор) */
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 1, 1,  1, 1, 1, 1, /* 16-31  = 64-124 ticks */
     1, 1, 1, 1,  1, 1, 1, 1,  2, 2, 2, 2,  2, 2, 2, 2, /* 32-47  = 128-188 ticks */
     2, 2, 2, 2,  3, 3, 3, 3,  3, 3, 3, 3,  3, 3, 3, 3, /* 48-63  = 192-252 ticks */
     4, 4, 4, 4,  4, 4, 4, 4,  5, 5, 5, 5,  5, 5, 5, 5, /* 64-79  = 256-316 ticks */
     5, 5, 5, 5,  6, 6, 6, 6,  6, 6, 6, 6,  6, 6, 6, 6, /* 80-95  = 320-380 ticks */
     7, 7, 7, 7,  7, 7, 7, 7,  8, 8, 8, 8,  8, 8, 8, 8, /* 96-111 = 384-444 ticks */
     8, 8, 8, 8,  9, 9, 9, 9,  9, 9, 9, 9,  9, 9, 9, 9, /* 112-127 = 448-508 ticks */
    10,10,10,10, 10,10,10,10, 11,11,11,11, 11,11,11,11, /* 128-143 = 512-572 ticks */
    11,11,11,11, 12,12,12,12, 12,12,12,12, 12,12,12,12, /* 144-159 = 576-636 ticks */
    13,13,13,13, 13,13,13,13, 14,14,14,14, 14,14,14,14, /* 160-175 = 640-700 ticks */
    14,14,14,14, 15,15,15,15, 15,15,15,15, 15,15,15,15, /* 176-191 = 704-764 ticks */
    16,16,16,16, 16,16,16,16, 17,17,17,17, 17,17,17,17, /* 192-207 = 768-828 ticks */
    17,17,17,17, 18,18,18,18, 18,18,18,18, 18,18,18,18, /* 208-223 = 832-892 ticks */
    19,19,19,19, 19,19,19,19, 20,20,20,20, 20,20,20,20, /* 224-239 = 896-956 ticks */
    20,20,20,20, 21,21,21,21, 21,21,21,21, 21,21,21,21, /* 240-255 = 960-1020 ticks */
};

/* Порог BREAK в тиках: 88мкс × 10 тиков/мкс = 880 */
#define BREAK_TICKS  880
/* Порог MAB: ≥8мкс = 80 тиков. Пауза между кадрами: >88мкс = 880 тиков */

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

/*
 * LUT быстрый декод: duration_in_ticks → количество бит
 */
static inline int IRAM_ATTR lut_decode_bits(uint32_t dur_ticks) {
    if (dur_ticks >= (uint32_t)(LUT_ENTRIES << LUT_SHIFT)) return 22;
    return duration_to_bits_lut[dur_ticks >> LUT_SHIFT];
}

/*
 * Декод буфера RMT символов в DMX байты.
 *
 * RMT symbol_word_t = 32 бита: [dur0:15|level0:1][dur1:15|level1:1]
 * Разбивается на 2 rmt_item16_t.
 *
 * DMX512 UART frame на один байт:
 *   [START=LOW 40тик][D0][D1]...[D7][STOP=HIGH 40тик][STOP=HIGH 40тик]
 *   LSB first: D0 — младший бит
 *
 * RMT RX на линии видит:
 *   BREAK: LOW > 880 тиков → начало нового кадра
 *   MAB:   HIGH ≥80 тиков
 *   START: LOW ~40 тиков
 *   Данные: чередование HIGH/LOW по 40 тиков (или кратные)
 *   STOP:  HIGH 80 тиков (2 стоп-бита)
 *
 * Декодер:
 *   - Накапливаем биты в текущем байте (bit_idx 0-9)
 *   - bit_idx=0: стартовый бит (LOW), проверяем
 *   - bit_idx=1..8: данные (LSB first)
 *   - bit_idx=9..10: стоп-биты (HIGH), проверяем
 *   - После bit_idx=10: байт готов → store, reset
 * ====================================================================== */

static void IRAM_ATTR decode_rmt_buffer(const rmt_symbol_word_t *syms,
                                         int num_symbols,
                                         sw_uart_ctx_t *ctx)
{
    /* Используем state из контекста (меж-batch) */
    uint32_t *accum = &ctx->byte_accum;
    int *bidx = &ctx->bit_idx;
    int *bcount = &ctx->byte_count;

    /* Разбиваем rmt_symbol_word_t на rmt_item16_t:
     * item[2*i]   = { level0, duration0 }
     * item[2*i+1] = { level1, duration1 } */
    const uint16_t *raw = (const uint16_t *)syms;
    int total_items = num_symbols * 2;

    for (int i = 0; i < total_items; i++) {
        uint16_t val = raw[i];
        int lvl = (val >> 15) & 1;          /* level: бит 15 */
        uint32_t dur = (uint32_t)(val & 0x7FFF); /* duration: биты 0-14 */

        if (dur == 0) continue;

        int bits = lut_decode_bits(dur);

        /* === BREAK DETECTION === */
        if (lvl == 0 && bits >= 22) {
            /* LOW > 880 тиков = BREAK → завершить предыдущий кадр */
            if (*bcount > 0) {
                ctx->rx_head = *bcount;
                memcpy(ctx->rx_done, ctx->rx_active, ctx->rx_head);
                ctx->last_frame_len = ctx->rx_head;
                ctx->frame_count++;
                ctx->frame_ready = true;
                if (ctx->notify_task) {
                    BaseType_t wake = pdFALSE;
                    vTaskNotifyGiveFromISR(ctx->notify_task, &wake);
                }
            }
            /* Сброс state */
            *bcount = 0;
            *bidx = 0;
            *accum = 0;
            memset(ctx->rx_active, 0, DMX_CHANNELS);
            ctx->in_frame = true;
            ctx->break_count++;
            continue;
        }

        if (!ctx->in_frame) continue;

        /* === DECODE BITS === */
        for (int b = 0; b < bits; b++) {
            if (*bidx == 0) {
                if (lvl != 0) {
                    ctx->err_count++;
                    ctx->in_frame = false;
                    break;
                }
                *bidx = 1;
                continue;
            }

            if (*bidx >= 1 && *bidx <= 8) {
                if (lvl) {
                    *accum |= (1U << (*bidx - 1));
                }
                (*bidx)++;
                continue;
            }

            if (*bidx >= 9) {
                if (lvl == 0) {
                    ctx->err_count++;
                }
                (*bidx)++;

                if (*bidx >= 11) {
                    if (*bcount < DMX_CHANNELS) {
                        ctx->rx_active[*bcount] = *accum & 0xFF;
                        (*bcount)++;
                    }
                    *bidx = 0;
                    *accum = 0;
                }
                continue;
            }
        }
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

    /* Буфер приёма RMT: 64 words = 256 байт, на стеке таска (4KB стек хватит) */
    rmt_symbol_word_t rx_buf[RMT_RX_BUF_SYMBOLS];

    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = 100,    /* Мин. 1 тик (100нс) */
        .signal_range_max_ns = 100000, /* Макс. 100мкс — BREAK проходит */
    };

    ESP_LOGI(TAG, "RMT RX task started for port %d (GPIO%d)", port, ctx->gpio_rx);

    while (1) {
        /* Запускаем приём в выделенный буфер */
        ESP_ERROR_CHECK(rmt_receive(ctx->rmt_rx_chan, rx_buf,
                                     sizeof(rx_buf), &rx_cfg));

        /* Ждём данные из callback */
        while (xQueueReceive(ctx->rmt_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            /* Декодируем RMT символы → DMX байты */
            decode_rmt_buffer(msg.symbols, msg.num_symbols, ctx);

            /* is_last = таймаут (линия тихая) → кадр завершён */
            if (msg.is_last && ctx->in_frame && ctx->rx_head > 0) {
                /* Кадр завершён по таймауту RMT */
                memcpy(ctx->rx_done, ctx->rx_active, ctx->rx_head);
                ctx->last_frame_len = ctx->rx_head;
                ctx->frame_count++;
                ctx->frame_ready = true;
                ctx->in_frame = false;

                if (ctx->notify_task) {
                    BaseType_t wake = pdFALSE;
                    vTaskNotifyGiveFromISR(ctx->notify_task, &wake);
                }
            }

            /* Re-arm приёма (ESP32 без ping-pong) */
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
    ctx->byte_accum = 0;
    ctx->bit_idx = 0;
    ctx->byte_count = 0;
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
