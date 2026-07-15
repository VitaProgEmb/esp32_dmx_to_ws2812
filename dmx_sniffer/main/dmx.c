/**
 * @file dmx.c
 * @brief Ядро DMX-подсистемы: приём, передача, маршрутизация на LED-ленту
 *
 * ========================================================================
 * ОБЩАЯ АРХИТЕКТУРА
 * ========================================================================
 *
 * Модуль реализует три режима работы устройства:
 *
 *   1. DMX_MODE_SNIFFER — приём DMX512 с двух портов, декодирование каналов,
 *      сопоставление с LED-лентой через таблицу патчей, вывод на WS2812
 *
 *   2. DMX_MODE_TESTER — генерация DMX-кадров (точечный тест / заполнение)
 *      для проверки DMX-устройств
 *
 *   3. DMX_MODE_PATCH — режим настройки патча: подсветка приборов по таблице
 *      (курсор = белый, skip = красный, активный = зелёный)
 *
 * ========================================================================
 * ЗАДАЧИ FREERTOS
 * ========================================================================
 *
 *   dmx_rx0 / dmx_rx1 — приём DMX-кадров с порта 0 и порта 1 (ядро 1, приоритет 4)
 *     Блокируются на dmx_receive() пока не придёт кадр.
 *     Вызывают process_dmx_frame() для обработки.
 *
 *   dmx_tx — передача DMX-кадров в режиме TESTER/PATCH (ядро 1, приоритет 5)
 *     Генерирует кадры с интервалом 33.3 мс (30 FPS, стандарт DMX).
 *     В режиме SNIFFER просто ждёт.
 *
 *   led_ref — обновление LED-ленты по событию (ядро 1, приоритет 3)
 *     Ждёт семафор от DMX-задач или флаг fallback.
 *     Вызывает led_strip_refresh() для кодирования и отправки в RMT.
 *
 * ========================================================================
 * FALLBACK-МЕХАНИЗМ
 * ========================================================================
 *
 * При потере DMX-сигнала (таймаут fallback_timeout_ms) лента заливается
 * заранее настроенным цветом (fallback_r/g/b). Реализация:
 *
 *   - esp_timer (one-shot) запускается при каждом принятом DMX-кадре
 *   - Если кадры перестают приходить, таймер срабатывает
 *   - Callback записывает fallback-цвет в буфер и ставит флаг s_fallback_pending
 *   - Задача led_ref видит флаг и вызывает led_strip_refresh()
 *   - При получении нового DMX-кадра флаг сбрасывается
 *
 * ========================================================================
 * ИНТЕРПОЛЯЦИЯ (СГЛАЖИВАНИЕ)
 * ========================================================================
 *
 * Если interpolate = true, цвета между приборами сглаживаются:
 *   - Каждый пиксель ленты "располагается" между двумя ближайшими приборами
 *   - Цвет вычисляется как линейная интерполяция (lerp) с весом weight
 *   - Это создаёт плавный градиент вместо ступенчатого переключения
 *
 * Если interpolate = false, каждый прибор занимает равный диапазон пикселей,
 * и цвета просто копируются без смешивания.
 */

#include "dmx.h"
#include "dmx_hal.h"
#include "settings.h"
#include "led_strip.h"
#include "patch_manager.h"
#include "web_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "uart_bypass.h"
#include <string.h>

static const char *DMX_TAG = "DMX";

/* ======================================================================
 * ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
 * ====================================================================== */

/**
 * Глобальное состояние DMX-подсистемы.
 * Доступ из разных задач защищён через g_dmx_mutex (dmx_lock/dmx_unlock).
 */
dmx_state_t g_dmx = {
    .mode                = DMX_MODE_SNIFFER,
    .tx_port             = 0,
    .tx_channel          = 1,
    .tx_r                = 255,
    .tx_g                = 255,
    .tx_b                = 255,
    .tx_mode             = TX_MODE_POINT,
    .tx_count            = 3,
    .channel_order       = CH_ORDER_RGB,
    .led_shift           = 0,
    .fallback_r          = 0,
    .fallback_g          = 0,
    .fallback_b          = 255,
    .fallback_timeout_ms = 100,
    .last_rx_ms          = { 0, 0 },
    .interpolate         = true,
    .patch_cursor        = 0,
    .patch_universe      = 1,
};

/** Мьютекс для доступа к g_dmx и共享-данным */
static SemaphoreHandle_t g_dmx_mutex = NULL;

/** Буферы принятых DMX-каналов для каждого порта (для веб-интерфейса) */
static uint8_t s_rx_buf[DMX_PORT_COUNT][DMX_CHANNELS];

/** Счётчики принятых кадров для отладки */
volatile uint32_t s_rx_frame_count[DMX_PORT_COUNT] = {0, 0};
volatile uint32_t s_rx_last_frame_size[DMX_PORT_COUNT] = {0, 0};

/* ======================================================================
 * ТАБЛИЦЫ ПОИСКА (Lookup Tables)
 * ======================================================================
 * Предвычисленные таблицы для быстрого сопоставления пикселей ленты
 * с приборами из таблицы патчей. Пересчитываются при изменении
 * количества LED или количества приборов (dmx_recompute_lookups).
 * ====================================================================== */

/** Начальный индекс пикселя для каждого прибора */
static uint16_t stream_ws_start[PATCH_MAX_ENTRIES];

/** Количество пикселей для каждого прибора */
static uint16_t stream_ws_count[PATCH_MAX_ENTRIES];

/** Цвета приборов из последнего DMX-кадра (R, G, B) */
static uint8_t  stream_fixture_colors[PATCH_MAX_ENTRIES][3];

/**
 * Таблица соответствия порядка каналов (RGB/GRB/...) → индексы.
 * s_order_map[CH_ORDER_RGB][0] = 0 (R — первый байт),
 * s_order_map[CH_ORDER_GRB][0] = 1 (G — первый байт в GRB).
 */
static const uint8_t s_order_map[CH_ORDER_COUNT][3] = {
    [CH_ORDER_RGB] = {0, 1, 2},
    [CH_ORDER_RBG] = {0, 2, 1},
    [CH_ORDER_GRB] = {1, 0, 2},
    [CH_ORDER_GBR] = {1, 2, 0},
    [CH_ORDER_BRG] = {2, 0, 1},
    [CH_ORDER_BGR] = {2, 1, 0},
};

/* ======================================================================
 * FALLBACK-ТАЙМЕР
 * ====================================================================== */

/** One-shot таймер для fallback при потере DMX */
static esp_timer_handle_t s_fallback_timer = NULL;

/** Семафор для сигнала задаче led_ref (нормальный путь обновления LED) */
static SemaphoreHandle_t s_led_refresh_sem = NULL;

/**
 * Флаг "fallback в процессе".
 * Ставится в true когда таймер fallback сработал и цвет записан в буфер.
 * Сбрасывается в false когда приходит новый DMX-кадр.
 *
 * Задача led_ref проверяет этот флаг каждые 50мс и вызывает
 * led_strip_refresh() если флаг установлен.
 */
static volatile bool s_fallback_pending = false;

/**
 * @brief Задача обновления LED-ленты
 *
 * Ждёт семафор (от DMX-задач при приёме кадра) ИЛИ проверяет флаг
 * fallback каждые 50мс. Вызывает led_strip_refresh() при любом событии.
 *
 * Защита от зависания: если led_strip_refresh() заблокирован в RMT,
 * задача не может обработать новые события. Таймаут 50мс на семафоре
 * обеспечивает периодический "выход" для проверки флага fallback.
 */
static void led_refresh_task(void *arg) {
    while (1) {
        xSemaphoreTake(s_led_refresh_sem, pdMS_TO_TICKS(5));
        if (s_fallback_pending) s_fallback_pending = false;
        led_strip_refresh();
    }
}

/* ======================================================================
 * БАЗОВЫЕ ФУНКЦИИ
 * ====================================================================== */

/** Захватить мьютекс DMX-данных */
void dmx_lock(void) {
    if (g_dmx_mutex) xSemaphoreTake(g_dmx_mutex, portMAX_DELAY);
}

/** Освободить мьютекс DMX-данных */
void dmx_unlock(void) {
    if (g_dmx_mutex) xSemaphoreGive(g_dmx_mutex);
}

/**
 * @brief Копировать принятые DMX-каналы в пользовательский буфер
 *
 * Используется веб-сервером для отображения значений каналов в реальном времени
 * (панель "Инфо" → DMX-бары и кружки).
 *
 * @param port     Номер DMX-порта (0 или 1)
 * @param out      Выходной буфер (минимум max_count байт)
 * @param max_count Максимальное количество каналов для копирования
 */
void dmx_get_channel_data(int port, uint8_t *out, int max_count) {
    if (port < 0 || port >= DMX_PORT_COUNT) return;
    int n = max_count < DMX_CHANNELS ? max_count : DMX_CHANNELS;
    dmx_lock();
    memcpy(out, s_rx_buf[port], n);
    dmx_unlock();
}

/** Вернуть указатель на цвета приборов (для отладки превью) */
const uint8_t (*dmx_get_fixture_colors(void))[3] {
    return stream_fixture_colors;
}

/**
 * @brief Пересчитать таблицы поиска (stream_ws_start/count)
 *
 * Вызывается при изменении количества LED или таблицы патчей.
 * Равномерно распределяет пиксели ленты между приборами:
 *   Прибор 0 → пиксели [0 .. start[1])
 *   Прибор 1 → пиксели [start[1] .. start[2])
 *   ...
 *   Последний прибор → пиксели [start[N] .. count]
 */
void dmx_recompute_lookups(void) {
    dmx_lock();
    uint16_t n = g_patch.count;
    uint16_t m = g_total_leds;
    if (n > 0) {
        /* Вычисляем начальный пиксель для каждого прибора */
        for (uint16_t f = 0; f < n; f++) {
            stream_ws_start[f] = (uint32_t)f * m / n;
        }
        /* Вычисляем количество пикселей = разница между соседними start */
        for (uint16_t f = 0; f < n; f++) {
            uint16_t next = (f + 1 < n) ? stream_ws_start[f + 1] : m;
            stream_ws_count[f] = next - stream_ws_start[f];
        }
    }
    dmx_unlock();
}

/* ======================================================================
 * ОБРАБОТКА DMX-КАДРА (СНИФФЕР)
 * ====================================================================== */

/**
 * @brief Обработать принятый DMX-кадр и обновить LED-ленту
 *
 * Алгоритм:
 *   1. Проверить режим (только SNIFFER) и тестовый режим
 *   2. Для каждого прибора в таблице патчей:
 *      - Определить порт по universe (1→порт 0, 2→порт 1)
 *      - Пропустить если skip=true или не тот порт
 *      - Прочитать 3 канала (R, G, B) с DMX-адреса прибора
 *      - Применить порядок каналов (RGB/GRB/...)
 *      - Сохранить в stream_fixture_colors[]
 *   3. Записать цвета в back-буфер (с интерполяцией или без)
 *   4. Поменять буферы местами (swap_banks)
 *   5. Сбросить fallback-таймер
 *
 * @param port     Номер DMX-порта (0 или 1)
 * @param slots    Массив значений DMX-каналов (без start code)
 * @param num_slots Количество принятых каналов
 */

void dmx_store_frame(int port, const uint8_t *slots, int num_slots) {
    uint16_t max_slot = (num_slots < DMX_CHANNELS) ? num_slots : DMX_CHANNELS;
    s_rx_frame_count[port]++;
    s_rx_last_frame_size[port] = num_slots;
    dmx_lock();
    memcpy(s_rx_buf[port], slots, max_slot);
    dmx_unlock();
}

static void do_led_processing(int port, const uint8_t *slots, uint16_t max_slot);

static void process_dmx_frame(int port, const uint8_t *slots, int num_slots) {
    /* Копируем сырые DMX-данные в буфер для веб-интерфейса (панель Инфо)
     * Всегда — независимо от режима, чтобы данные отображались всегда */
    uint16_t max_slot = (num_slots < DMX_CHANNELS) ? num_slots : DMX_CHANNELS;
    dmx_store_frame(port, slots, num_slots);
    do_led_processing(port, slots, max_slot);
}

static void do_led_processing(int port, const uint8_t *slots, uint16_t max_slot) {

    dmx_lock();
    dmx_mode_t mode = g_dmx.mode;
    uint8_t cpf = DMX_FIXTURE_CH;           /* Каналов на прибор (3 для RGB) */
    uint8_t ch_order = g_dmx.channel_order;
    uint8_t n_entries = g_patch.count;
    dmx_unlock();

    /* Обрабатываем кадры только в режиме сниффера */
    if (mode != DMX_MODE_SNIFFER) return;

    /* Если активен ручной тест LED — не трогаем ленту */
    if (g_led_test_mode != 0) return;

    /* --- Шаг 1: Извлечь цвета приборов из DMX-каналов --- */
    for (uint8_t f = 0; f < n_entries; f++) {
        dmx_lock();
        uint8_t uni = g_patch.entries[f].universe;
        bool skip = g_patch.entries[f].skip;
        dmx_unlock();

        if (skip) continue;                          /* Прибор помечен как пропущенный */
        int mapped_port = (uni == 2) ? 1 : 0;       /* Universe 2 → порт 1 */
        if (mapped_port != port) continue;           /* Не наш порт — пропускаем */

        uint16_t addr = g_patch.entries[f].dmx_addr;
        if (addr + cpf - 1 > max_slot) continue;    /* Адрес за пределами кадра */

        /* Читаем 3 канала (R, G, B) и применяем порядок каналов.
         * raw[] содержит цвет в "натуральном" порядке прибора,
         * после s_order_map[] получаем порядок для WS2812B (RGB). */
        uint8_t raw[3] = { slots[addr - 1], slots[addr], slots[addr + 1] };
        stream_fixture_colors[f][0] = raw[s_order_map[ch_order][0]];
        stream_fixture_colors[f][1] = raw[s_order_map[ch_order][1]];
        stream_fixture_colors[f][2] = raw[s_order_map[ch_order][2]];
    }

    /* --- Шаг 2: Записать цвета в back-буфер --- */
    led_strip_lock();
    uint16_t num_fixtures = g_patch.count;
    uint16_t num_leds = g_led_strip.count;
    /* Определяем "тихий" буфер (в который пишем, пока RMT читает другой) */
    led_color_t *back = (g_led_strip.colors == g_led_strip.bank_a)
                      ? g_led_strip.bank_b : g_led_strip.bank_a;
    led_strip_unlock();

    dmx_lock();
    bool interp = g_dmx.interpolate;
    dmx_unlock();

    if (interp) {
        /* --- ИНТЕРПОЛЯЦИЯ: плавное сглаживание между приборами --- */
        if (num_fixtures < 2 || num_leds <= 1) {
            /* 0-1 приборов: все пиксели = цвет первого прибора */
            led_color_t color = {0, 0, 0};
            if (num_fixtures > 0) {
                color.r = stream_fixture_colors[0][0];
                color.g = stream_fixture_colors[0][1];
                color.b = stream_fixture_colors[0][2];
            }
            for (uint16_t i = 0; i < num_leds; i++)
                back[i] = color;
        } else {
            /* 2+ приборов: линейная интерполяция между соседними.
             * Каждый пиксель позиционируется на "оси" от 0 до (N-1),
             * где целочисленная часть = индекс левого прибора,
             * дробная = вес для линейной комбинации с правым прибором. */
            for (uint16_t i = 0; i < num_leds; i++) {
                uint32_t pos_fp = (uint32_t)i * (num_fixtures - 1) * 256 / (num_leds - 1);
                uint16_t idx_low = pos_fp >> 8;         /* Целая часть = левый прибор */
                uint16_t idx_high = idx_low + 1;        /* Правый прибор */
                if (idx_high >= num_fixtures) idx_high = num_fixtures - 1;
                uint8_t weight = pos_fp & 0xFF;         /* Дробная часть = вес (0-255) */

                /* Линейная интерполяция: result = left * (256-w) + right * w */
                led_color_t color;
                color.r = ((uint32_t)stream_fixture_colors[idx_low][0] * (256 - weight) +
                           (uint32_t)stream_fixture_colors[idx_high][0] * weight) >> 8;
                color.g = ((uint32_t)stream_fixture_colors[idx_low][1] * (256 - weight) +
                           (uint32_t)stream_fixture_colors[idx_high][1] * weight) >> 8;
                color.b = ((uint32_t)stream_fixture_colors[idx_low][2] * (256 - weight) +
                           (uint32_t)stream_fixture_colors[idx_high][2] * weight) >> 8;
                back[i] = color;
            }
        }
    } else {
        /* --- БЕЗ ИНТЕРПОЛЯЦИИ: каждый прибор占据 равный диапазон пикселей --- */
        for (uint16_t pe = 0; pe < num_fixtures; pe++) {
            uint16_t start = stream_ws_start[pe];
            uint16_t count = stream_ws_count[pe];
            led_color_t color = {
                .r = stream_fixture_colors[pe][0],
                .g = stream_fixture_colors[pe][1],
                .b = stream_fixture_colors[pe][2]
            };
            for (uint16_t j = 0; j < count; j++) {
                if (start + j < num_leds)
                    back[start + j] = color;
            }
        }
    }

    /* Повторная проверка тестового режима перед swap (защита от гонки) */
    if (g_led_test_mode != 0) return;

    /* Меняем буферы: back становится активным → led_ref его отобразит */
    led_strip_swap_banks();

    /* Сбрасываем fallback — DMX-кадр принят, сигнал есть */
    s_fallback_pending = false;
    xSemaphoreGive(s_led_refresh_sem);

    /* Перезапускаем таймер fallback на следующий таймаут.
     * Любой принятый кадр (даже все нули) = валидный DMX-сигнал. */
    dmx_lock();
    g_dmx.last_rx_ms[port] = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t fb_timeout = g_dmx.fallback_timeout_ms;
    dmx_unlock();

    if (fb_timeout > 0 && s_fallback_timer) {
        esp_timer_stop(s_fallback_timer);
        esp_timer_start_once(s_fallback_timer, (uint64_t)fb_timeout * 1000);
    }
}

void dmx_process_frame(int port, const uint8_t *slots, int num_slots) {
    process_dmx_frame(port, slots, num_slots);
}

void dmx_process_leds(int port) {
    uint8_t buf[BYPASS_DMX_SIZE];
    dmx_get_channel_data(port, buf, DMX_CHANNELS);
    do_led_processing(port, buf, DMX_CHANNELS);
}

/* ======================================================================
 * ЗАДАЧА ПРИЁМА DMX
 * ====================================================================== */

/**
 * @brief Задача приёма DMX-кадров с одного порта
 *
 * Polling bare-metal bypass каждые 2мс.
 * Создаётся дважды: для порта 0 и порта 1.
 *
 * @param arg Номер порта (0 или 1), приведённый к void*
 */
static void dmx_stream_rx_task(void *arg) {
    int port = (int)(intptr_t)arg;

    uart_bypass_set_notify_task(port, xTaskGetCurrentTaskHandle());

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        dmx_process_leds(port);
    }
}

/* ======================================================================
 * ЗАДАЧА ПЕРЕДАЧИ DMX
 * ====================================================================== */

/**
 * @brief Задача передачи DMX-кадров (режим TESTER и PATCH)
 *
 * В режиме TESTER:
 *   - Point: отправляет кадр с одним RGB-значением на заданном адресе
 *   - Fill: заполняет кадр одинаковым цветом на всех каналах
 *   - Интервал передачи: 33.3 мс (30 FPS, стандарт DMX512)
 *
 * В режиме PATCH:
 *   - Отправляет кадр с подсветкой приборов:
 *     * Текущий пиксель (курсор) → белый (255,255,255)
 *     * Пропущенные (skip) → красный (128,0,0)
 *     * Активные → зелёный (0,128,0)
 *
 * В режиме SNIFFER:
 *   - Просто ждёт (задача неактивна)
 */
static void dmx_tx_task(void *arg) {
    uint8_t frame[DMX_CHANNELS + 1];     /* +1 для start code */
    dmx_mode_t prev_mode = DMX_MODE_SNIFFER;

    while (1) {
        /* Читаем текущий режим */
        dmx_lock();
        dmx_mode_t mode = g_dmx.mode;
        dmx_unlock();

        /* При смене режима на TESTER/PATCH — останавливаем fallback-таймер
         * и даём время переключиться UART */
        if (mode != prev_mode) {
            if (mode == DMX_MODE_TESTER || mode == DMX_MODE_PATCH) {
                if (s_fallback_timer) esp_timer_stop(s_fallback_timer);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            prev_mode = mode;
        }

        if (mode == DMX_MODE_TESTER) {
            /* --- РЕЖИМ ТЕСТЕРА: генерация DMX-кадра --- */
            dmx_lock();
            int txp = g_dmx.tx_port;
            uint16_t txch = g_dmx.tx_channel;
            uint8_t tr = g_dmx.tx_r, tg = g_dmx.tx_g, tb = g_dmx.tx_b;
            tx_mode_t txm = g_dmx.tx_mode;
            uint16_t txc = g_dmx.tx_count;
            dmx_unlock();

            if (txp < 0 || txp > DMX_PORT_COUNT) txp = 0;

            memset(frame, 0, sizeof(frame));
            frame[0] = 0;  /* Start code = 0x00 (DMX512) */

            if (txm == TX_MODE_POINT) {
                /* Точечный тест: RGB на конкретном адресе */
                if (txch >= 1 && txch + 2 <= DMX_CHANNELS) {
                    frame[txch]     = tr;
                    frame[txch + 1] = tg;
                    frame[txch + 2] = tb;
                }
            } else {
                /* Заполнение: одинаковый цвет на всех приборах подряд */
                for (uint16_t i = 0; i < txc && i * 3 + 3 <= DMX_CHANNELS; i++) {
                    uint16_t addr = i * 3 + 1;
                    frame[addr]     = tr;
                    frame[addr + 1] = tg;
                    frame[addr + 2] = tb;
                }
            }

            /* Отправка с соблюдением минимального интервала 33.3 мс (30 FPS) */
            int64_t t_start = esp_timer_get_time();
            if (txp >= DMX_PORT_COUNT) {
                dmx_hal_send(0, frame, DMX_CHANNELS + 1, 100);
                dmx_hal_send(1, frame, DMX_CHANNELS + 1, 100);
            } else {
                dmx_hal_send(txp, frame, DMX_CHANNELS + 1, 100);
            }
            int64_t min_interval = 33333;  /* 33.3 мкс → 30 FPS */
            int64_t remain = min_interval - (esp_timer_get_time() - t_start);
            if (remain > 2000) {
                vTaskDelay(pdMS_TO_TICKS((remain / 1000) - 1));
            }
            while (esp_timer_get_time() - t_start < min_interval) {}

        } else if (mode == DMX_MODE_PATCH) {
            /* --- РЕЖИМ ПАЧА: подсветка приборов --- */
            dmx_lock();
            int uni = g_dmx.patch_universe;
            int cursor = g_dmx.patch_cursor;
            uint8_t n_entries = g_patch.count;
            dmx_unlock();

            int port = (uni == 2) ? DMX_PORT_2 : DMX_PORT_1;

            memset(frame, 0, sizeof(frame));
            frame[0] = 0;

            for (int f = 0; f < n_entries && f * 3 + 3 <= DMX_CHANNELS; f++) {
                dmx_lock();
                uint16_t addr = g_patch.entries[f].dmx_addr;
                bool skip = g_patch.entries[f].skip;
                dmx_unlock();

                if (f == cursor) {
                    /* Текущий прибор (курсор) — белый */
                    frame[addr]     = 255;
                    frame[addr + 1] = 255;
                    frame[addr + 2] = 255;
                } else if (skip) {
                    /* Пропущенный прибор — красный */
                    frame[addr]     = 128;
                    frame[addr + 1] = 0;
                    frame[addr + 2] = 0;
                } else {
                    /* Активный прибор — зелёный */
                    frame[addr]     = 0;
                    frame[addr + 1] = 128;
                    frame[addr + 2] = 0;
                }
            }

            /* Отправка с интервалом 33.3 мс */
            int64_t t_start = esp_timer_get_time();
            dmx_hal_send(port, frame, DMX_CHANNELS + 1, 100);
            int64_t min_interval = 33333;
            int64_t remain = min_interval - (esp_timer_get_time() - t_start);
            if (remain > 2000) {
                vTaskDelay(pdMS_TO_TICKS((remain / 1000) - 1));
            }
            while (esp_timer_get_time() - t_start < min_interval) {}

        } else {
            /* РЕЖИМ СНИФФЕР: задача передачи неактивна */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ======================================================================
 * УТИЛИТЫ
 * ====================================================================== */

/**
 * @brief Применить порядок каналов к цвету
 *
 * Переставляет компоненты R, G, B согласно выбранному порядку.
 * Используется для fallback-цвета и тестовых цветов.
 *
 * @param ch_order Индекс порядка каналов (CH_ORDER_RGB, CH_ORDER_GRB, ...)
 * @param r, g, b  Указатели на компоненты цвета (модифицируются)
 */
void dmx_apply_color_order(uint8_t ch_order, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (ch_order >= CH_ORDER_COUNT) return;
    uint8_t raw[3] = {*r, *g, *b};
    *r = raw[s_order_map[ch_order][0]];
    *g = raw[s_order_map[ch_order][1]];
    *b = raw[s_order_map[ch_order][2]];
}

/* ======================================================================
 * FALLBACK-МЕХАНИЗМ
 * ====================================================================== */

/**
 * @brief Callback таймера fallback (вызывается из esp_timer task)
 *
 * Записывает fallback-цвет в буфер и ставит флаг s_fallback_pending.
 * Задача led_ref обнаружит флаг и вызовет led_strip_refresh().
 */
static void fallback_timer_callback(void *arg) {
    dmx_apply_fallback();
}

/** Внешняя ссылка на флаг тестового режима (объявлен в web_server.c) */
extern volatile int g_led_test_mode;

/**
 * @brief Применить fallback-цвет к LED-ленте
 *
 * Вызывается из:
 *   1. fallback_timer_callback — при потере DMX-сигнала
 *   2. web_server — при ручном сбросе тестового режима ("clear")
 *
 * Записывает fallback-цвет во все пиксели активного буфера,
 * затем сигнализирует задаче led_ref для обновления ленты.
 */
void dmx_apply_fallback(void) {
    dmx_lock();
    bool is_sniffer = (g_dmx.mode == DMX_MODE_SNIFFER);
    uint8_t fb_r = g_dmx.fallback_r;
    uint8_t fb_g = g_dmx.fallback_g;
    uint8_t fb_b = g_dmx.fallback_b;
    uint8_t ch_order = g_dmx.channel_order;
    dmx_unlock();

    /* Fallback работает только в режиме сниффера и не во время теста LED */
    if (!is_sniffer || g_led_strip.count == 0 || g_led_test_mode != 0) return;

    /* Применяем порядок каналов чтобы fallback-цвет соответствовал DMX */
    dmx_apply_color_order(ch_order, &fb_r, &fb_g, &fb_b);

    /* Записываем fallback-цвет во все пиксели */
    led_strip_lock();
    for (uint16_t i = 0; i < g_led_strip.count; i++) {
        g_led_strip.colors[i].r = fb_r;
        g_led_strip.colors[i].g = fb_g;
        g_led_strip.colors[i].b = fb_b;
    }
    led_strip_unlock();

    /* Ставим флаг и сигналим задаче led_ref */
    s_fallback_pending = true;
    xSemaphoreGive(s_led_refresh_sem);
}

/* ======================================================================
 * ИНИЦИАЛИЗАЦИЯ
 * ====================================================================== */

/**
 * @brief Инициализация DMX-подсистемы
 *
 * Выполняет:
 *   1. Создание мьютекса g_dmx_mutex
 *   2. Создание семафора s_led_refresh_sem (binary semaphore)
 *   3. Инициализацию HAL (аппаратные UART-модули DMX)
 *   4. Очистку таблицы цветов приборов
 *   5. Создание one-shot таймера fallback
 *   6. Первый запуск таймера fallback (срабатывает если DMX не придёт)
 *
 * Вызывается ОДИН раз из app_main().
 */
void dmx_init(void) {
    g_dmx_mutex = xSemaphoreCreateMutex();
    s_led_refresh_sem = xSemaphoreCreateCounting(10, 0);

    dmx_hal_init();

    memset(stream_fixture_colors, 0, sizeof(stream_fixture_colors));

    /* One-shot таймер: вызывает fallback_timer_callback после fallback_timeout_ms */
    const esp_timer_create_args_t timer_args = {
        .callback = &fallback_timer_callback,
        .name     = "dmx_fallback"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_fallback_timer));

    /* Первый запуск таймера — если DMX не придёт за fallback_timeout_ms,
     * сразу применится fallback-цвет */
    uint16_t fb_timeout = g_dmx.fallback_timeout_ms;
    if (fb_timeout > 0) {
        ESP_ERROR_CHECK(esp_timer_start_once(s_fallback_timer, (uint64_t)fb_timeout * 1000));
    }
}

/**
 * @brief Переключить режим работы DMX
 *
 * @param m Новый режим (DMX_MODE_SNIFFER / DMX_MODE_TESTER / DMX_MODE_PATCH)
 */
void dmx_set_mode(dmx_mode_t m) {
    bool tx_mode = (m == DMX_MODE_TESTER || m == DMX_MODE_PATCH);

    dmx_lock();
    g_dmx.mode = m;
    dmx_unlock();

    if (tx_mode) {
        uart_bypass_set_tx_mode(0, true);
        uart_bypass_set_tx_mode(1, true);
    }

    int dir = tx_mode ? 1 : 0;
    uart_bypass_set_dir(0, dir);
    uart_bypass_set_dir(1, dir);

    if (!tx_mode) {
        uart_bypass_set_tx_mode(0, false);
        uart_bypass_set_tx_mode(1, false);
    }

    ESP_LOGI(DMX_TAG, "Mode: %s, DIR=%d",
             m == DMX_MODE_SNIFFER ? "SNIFFER" :
             m == DMX_MODE_TESTER  ? "TESTER"  : "PATCH", dir);
}

/* ======================================================================
 * ЗАПУСК ЗАДАЧ
 * ====================================================================== */

/**
 * @brief Запустить задачи приёма DMX и обновления LED
 *
 * Создаёт:
 *   - dmx_rx0: приём с порта 0 (ядро 1, приоритет 4)
 *   - dmx_rx1: приём с порта 1 (ядро 1, приоритет 4)
 *   - led_ref: обновление LED-ленты (ядро 1, приоритет 3)
 */
void dmx_start_rx_task(void) {
    xTaskCreatePinnedToCore(dmx_stream_rx_task, "dmx_rx0",
                            8192, (void *)(intptr_t)0, 4, NULL, 0);
    xTaskCreatePinnedToCore(dmx_stream_rx_task, "dmx_rx1",
                            8192, (void *)(intptr_t)1, 4, NULL, 1);
    xTaskCreatePinnedToCore(led_refresh_task, "led_ref", 4096, NULL, 5, NULL, 1);
}

/**
 * @brief Запустить задачу передачи DMX
 *
 * dmx_tx: передача DMX-кадров (ядро 1, приоритет 5)
 */
void dmx_start_tx_task(void) {
    xTaskCreatePinnedToCore(dmx_tx_task, "dmx_tx", 8192, NULL, 5, NULL, 1);
}
