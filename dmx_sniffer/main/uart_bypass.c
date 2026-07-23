#include "uart_bypass.h"
#include "settings.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "hal/uart_ll.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "rom/ets_sys.h"
#include "dmx_hal.h"
#include "dmx.h"
#include <string.h>

static const char *TAG = "UART_SW";

/* ======================================================================
 * DMX512 ПАРАМЕТРЫ
 * ======================================================================
 * DMX512: 250kbaud, 8N2 (8 data, no parity, 2 stop bits).
 * Break: ≥88мкс LOW на линии. QLC+ отправляет ~120мкс.
 * MAB (Mark After Break): ≥8мкс HIGH между break и данными.
 * Формат кадра: [BREAK] [MAB] [Start Code] [CH1..CH512]
 * Время передачи 513 байт: 513 × 11 × 4мкс = 22.57мкс/бит.
 * Полный кадр DMX512: ~22.6мс (при 30fps ≈ 33мс период).
 * ====================================================================== */
#define UART_BAUD_RATE  250000
#define BIT_PERIOD_US   4
#define BREAK_MIN_US    88
#define FRAME_GAP_US    100

/* ======================================================================
 * ОБЩАЯ СТРУКТУРА СОСТОЯНИЯ ДЛЯ КАЖДОГО DMX ПОРТА
 * ======================================================================
 * Двойная буферизация (double buffering):
 *   rx_active[] — заполняется ISR во время приёма кадра
 *   rx_done[]   — готовый кадр для чтения задачей
 *   При BREAK_DET: атомарно копируем rx_active → rx_done (memcpy),
 *   задача читает из rx_done через uart_bypass_get_frame().
 *
 * Поля:
 *   rx_active[]    — буфер приёма (заполняется ISR)
 *   rx_done[]      — буфер готового кадра (копируется из rx_active)
 *   rx_head        — текущая позиция записи в rx_active
 *   last_frame_len — длина последнего принятого кадра
 *   frame_ready    — флаг: готовый кадр доступен для чтения
 *   frame_count    — счётчик принятых кадров (uint32_t, переполнение через ~4.5 года при 30fps)
 *   break_count    — счётчик обнаруженных break-сигналов
 *   err_count      — счётчик ошибок (фантомные байты во время break, переполнение через ~10 лет)
 *   isr_count      — счётчик вызовов ISR
 *   notify_task    — задача для уведомления о готовом кадре (FreeRTOS notification)
 *   gpio_rx        — GPIO пин приёма
 *   uart_tx        — UART порт для передачи (тест/патчер)
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
    /* Bit-level decoder state (меж-batch) */
    uint8_t bit_pos;            /* 0=waiting start, 1-8=data, 9-10=stop */
    uint16_t byte_val;          /* accumulated byte value */
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

#if DMX_SW_UART_MODE == 2
    /* HW UART RX specific */
    bool in_frame;
    uint32_t last_break_cyc;
    intr_handle_t intr_handle;
#endif
} sw_uart_ctx_t;

static sw_uart_ctx_t s_ctx[2];

/* ======================================================================
 *  RMT RX РЕЖИМ (DMX_SW_UART_MODE == 1)
 * ====================================================================== */

#if DMX_SW_UART_MODE == 1

/* Буфер RMT RX: 64 rmt_symbol_word_t = 128 rmt_item16_t.
 * 513 байт DMX декодируется через МНОГОКАЗОВЫЙ callback.
 * Каждый callback = 64 symbols ≈ 20-30 байт, цикл продолжается до is_last (idle).
 * Итого ~25 callback на кадр, byte_count накапливается. */
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
 * БИТОВЫЙ ДЕКОДЕР: RMT items → DMX bytes (4мкс/бит)
 * ======================================================================
 *
 * DMX512 = 250kbaud = 4мкс/бит. Каждый байт:
 *   1 стартовый бит (LOW, 4мкс)
 *   8 бит данных (LSB first, каждый 4мкс)
 *   2 стоп-бита (HIGH, 8мкс всего)
 *
 * RMT items: [level, duration_ticks] при 1MHz = 1 тик = 1мкс.
 * RMT ОБЪЕДИНЯЕТ подряд идущие одинаковые уровни:
 *   0x00 → [LOW, 36] [HIGH, 8]   (старт+8×0=36мкс LOW)
 *   0xFF → [LOW, 4] [HIGH, 40]   (старт=4мкс LOW, 8×1+стоп=40мкс HIGH)
 *   0x55 → [LOW,4][HIGH,4][LOW,4]...[HIGH,8]
 *
 * Алгоритм: для каждого item делим duration на 4мкс → количество бит.
 * Бит 0 = старт (LOW), биты 1-8 = данные, биты 9-10 = стоп (HIGH).
 * ====================================================================== */

/*
 * Декодирование байтов из потока RMT items.
 * Заменяет nibble-декодер — работает напрямую с битами (4мкс/бит).
 *
 * @param stream_idx   Индекс DMX порта (для логирования)
 * @param rmt_pulses   Массив длительностей (в тиках)
 * @param rmt_levels   Массив уровней (true=HIGH)
 * @param rmt_count    Количество items
 * @param ctx          Контекст декодера
 */
static void decode_bytes_from_items(int stream_idx,
                                    const uint8_t *rmt_pulses,
                                    const bool *rmt_levels,
                                    uint8_t rmt_count,
                                    sw_uart_ctx_t *ctx)
{
    for (int i = 0; i < rmt_count; i++) {
        uint8_t dur = rmt_pulses[i];
        bool level = rmt_levels[i];

        /* Количество бит в этом item: duration / 4мкс */
        uint8_t num_bits = dur / BIT_PERIOD_US;
        if (num_bits == 0) num_bits = 1;

        for (uint8_t b = 0; b < num_bits; b++) {
            if (ctx->bit_pos == 0) {
                /* Ожидаем стартовый бит (LOW) */
                if (!level) {
                    ctx->bit_pos = 1;
                    ctx->byte_val = 0;
                }
                /* HIGH — межбайтовая пауза, пропускаем */
            } else if (ctx->bit_pos <= 8) {
                /* Бит данных (LSB first) */
                if (level) {
                    ctx->byte_val |= (1 << (ctx->bit_pos - 1));
                }
                ctx->bit_pos++;
            } else {
                /* Стоп-биты (биты 9, 10) — ожидаем HIGH */
                ctx->bit_pos++;
                if (ctx->bit_pos >= 11) {
                    /* Байт готов */
                    if (ctx->byte_count < DMX_CHANNELS) {
                        ctx->rx_active[ctx->byte_count] = ctx->byte_val;
                        ctx->byte_count++;
                    }
                    ctx->bit_pos = 0;
                    ctx->byte_val = 0;
                }
            }
        }
    }
}

/*
 * Task приёма RMT RX — обрабатывает символы из очереди.
 *
 * КРИТИЧЕСКИ ВАЖНО для ESP32:
 *   rmt_disable()/rmt_enable() НЕ останавливают движок (нет async stop).
 *   После re-arm движок может выдать мусорный callback с предыдущего захвата.
 *   Решение: пропускать сообщения с num_symbols==0 или dur==0 (мусор).
 *   Также: signal_range_max_ns > BREAK (>88мкс) чтобы idle не срабатывал
 *   во время данных, а только в паузах между кадрами.
 */
static void rmt_rx_task(void *arg)
{
    int port = (int)arg;
    sw_uart_ctx_t *ctx = &s_ctx[port];
    rmt_rx_msg_t msg;

    /* Буферы на стеке — каждая задача (port0/port1) свои */
    rmt_symbol_word_t rx_buf[RMT_RX_BUF_SYMBOLS];
    uint8_t item_pulses[128];
    bool    item_levels[128];

    /*
     * signal_range_max_ns: при 1MHz → 1 тик = 1мкс.
     * DMX кадр: BREAK(120мкс) + MAB(8мкс) + данные(~22мс) + MTBP(≥8мкс).
     * Между кадрами пауза ≥ 8мкс (MTBP).
     * Ставим 3000мкс (3мс) — idle сработает только в паузах между кадрами,
     * НЕ во время BREAK и НЕ во время данных.
     * При этом движок сам остановится через 3мс тишины на линии.
     */
    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = 1000,        /* 1мкс — глитч-фильтр */
        .signal_range_max_ns = 3000000,     /* 3мс — idle только в паузах */
    };

    ESP_LOGI(TAG, "RMT RX task started for port %d (GPIO%d)", port, ctx->gpio_rx);

    while (1) {
        /* === RE-ARM CYCLE ===
         * На ESP32 rmt_disable() не останавливает движок (нет async stop).
         * Движок продолжает работать и может выдать callback с мусором.
         * Стратегия:
         * 1. rmt_disable() — запрещаем новые захваты
         * 2. Ждём 2мс — даём движку завершить текущий захват
         * 3. Очищаем очередь от мусорных сообщений
         * 4. rmt_enable() + rmt_receive() — начинаем новый захват
         */
        rmt_disable(ctx->rmt_rx_chan);
        vTaskDelay(pdMS_TO_TICKS(2));

        /* Очистить очередь от мусорных callback после disable */
        { rmt_rx_msg_t stale;
          while (xQueueReceive(ctx->rmt_rx_queue, &stale, 0) == pdTRUE) {
              ctx->err_count++;
          }
        }

        rmt_enable(ctx->rmt_rx_chan);
        vTaskDelay(pdMS_TO_TICKS(1));

        esp_err_t err = rmt_receive(ctx->rmt_rx_chan, rx_buf,
                                     sizeof(rx_buf), &rx_cfg);
        if (err != ESP_OK) {
            ctx->err_count++;
            ESP_LOGW(TAG, "rmt_receive err: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Ждём реальный callback с данными */
        while (xQueueReceive(ctx->rmt_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {

            /* === ФИЛЬТРАЦИЯ МУСОРА ===
             * После re-arm на ESP32 движок может выдать callback с:
             *   num_symbols==0 — пустой захват
             *   Все символы с dur==0 — мусор от предыдущего захвата
             * Пропускаем такие сообщения.
             */
            bool is_garbage = (msg.num_symbols == 0);
            if (!is_garbage && msg.num_symbols > 0) {
                const uint16_t *raw_check = (const uint16_t *)msg.symbols;
                bool all_zero = true;
                for (int i = 0; i < msg.num_symbols * 2; i++) {
                    if ((raw_check[i] & 0x7FFF) != 0) {
                        all_zero = false;
                        break;
                    }
                }
                is_garbage = all_zero;
            }
            if (is_garbage) {
                /* Мусор от re-arm — пропускаем */
                continue;
            }

            /* Извлекаем items: каждый rmt_symbol_word_t = 2 items */
            const uint16_t *raw = (const uint16_t *)msg.symbols;
            int total_items = msg.num_symbols * 2;
            int item_count = 0;

            /* Scan ALL items for BREAK first */
            int break_item_idx = -1;
            for (int i = 0; i < total_items; i++) {
                uint16_t val = raw[i];
                int lvl = (val >> 15) & 1;
                uint32_t dur = val & 0x7FFF;
                if (lvl == 0 && dur >= 88) {
                    break_item_idx = i;
                    break;
                }
            }


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
                    ctx->bit_pos = 0;
                    ctx->byte_val = 0;
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
                decode_bytes_from_items(port, item_pulses, item_levels, item_count, ctx);
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

            if (msg.is_last) {
                /* Idle timeout → break for full re-arm cycle */
                break;
            }
            /* Buffer-full: on ESP32 original no en_partial_rx support.
             * Must re-arm with full disable/enable cycle to continue. */
            rmt_disable(ctx->rmt_rx_chan);
            esp_err_t re = rmt_enable(ctx->rmt_rx_chan);
            if (re != ESP_OK) { ctx->err_count++; break; }
            re = rmt_receive(ctx->rmt_rx_chan, rx_buf,
                             sizeof(rx_buf), &rx_cfg);
            if (re != ESP_OK) { ctx->err_count++; break; }
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
 * HW UART RX РЕЖИМ (DMX_SW_UART_MODE == 2)
 * ======================================================================
 * АРХИТЕКТУРА:
 *   Используем аппаратный UART (UART1/UART2) для приёма DMX512.
 *   Полный bypass ESP-IDF UART driver: uart_driver_install() НЕ вызывается.
 *   Вместо этого: uart_param_config() настраивает регистры,
 *   uart_set_pin() подключает GPIO, esp_intr_alloc() ставит НАШ ISR.
 *
 * ISR (Interrupt Service Routine):
 *   uart_rx_isr() — обработчик прерываний UART1 и UART2.
 *   Вызывается аппаратным прерыванием, работает напрямую с регистрами.
 *   Читает FIFO побайтно: HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt)
 *   + hw->fifo.rw_byte. Не использует ESP-IDF UART driver.
 *
 * Прерывания UART (UART_HW_RX_ISR_FLAGS):
 *   UART_BRK_DET_INT_ENA  — обнаружен break (≥88мкс LOW)
 *   UART_FRM_ERR_INT_ENA  — ошибка кадра (стоп-бит != 1, нормально во время break)
 *   UART_RXFIFO_FULL_INT_ENA — FIFO заполнен до порога (32 байта)
 *   UART_RXFIFO_OVF_INT_ENA  — переполнение FIFO (если не успели прочитать)
 *
 * Проблема фантомных байтов (phantom bytes):
 *   Во время break (RXD=LOW) аппаратный UART интерпретирует LOW как старт-биты
 *   и генерирует "фантомные" байты с ошибкой кадра (FRM_ERR).
 *   err_wr_mask на ESP32 НЕ работает — фантомы попадают в FIFO.
 *
 *   Решение: FIFO_FULL порог = 32 байта. BREAK_DET срабатывает через ~44мкс
 *   (11 битных периодов × 4мкс). За это время в FIFO накапливаются фантомы.
 *   При BREAK_DET: дочитываем остаток FIFO, сохраняем предыдущий кадр,
 *   сбрасываем состояние для нового кадра. Фантомы просто дропаются.
 *
 * Таймауты:
 *   debounce: BREAK не может приходить чаще чем раз в 1мс (~160000 циклов при 160MHz).
 *   Если break пришёл слишком рано — дропаем и ждём следующего.
 *
 * Глобальный spinlock (s_global_fifo_mux):
 *   Защищает доступ к FIFO от гонок между двумя UART ISR (UART1 и UART2).
 *   Оба ISR вызываются на разных ядрах ESP32, но FIFO — общий ресурс APB шины.
 *   portENTER_CRITICAL_ISR / portEXIT_CRITICAL_ISR — блокируют прерывания + spinlock.
 *
 * Преимущества:
 *   - Аппаратный UART декодирует 250kbaud 8N2 без CPU
 *   - BREAK detection через аппаратное прерывание UART_BRK_DET
 *   - FIFO 128 байт, threshold 32 → прерывание каждые 32 байта
 *   - 0 CPU для захвата бит, только чтение FIFO в ISR (~1мкс)
 *
 * Результаты тестирования (15мин стресс-тест,.gradient 512ch, 30fps):
 *   - 16101 кадр, 0 ошибок каналов, оба порта синхронны
 *   - err_count ~7184 (фантомы, не влияют на данные)
 *   - FC0 == FC1 на протяжении всего теста
 * ====================================================================== */

#if DMX_SW_UART_MODE == 2

/* Глобальный spinlock — ОДИН на оба UART.
 * portENTER_CRITICAL_ISR блокирует прерывания на текущем ядре
 * и крутится если другое ядро держит этот же spinlock.
 * Зачем: ESP32 UART FIFO — общий ресурс APB шины. Одновременный доступ
 * с двух ядер вызывает "APB bus hang" (см. errata CPU-3.21). */
static portMUX_TYPE s_global_fifo_mux = portMUX_INITIALIZER_UNLOCKED;

/* Маска прерываний UART для приёма DMX:
 *   BREAK_DET  — обнаружен break (≥88мкс LOW) → начало нового кадра
 *   FRM_ERR    — ошибка кадра (стоп-бит != 1) → фантом во время break
 *   FIFO_FULL  — FIFO заполнен до порога (32 байта) → читаем данные
 *   FIFO_OVF   — переполнение FIFO → ошибка, дропаем */
#define UART_HW_RX_ISR_FLAGS  (UART_BRK_DET_INT_ENA | UART_FRM_ERR_INT_ENA | \
                                UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_OVF_INT_ENA)

/* Прямая маппинг UART периферии: UART1 → порт 0, UART2 → порт 1.
 * IRAM_ATTR — ISR должен быть в IRAM для быстрого вызова. */
static IRAM_ATTR uart_dev_t *uart_hw_by_port[] = { &UART1, &UART2 };

static void IRAM_ATTR uart_rx_isr(void *arg) {
    int port = (int)arg;
    sw_uart_ctx_t *ctx = &s_ctx[port];
    uart_dev_t *hw = uart_hw_by_port[port];

    /* Главный цикл ISR: обрабатываем ВСЕ.pending прерывания за один вызов.
     * Если interrupt status == 0 — выходим. Нет timer → нет таймаута,
     * ISR завершается когда все флаги очищены. */
    uint32_t int_st;
    while ((int_st = hw->int_st.val) != 0) {

        /* === RXFIFO_FULL: в FIFO накопилось ≥32 байта ===
         * Читаем ВСЕ байты из FIFO пока rxfifo_cnt > 0.
         * Если in_frame и rx_head <= 512 — сохраняем в rx_active[].
         * Иначе — дропаем (фантомы, мусор, переполнение). */
        if (int_st & UART_RXFIFO_FULL_INT_ST) {
            hw->int_clr.val = UART_RXFIFO_FULL_INT_CLR;
            ctx->isr_count++;

            portENTER_CRITICAL_ISR(&s_global_fifo_mux);
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                if (ctx->in_frame && ctx->rx_head <= DMX_CHANNELS) {
                    ctx->rx_active[ctx->rx_head] = hw->fifo.rw_byte;
                    ctx->rx_head++;
                } else {
                    (void)hw->fifo.rw_byte;
                    if (ctx->in_frame) ctx->err_count++;
                }
            }
            portEXIT_CRITICAL_ISR(&s_global_fifo_mux);
        }

        /* === BREAK_DET: обнаружен break (≥88мкс LOW на RXD) ===
         * Break означает начало нового DMX-кадра.
         * Тайминг: BREAK_DET срабатывает через ~44мкс (11 бит × 4мкс)
         * от начала break. Реальный break в DMX512: 88-176мкс.
         *
         * При BREAK_DET:
         *   1. Debounce — пропускаем если break пришёл <1мс назад
         *   2. Дочитываем остаток FIFO (последние байты ниже порога)
         *   3. Сохраняем rx_active → rx_done (double buffer swap)
         *   4. Сбрасываем rx_head=0 для нового кадра
         *   5. Уведомляем задачу о готовом кадре */
        if (int_st & UART_BRK_DET_INT_ST) {
            hw->int_clr.val = UART_BRK_DET_INT_CLR;
            ctx->break_count++;

            uint32_t now = esp_cpu_get_cycle_count();

            /* Debounce: BREAK не может приходить чаще чем раз в 1мс
             * CPUfreq 80MHz → 1ms = 80000 cycles. Используем 160000 для запаса.
             * Без debounce: ложные break от шума на линии. */
            if (ctx->last_break_cyc != 0 &&
                (now - ctx->last_break_cyc) < 160000) {
                portENTER_CRITICAL_ISR(&s_global_fifo_mux);
                while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                    (void)hw->fifo.rw_byte;
                }
                portEXIT_CRITICAL_ISR(&s_global_fifo_mux);
                continue;
            }
            ctx->last_break_cyc = now;

            /* Дочитать остаток FIFO побайтно через rxfifo_cnt.
             * Последние байты кадра (ниже порога FIFO_FULL=32) сидят в FIFO
             * на момент BREAK_DET. Нужно забрать их в rx_active[]. */
            portENTER_CRITICAL_ISR(&s_global_fifo_mux);
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0 &&
                   ctx->rx_head <= DMX_CHANNELS) {
                ctx->rx_active[ctx->rx_head] = hw->fifo.rw_byte;
                ctx->rx_head++;
            }
            /* Дропаем мусор если остался (фантомы после последнего байта) */
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                (void)hw->fifo.rw_byte;
            }
            portEXIT_CRITICAL_ISR(&s_global_fifo_mux);

            /* Double-buffer swap: копируем rx_active → rx_done.
             * memcpy 513 байт ≈ 2мкс — быстро, безопасно.
             * Уведомляем задачу FreeRTOS notification (без очереди, 0 RAM). */
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

            /* Сброс для нового кадра */
            ctx->rx_head = 0;
            ctx->in_frame = true;
        }

        /* === FRM_ERR: ошибка кадра (стоп-бит != 1) ===
         * Нормально во время break — LOW на линии = "фантомный" старт-бит,
         * стоп-бит тоже LOW → FRM_ERR. Просто очищаем флаг. */
        if (int_st & UART_FRM_ERR_INT_ST) {
            hw->int_clr.val = UART_FRM_ERR_INT_CLR;
        }

        /* === RXFIFO_OVF: переполнение FIFO ===
         * Если ISR не успел прочитать FIFO до переполнения — дропаем
         * все байты и считаем ошибку. На практике не происходит при
         * пороге 32 и быстром ISR (~1мкс на чтение). */
        if (int_st & UART_RXFIFO_OVF_INT_ST) {
            hw->int_clr.val = UART_RXFIFO_OVF_INT_CLR;
            ctx->err_count++;

            portENTER_CRITICAL_ISR(&s_global_fifo_mux);
            while (HAL_FORCE_READ_U32_REG_FIELD(hw->status, rxfifo_cnt) > 0) {
                (void)hw->fifo.rw_byte;
            }
            portEXIT_CRITICAL_ISR(&s_global_fifo_mux);
        }
    }
}

#endif /* DMX_SW_UART_MODE == 2 */

/* ======================================================================
 * ОБЩИЙ API: init / start / get_frame / set_tx_mode
 * ======================================================================
 * uart_bypass_init()      — инициализация состояния порта (без аппаратуры)
 * uart_bypass_init_all()  — инициализация GPIO + RMT (зависит от Mode)
 * uart_bypass_start_timer() — запуск UART hardware (настройка регистров + ISR)
 * uart_bypass_get_frame() — получение готового кадра из double buffer
 * uart_bypass_set_tx_mode() — переключение RX→TX (отключение прерываний)
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
#if DMX_RX_ROUTING_MATRIX
        ctx->gpio_rx = DMX_GPIO_RX1;  /* матрица: оба порта на одном пине */
#else
        ctx->gpio_rx = DMX_GPIO_RX2;
#endif
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
    ctx->bit_pos = 0;
    ctx->byte_val = 0;
    ctx->byte_count = 0;
#endif

#if DMX_SW_UART_MODE == 2
    ctx->in_frame = false;
    ctx->last_break_cyc = 0;
    ctx->intr_handle = NULL;
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
    /* RMT RX для портов */
    int rx_port_count = DMX_RX_ROUTING_MATRIX ? 1 : 2;
    for (int port = 0; port < rx_port_count; port++) {
        sw_uart_ctx_t *ctx = &s_ctx[port];

        /* Очередь RMT callback → task */
        ctx->rmt_rx_queue = xQueueCreate(8, sizeof(rmt_rx_msg_t));
        assert(ctx->rmt_rx_queue);

        /* RMT RX channel — matrix: весь pool одному RX (512-LED64=448)
         * normal: каждый RX по 64 (LED64+RX64+RX64=192 из 512) */
        int buf_sym = DMX_RX_ROUTING_MATRIX
                    ? (512 - 64)   /* весь pool минус LED */
                    : RMT_RX_BUF_SYMBOLS;
        rmt_rx_channel_config_t rx_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = DMX_RMT_CLK_HZ,
            .mem_block_symbols = buf_sym,
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
#if DMX_RX_ROUTING_MATRIX
        if (port == 1) {
            ESP_LOGI(TAG, "  [MATRIX] port1 routed to GPIO%d (same as port0)", DMX_GPIO_RX1);
        }
#endif
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
    ESP_LOGI(TAG, "Free heap before UART init: %lu bytes", (unsigned long)esp_get_free_heap_size());

    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

#if DMX_SW_UART_MODE == 2
    /* =================================================================
     * Mode 2: HW UART RX — полный bypass ESP-IDF UART driver.
     * uart_param_config настраивает регистры (без ISR).
     * uart_set_pin подключает GPIO к UART (без ISR).
     * esp_intr_alloc ставит НАШ ISR вместо ISR драйвера.
     * uart_driver_install НЕ вызывается!
     * ================================================================= */

    /* UART1: TX=GPIO2, RX=GPIO15 */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, DMX_GPIO_TX1, DMX_GPIO_RX1,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

#if DMX_RX_ROUTING_MATRIX
    /* Routing matrix: UART2 тоже слушает GPIO15 (как UART1) */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, DMX_GPIO_TX2, DMX_GPIO_RX1,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "[MATRIX] UART2 RX routed to GPIO%d (same as UART1)", DMX_GPIO_RX1);
#else
    /* Normal: UART2 RX=GPIO16 */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, DMX_GPIO_TX2, DMX_GPIO_RX2,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#endif

    /* RX FIFO threshold: прерывание при 32 байтах — быстрое чтение,
     * данные не застряют в FIFO к моменту BREAK */
    uart_ll_set_rxfifo_full_thr(&UART1, 32);
    uart_ll_set_rxfifo_full_thr(&UART2, 32);

    /* Очистить FIFO */
    uart_ll_rxfifo_rst(&UART1);
    uart_ll_rxfifo_rst(&UART2);

    /* Установить наши ISR — заменяют ISR ESP-IDF драйвера */
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_UART1_INTR_SOURCE,
                                   ESP_INTR_FLAG_IRAM,
                                   uart_rx_isr, (void *)0,
                                   &s_ctx[0].intr_handle));
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_UART2_INTR_SOURCE,
                                   ESP_INTR_FLAG_IRAM,
                                   uart_rx_isr, (void *)1,
                                   &s_ctx[1].intr_handle));

    /* Включить прерывания: BREAK + FRM_ERR + FIFO_FULL + FIFO_OVF */
    uart_ll_ena_intr_mask(&UART1, UART_HW_RX_ISR_FLAGS);
    uart_ll_ena_intr_mask(&UART2, UART_HW_RX_ISR_FLAGS);

    ESP_LOGI(TAG, "HW UART RX mode2: UART1(TX=GPIO%d,RX=GPIO%d) UART2(TX=GPIO%d,RX=GPIO%d)",
             DMX_GPIO_TX1, DMX_GPIO_RX1, DMX_GPIO_TX2, DMX_GPIO_RX2);

#else
    /* Mode 0/1: UART driver только для TX (test/patcher mode) */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, DMX_GPIO_TX1, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 256, 0, NULL, 0));

    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, DMX_GPIO_TX2, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 256, 256, 0, NULL, 0));

    ESP_LOGI(TAG, "TX UARTs on UART1=GPIO%d, UART2=GPIO%d",
             DMX_GPIO_TX1, DMX_GPIO_TX2);
#endif
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

#if DMX_SW_UART_MODE == 2
    uart_dev_t *hw = (port == 0) ? &UART1 : &UART2;
    if (tx_mode) {
        /* Отключить RX прерывания перед TX */
        uart_ll_disable_intr_mask(hw, UART_HW_RX_ISR_FLAGS);
    } else {
        /* Очистить FIFO и состояние, включить RX прерывания */
        portENTER_CRITICAL_ISR(&s_global_fifo_mux);
        uart_ll_rxfifo_rst(hw);
        portEXIT_CRITICAL_ISR(&s_global_fifo_mux);
        ctx->rx_head = 0;
        ctx->in_frame = false;
        ctx->frame_ready = false;
        ctx->last_break_cyc = 0;
        uart_ll_ena_intr_mask(hw, UART_HW_RX_ISR_FLAGS);
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
