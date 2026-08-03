/**
 * @file dmx.c
 * @brief Реализация DMX512 UART HAL — приём, передача, переключение режимов
 *
 * Данный модуль реализует чистый HAL (Hardware Abstraction Layer) для протокола DMX512
 * на базе UART. Поддерживает два порта с независимым приёмом и передачей.
 *
 * Архитектура модуля:
 *   - UART RX ISR (Interrupt Service Routine) — обработка прерываний от UART
 *   - Задачи приема (dmx_rx_task) — обработка полученных кадров
 *   - Задача передачи (dmx_tx_task) — генерация DMX данных
 *   - Публичный API — интерфейс для других модулей
 *
 * Параметры протокола DMX512:
 *   - Скорость передачи: 250 кбит/с (250000 бод)
 *   - Формат данных: 8N2 (8 данных, без проверки чётности, 2 стоп-бита)
 *   - Количество каналов: 512 (нумерация с 1 до 512)
 *   - Длина кадра: 513 байт (1 старт-код + 512 каналов данных)
 *
 * Состояния UART RX ISR (конечный автомат обнаружения кадров):
 *   1. Ожидание break-сигнала (начало нового кадра)
 *   2. Обработка break — сброс состояния, подготовка к приему
 *   3. Прием данных — заполнение буфера до следующего break
 *   4. Завершение кадра — копирование данных, уведомление задачи
 *
 * @note Модуль не содержит логики управления LED, fallback-механизмов или эффектов.
 *       Это чистый UART HAL для работы с DMX512.
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

/** @brief Тег для логирования ESP-IDF */
static const char *TAG = "DMX";

/* ===== UART константы ===== */

/** @brief Скорость передачи UART для DMX512 (250000 бод) */
#define UART_BAUD_RATE       250000

/**
 * @brief Количество циклов для подавления дребезга break-сигнала
 * @details Используется для предотвращения ложных срабатываний при коротких break.
 *          Значение 160000 циклов соответствует примерно 1-2 мс при частоте CPU 240 МГц.
 */
#define BREAK_DEBOUNCE_CYCLES 160000

/**
 * @brief Маска флагов прерываний для RX UART
 * @details Включает прерывания:
 *   - UART_BRK_DET_INT_ENA — обнаружение break-сигнала
 *   - UART_FRM_ERR_INT_ENA — ошибка формата кадра
 *   - UART_RXFIFO_FULL_INT_ENA — заполнение RX FIFO
 *   - UART_RXFIFO_OVF_INT_ENA — переполнение RX FIFO
 */
#define RX_ISR_FLAGS (UART_BRK_DET_INT_ENA | UART_FRM_ERR_INT_ENA | \
                      UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_OVF_INT_ENA)

/**
 * @brief Массив указателей на регистры UART для каждого порта
 * @details Используется для прямого доступа к регистрам UART1 и UART2.
 *          Помещен в IRAM для быстрого доступа из ISR.
 */
static IRAM_ATTR uart_dev_t *uart_hw[] = { &UART1, &UART2 };

/* ===== RX context ===== */

/**
 * @brief Контекст приема DMX для одного порта
 * @details Содержит все необходимые данные для обработки DMX кадров,
 *          включая буферы данных, состояние конечного автомата и ресурсы FreeRTOS.
 */
typedef struct {
    uint8_t           rx_active[DMX_FRAME_LEN];  /**< Активный буфер приема — заполняется ISR во время получения кадра */
    uint8_t           rx_done[DMX_FRAME_LEN];    /**< Буфер завершенного кадра — копируется из rx_active после получения */
    volatile uint16_t frame_len;                  /**< Текущая длина принимаемого кадра (в байтах) */
    volatile uint16_t frame_len_saved;            /**< Длина последнего завершенного кадра */
    volatile bool     enabled;                    /**< Флаг включения приема на данном порту */
    volatile bool     sync;                       /**< Флаг синхронизации — true после первого break, когда данные могут быть скопированы */
    bool              in_frame;                   /**< Флаг нахождения в процессе приема кадра (после break) */
    uint32_t          last_break_cyc;             /**< Временная метка последнего break в циклах CPU (для подавления дребезга) */
    intr_handle_t     intr_handle;                /**< Дескриптор прерывания UART */
    TaskHandle_t      notify_task;                /**< Дескриптор задачи для уведомления о завершении кадра */
    int               uart_num;                   /**< Номер UART (UART_NUM_1 или UART_NUM_2) */
    int               rx_pin;                     /**< GPIO пин приемника (UART RX) */
    int               tx_pin;                     /**< GPIO пин передатчика (UART TX) */
    int               dir_pin;                    /**< GPIO пин управления направлением (RS485 DE/RE), -1 если не используется */
    int               fifo_thr;                   /**< Порог заполнения FIFO для генерации прерывания */
    portMUX_TYPE      mux;                        /**< Мьютекс для безопасного доступа к буферам из ISR и задач */
} dmx_rx_ctx_t;

/** @brief Массив контекстов приема для двух портов */
static dmx_rx_ctx_t s_ctx[2];

/* ===== RX: ISR ===== */

/**
 * @brief ISR обработки прерываний UART для приема DMX
 * @param arg Порт (0 или 1), переданный как void*
 * @details Конечный автомат обнаружения DMX кадров:
 *
 * 1. UART_RXFIFO_FULL_INT_ST — заполнение RX FIFO:
 *    - Читает все байты из FIFO
 *    - Если в процессе приема кадра (in_frame) и длина не превышает DMX_FRAME_LEN,
 *      сохраняет байты в rx_active
 *    - Иначе отбрасывает байты
 *
 * 2. UART_BRK_DET_INT_ST — обнаружение break-сигнала (начало нового кадра):
 *    - Проверка дребезга: если с момента последнего break прошло менее BREAK_DEBOUNCE_CYCLES,
 *      break отбрасывается
 *    - Если был предыдущий кадр (in_frame и frame_len > 0):
 *      * Первый break: устанавливает sync = true (начало синхронизации)
 *      * Второй и последующие break: копирует данные в rx_done, уведомляет задачу
 *    - Сбрасывает состояние для нового кадра
 *
 * 3. UART_FRM_ERR_INT_ST — ошибка формата кадра:
 *    - Просто очищает флаг ошибки
 *
 * 4. UART_RXFIFO_OVF_INT_ST — переполнение RX FIFO:
 *    - Сбрасывает состояние приема (in_frame = false, frame_len = 0)
 *    - Очищает FIFO
 *
 * @note Функция размещена в IRAM для быстрого доступа из ISR.
 */
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

/** @brief Указатель на callback-функцию обработки полученных кадров */
static dmx_frame_cb_t s_frame_callback = NULL;

/**
 * @brief Задача обработки полученных DMX кадров
 * @param arg Порт (0 или 1), переданный как void*
 * @details Задача работает в бесконечном цикле, ожидая уведомления от ISR.
 *          При получении уведомления:
 *    1. Проверяет, включен ли прием на данном порту
 *    2. Копирует данные из rx_done в локальный буфер (с защитой от прерываний)
 *    3. Сохраняет данные в g_raw_frames[port] с временной меткой
 *    4. Если кадр достаточно длинный (>= 100 байт), вычисляет контрольные суммы
 *       и сохраняет их в g_ck_rings[port]
 *    5. Устанавливает соответствующий бит в g_dmx_events для уведомления других задач
 *    6. Вызывает registered callback-функцию, если она зарегистрирована
 *
 * @note Задача создается с приоритетом 4 и привязана к ядру CPU, соответствующему порту.
 */
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

/**
 * @brief Включение/отключение приема DMX на указанном порту
 * @param port Номер порта (0 или 1)
 * @param enable true — включить прием, false — отключить
 * @details При включении:
 *    - Сбрасывает RX FIFO
 *    - Сбрасывает состояние конечного автомата
 *    - Включает прерывания UART
 *
 *    При отключении:
 *    - Отключает прерывания UART
 *    - Сбрасывает RX FIFO
 *
 * @note Функция используется при переключении режимов работы.
 */
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

/**
 * @brief Инициализация контекста приема для одного порта
 * @param port Номер порта (0 или 1)
 * @param rx_pin GPIO пин приемника (UART RX)
 * @param tx_pin GPIO пин передатчика (UART TX)
 * @param dir_pin GPIO пин управления направлением (RS485 DE/RE), -1 если не используется
 * @param uart_num Номер UART (UART_NUM_1 или UART_NUM_2)
 * @param fifo_thr Порог заполнения FIFO для генерации прерывания
 * @details Заполняет структуру dmx_rx_ctx_t начальными значениями.
 *          Не настраивает UART — это делается в dmx_rx_start().
 */
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

/**
 * @brief Запуск приема DMX на обоих портах
 * @details Выполняет полную настройку UART для обоих портов:
 *    1. Создает группу событий g_dmx_events
 *    2. Настраивает UART с параметрами DMX512 (250000, 8N2)
 *    3. Устанавливает GPIO пины для RX и TX
 *    4. Настраивает порог FIFO и сбрасывает буфер
 *    5. Регистрирует ISR обработчик прерываний
 *    6. Включает прерывания UART
 *    7. Настраивает GPIO пин управления направлением (если указан)
 *    8. Создает задачу приема для каждого порта
 *
 * @note Функция вызывается из dmx_init() после инициализации контекстов.
 */
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

/**
 * @brief Отправка DMX кадра через UART (HAL-уровень)
 * @param port Номер порта (0 или 1)
 * @param data Указатель на данные для отправки (включая старт-код)
 * @param len Длина данных для отправки в байтах
 * @return true всегда (для совместимости с интерфейсом)
 * @details Реализует полный цикл передачи DMX кадра:
 *    1. Генерация break-сигнала:
 *       - Устанавливает txd_inv = 1 (инверсия TX) на 176 мкс (минимум 88 мкс для DMX)
 *       - Устанавливает txd_inv = 0 (возврат к норме) на 16 мкс (マーキング time)
 *    2. Заполнение TX FIFO:
 *       - Читает количество свободных байтов в FIFO (максимум 128)
 *       - Заполняет FIFO порциями по.available bytes
 *       - Ждет 10 мкс между порциями, если FIFO заполнено
 *    3. Ожидание завершения передачи:
 *       - Ждет установки флага UART_TX_DONE_INT_ST
 *       - Очищает флаг
 *
 * @note Функция блокирующая — ждет завершения передачи всех данных.
 * @note Break-сигнал генерируется путем инверсии линии TX (txd_inv = 1).
 */
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

/**
 * @brief Задача передачи DMX данных
 * @param arg Не используется (NULL)
 * @details Задача работает в бесконечном цикле, обрабатывая три режима:
 *
 * 1. DMX_MODE_TESTER (режим тестера):
 *    - Формирует DMX кадр с указанными параметрами (порт, канал, RGB, режим)
 *    - В TX_MODE_POINT: заполняет одну тройку каналов (R, G, B) на указанном канале
 *    - В TX_MODE_FILL: заполняет несколько тройек каналов подряд (от 1 до tx_count)
 *    - Отправляет кадр на указанный порт (или оба, если tx_port >= DMX_PORT_COUNT)
 *    - Обеспечивает частоту 30 Гц (33.333 мкс между кадрами)
 *
 * 2. DMX_MODE_PATCH (режим проходного):
 *    - Отправляет пустой кадр (все нули) на указанный порт
 *    - Данные для передачи заполняются извне через dmx_write()
 *    - Обеспечивает частоту 30 Гц
 *
 * 3. DMX_MODE_SNIFFER (режим подслушивания) или другой:
 *    - Задача просто ждет 100 мс (не потребляя ресурсы CPU)
 *
 * @note Задача создается с приоритетом 5 и привязана к ядру CPU 1.
 * @note При переключении режимов добавляется задержка 100 мс для стабилизации.
 */
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

/**
 * @brief Глобальное состояние DMX модуля
 * @details Инициализируется начальными значениями:
 *   - mode: DMX_MODE_SNIFFER (режим подслушивания)
 *   - tx_port: 0 (первый порт)
 *   - tx_channel: 1 (начальный канал)
 *   - tx_r, tx_g, tx_b: 255 (белый цвет)
 *   - tx_mode: TX_MODE_POINT (точечный режим)
 *   - tx_count: 3 (количество тройки каналов для заполнения)
 *   - last_rx_ms: {0, 0} (нет приема)
 *   - patch_cursor: 0 (начало вселенной)
 *   - patch_universe: 1 (первая вселенная)
 */
dmx_state_t g_dmx = {
    .mode                = DMX_MODE_SNIFFER,
    .tx_port             = 0,
    .tx_channel          = 1,
    .tx_r                = 255, .tx_g = 255, .tx_b = 255,
    .tx_mode             = TX_MODE_POINT,
    .tx_count            = 3,
    .last_rx_ms          = { 0, 0 },
    .patch_cursor        = 0,
    .patch_universe      = 1,
};

/**
 * @brief Группа событий для уведомления о получении DMX кадров
 * @details Создается в dmx_rx_start(). Используется FreeRTOS EventGroup.
 */
EventGroupHandle_t g_dmx_events;

/**
 * @brief Массив последних полученных DMX кадров
 * @details Индекс 0 — порт 1, индекс 1 — порт 2.
 *          Заполняется в dmx_rx_task().
 */
dmx_raw_frame_t    g_raw_frames[2];

/**
 * @brief Массив кольцевых буферов контрольных сумм
 * @details Индекс 0 — порт 1, индекс 1 — порт 2.
 *          Заполняется в dmx_rx_task() для кадров длиной >= 100 байт.
 */
dmx_ck_ring_t      g_ck_rings[2];

/**
 * @brief Счетчик прерываний для каждого порта
 * @details Увеличивается в uart_rx_isr() при каждом срабатывании прерывания.
 */
volatile uint32_t  g_isr_count[2]   = {0, 0};

/**
 * @brief Счетчик обнаруженных break-сигналов для каждого порта
 * @details Увеличивается в uart_rx_isr() при обнаружении break (начало нового кадра).
 */
volatile uint32_t  g_break_count[2] = {0, 0};

/** @brief Мьютекс для доступа к глобальным данным DMX */
static SemaphoreHandle_t g_dmx_mutex = NULL;

/* ===== Lock / Unlock ===== */

/**
 * @brief Захват мьютекса для доступа к глобальным данным DMX
 * @details Блокирует доступ к g_dmx и другим общим данным.
 *          Если мьютекс не создан, функция ничего не делает.
 */
void dmx_lock(void) {
    if (g_dmx_mutex) xSemaphoreTake(g_dmx_mutex, portMAX_DELAY);
}

/**
 * @brief Освобождение мьютекса для доступа к глобальным данным DMX
 * @details Разблокирует доступ к g_dmx и другим общим данным.
 *          Если мьютекс не создан, функция ничего не делает.
 */
void dmx_unlock(void) {
    if (g_dmx_mutex) xSemaphoreGive(g_dmx_mutex);
}

/* ===== Публичный API ===== */

/**
 * @brief Чтение данных из последнего полученного DMX кадра
 * @param port Номер порта (0 или 1)
 * @param buf Указатель на буфер для чтения данных
 * @param len Максимальное количество байт для чтения
 * @details Читает данные каналов DMX из g_raw_frames[port].data.
 *          Данные копируются с защитой от конкурентного доступа через мьютекс.
 *          Если порт недействителен, функция ничего не делает.
 */
void dmx_read(int port, uint8_t *buf, int len) {
    if (port < 0 || port >= DMX_PORT_COUNT) return;
    int n = len < DMX_CHANNELS ? len : DMX_CHANNELS;
    dmx_lock();
    memcpy(buf, g_raw_frames[port].data, n);
    dmx_unlock();
}

/**
 * @brief Отправка DMX кадра на указанный порт
 * @param port Номер порта (0 или 1)
 * @param frame Указатель на массив данных для отправки (включая старт-код)
 * @param len Длина данных для отправки в байтах
 * @details Отправляет данные через UART с генерацией break-сигнала.
 *          Функция блокирующая — ждет завершения передачи.
 *          Если порт недействителен, функция ничего не делает.
 */
void dmx_write(int port, const uint8_t *frame, int len) {
    if (port < 0 || port >= DMX_PORT_COUNT) return;
    dmx_hal_send(port, frame, len);
}

/**
 * @brief Регистрация callback-функции для обработки полученных DMX кадров
 * @param cb Указатель на callback-функцию или NULL для отмены регистрации
 * @details Функция вызывается из контекста задачи dmx_rx_task после обработки кадра.
 *          Регистрация нового callback заменяет предыдущий.
 */
void dmx_on_frame(dmx_frame_cb_t cb) {
    s_frame_callback = cb;
}

/**
 * @brief Установка режима работы DMX модуля
 * @param m Режим работы (DMX_MODE_SNIFFER, DMX_MODE_TESTER, DMX_MODE_PATCH)
 * @details Переключает режим работы модуля:
 *    - При переключении в TESTER или PATCH:
 *      * Отключается прием на обоих портах
 *      * Устанавливается направление передачи (DIR = 1)
 *    - При переключении в SNIFFER:
 *      * Включается прием на обоих портах
 *      * Устанавливается направление приема (DIR = 0)
 *    - Изменение режима логируется через ESP_LOGI
 *
 * @note Изменение режима требует времени для стабилизации UART.
 */
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

/**
 * @brief Инициализация DMX модуля
 * @param tx1 GPIO пин передатчика первого порта (UART1 TX)
 * @param tx2 GPIO пин передатчика второго порта (UART2 TX)
 * @param rx1 GPIO пин приемника первого порта (UART1 RX)
 * @param rx2 GPIO пин приемника второго порта (UART2 RX)
 * @param dir GPIO пин управления направлением (RS485 DE/RE), -1 если не используется
 * @details Выполняет полную инициализацию модуля:
 *    1. Создает мьютекс g_dmx_mutex
 *    2. Инициализирует контексты приема для обоих портов
 *    3. Запускает прием на обоих портах (UART, ISR, задачи)
 *    4. Создает задачу передачи
 *
 * @note Порт 1 (UART1) всегда инициализируется, порт 2 (UART2) — опционально.
 * @note Задача передачи создается с приоритетом 5 на ядре CPU 1.
 */
void dmx_init(int tx1, int tx2, int rx1, int rx2, int dir) {
    g_dmx_mutex = xSemaphoreCreateMutex();

    dmx_rx_init_port(0, rx1, tx1, -1,  UART_NUM_1, 8);
    dmx_rx_init_port(1, rx2, tx2, dir, UART_NUM_2, 16);
    dmx_rx_start();

    xTaskCreatePinnedToCore(dmx_tx_task, "dmx_tx", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Init: TX1=GPIO%d TX2=GPIO%d RX1=GPIO%d RX2=GPIO%d DIR=GPIO%d",
             tx1, tx2, rx1, rx2, dir);
}
