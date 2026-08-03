/**
 * @file dmx_led.c
 * @brief DMX → LED мост: патч, интерполяция, fallback
 *
 * Чистый модуль — без signal.h, без event_bus.h.
 * Все действия через callback struct из dmx_led_init().
 *
 * Алгоритм работы:
 *   1. При получении DMX-кадра (callback on_dmx_frame)
 *      вызывается do_led_processing().
 *   2. Для каждой активной фикстуры из DMX-патча
 *      извлекаются 3 канала (R, G, B) и переставляются
 *      в соответствии с channel_order.
 *   3. Цвета фикстур записываются в пиксели LED-ленты:
 *      - в режиме SEQUENTIAL — все фикстуры → общая лента
 *      - в режиме PARALLEL  — каждая фикстура → своя полоса
 *   4. При включённой интерполяции между фикстурами
 *      выполняется линейная смешивание по 8-битному весу.
 *   5. Если DMX-кадр не приходит дольше fallback_timeout_ms,
 *      таймер запускает fallback-процедуру, заливая ленту
 *      фиксированным цветом.
 */

#include "dmx/dmx.h"
#include "dmx/dmx_led.h"
#include "settings.h"
#include "led_driver/ws2812.h"
#include "utilite/patch_manager.h"
#include "utilite/effects.h"
#include "esp_log.h"

/**
 * @brief Глобальные настройки моста DMX→LED
 *
 * Инициализируются значениями по умолчанию:
 *   - reverse     = false   (прямой порядок пикселей)
 *   - shift       = 0       (без сдвига)
 *   - interpolate = true    (интерполяция включена)
 *   - channel_order = CH_ORDER_RGB (стандартный порядок)
 *
 * Остальные поля (fallback_*) инициализируются нулями
 * и настраиваются через handle_fixture_settings().
 */
dmx_led_settings_t g_led_settings = {
    .reverse      = false,
    .shift        = 0,
    .interpolate  = true,
    .channel_order = CH_ORDER_RGB
};

#include "esp_timer.h"
#include <string.h>

static const char *TAG = "DMX_LED";

/* ===== Callbacks (устанавливаются при init) ===== */

/** @brief Указатель на структуру callback-функций (on_mode, on_led_test и т.д.) */
static const dmx_led_cbs_t *s_cbs = NULL;

/* ===== Таблицы для быстрого маппинга ===== */

/**
 * @brief Начальный пиксель для каждой фикстуры
 *
 * s_ws_start[f] — индекс первого пикселя фикстуры @c f
 * в общем массиве LED. Вычисляется в dmx_led_recompute()
 * по формуле: start[f] = f * total_leds / patch.count.
 */
static uint16_t s_ws_start[PATCH_MAX_ENTRIES];

/**
 * @brief Количество пикселей для каждой фикстуры
 *
 * s_ws_count[f] — сколько пикселей отведено фикстуре @c f.
 * Для последней фикстуры: count = total_leds - start[f].
 * Вычисляется в dmx_led_recompute().
 */
static uint16_t s_ws_count[PATCH_MAX_ENTRIES];

/**
 * @brief Текущий цвет каждой фикстуры (с учётом channel_order)
 *
 * s_fixture_colors[f][0..2] — R, G, B для фикстуры @c f
 * после перестановки каналов. Заполняется в do_led_processing()
 * из DMX-слотов и используется модулем Effects для чтения.
 */
static uint8_t  s_fixture_colors[PATCH_MAX_ENTRIES][3];

/**
 * @brief Таблица перестановки цветовых каналов
 *
 * s_order_map[порядок][индекс_канала] = исходный_индекс.
 * Для порядка GRB: канал 0 берётся из позиции 1 (G),
 * канал 1 — из позиции 0 (R), канал 2 — из позиции 2 (B).
 *
 * Позволяет переставлять каналы без условных операторов:
 * @code
 *   uint8_t raw[3] = {r, g, b};
 *   r = raw[s_order_map[order][0]];
 *   g = raw[s_order_map[order][1]];
 *   b = raw[s_order_map[order][2]];
 * @endcode
 */
static const uint8_t s_order_map[CH_ORDER_COUNT][3] = {
    [CH_ORDER_RGB] = {0, 1, 2}, [CH_ORDER_RBG] = {0, 2, 1},
    [CH_ORDER_GRB] = {1, 0, 2}, [CH_ORDER_GBR] = {1, 2, 0},
    [CH_ORDER_BRG] = {2, 0, 1}, [CH_ORDER_BGR] = {2, 1, 0},
};

/* ===== Механизм fallback ===== */

/**
 * @brief Таймер обратного отсчёта (fallback)
 *
 * Одноразовый таймер esp_timer. Запускается при каждом
 * успешном приёме DMX-кадра. Если за fallback_timeout_ms
 * не приходит новый кадр — срабатывает callback
 * fallback_timer_callback(), который устанавливает
 * флаг s_fallback_pending и даёт семафор.
 */
static esp_timer_handle_t s_fallback_timer = NULL;

/**
 * @brief Семафор для сигнализации обновления LED
 *
 * Используется для пробуждения led_refresh_task из
 * fallback_timer_callback и do_led_processing.
 * Тип: counting semaphore (макс. значение 10).
 */
static SemaphoreHandle_t  s_led_refresh_sem = NULL;

/**
 * @brief Флаг: запланирован fallback
 *
 * Устанавливается в true при срабатывании таймера
 * или при вызове dmx_led_apply_fallback().
 * Сбрасывается в led_refresh_task после обработки.
 */
static volatile bool      s_fallback_pending = false;

/**
 * @brief Callback таймера fallback
 *
 * Вызывается esp_timer при истечении таймаута.
 * Устанавливает флаг и даёт семафор для пробуждения
 * задачи led_refresh_task.
 *
 * @param[in] arg  Не используется (NULL).
 */
static void fallback_timer_callback(void *arg) {
    (void)arg;
    s_fallback_pending = true;
    xSemaphoreGive(s_led_refresh_sem);
}

/* ===== Задача обновления LED ===== */

/**
 * @brief Задача обновления состояния LED-ленты
 *
 * Запускается на ядре 1 с приоритетом 5.
 * В бесконечном цикле ждёт семафор (таймаут 5 мс).
 * При получении сигнала:
 *   1. Если s_fallback_pending — применяет fallback-цвет.
 *   2. Вызывает effects_render() для обработки эффектов.
 *   3. Обновляет физические ленты (порты 0 и 1).
 *
 * Задача работает параллельно с DMX-обработкой и
 * обеспечивает плавное обновление LED без блокировки
 * основного потока.
 *
 * @param[in] arg  Не используется (NULL).
 */
static void led_refresh_task(void *arg) {
    (void)arg;
    while (1) {
        xSemaphoreTake(s_led_refresh_sem, pdMS_TO_TICKS(5));
        if (s_fallback_pending) {
            s_fallback_pending = false;
            dmx_led_apply_fallback();
        }
        effects_render();
        led_refresh(0);
        led_refresh(1);
    }
}

/* ===== Публичный API ===== */

void dmx_led_recompute(void) {
    /**
     * Пересчитывает распределение фикстур по пикселям LED.
     *
     * Алгоритм:
     *   1. Блокируем DMX для чтения патча и количества LED.
     *   2. Для каждой фикстуры f вычисляем стартовый пиксель:
     *        start[f] = f * total_leds / patch.count
     *   3. Количество пикселей фикстуры:
     *        count[f] = start[f+1] - start[f]
     *      (для последней фикстуры: count = total_leds - start)
     *
     * Пример: 3 фикстуры, 300 пикселей
     *   f=0: start=0,   count=100
     *   f=1: start=100, count=100
     *   f=2: start=200, count=100
     *
     * При неравном делении (например 302 пикселя, 3 фикстуры):
     *   f=0: start=0,   count=101  (302*0/3=0, 302*1/3=101)
     *   f=1: start=101, count=100  (302*1/3=100, 302*2/3=201)
     *   f=2: start=201, count=101  (302*2/3=200, 302*3/3=302)
     */
    dmx_lock();
    uint16_t n = g_patch.count;
    uint16_t m = g_total_leds;
    if (n > 0) {
        for (uint16_t f = 0; f < n; f++)
            s_ws_start[f] = (uint32_t)f * m / n;
        for (uint16_t f = 0; f < n; f++) {
            uint16_t next = (f + 1 < n) ? s_ws_start[f + 1] : m;
            s_ws_count[f] = next - s_ws_start[f];
        }
    }
    dmx_unlock();
}

/**
 * @brief Применить перестановку цветовых каналов
 *
 * Переставляет компоненты R, G, B в соответствии с
 * указанным порядком каналов. Использует таблицу
 * s_order_map для быстрого маппинга без ветвлений.
 *
 * @param[in]     ch_order  Индекс порядка (channel_order_t)
 * @param[in,out] r         Указатель на красный компонент
 * @param[in,out] g         Указатель на зелёный компонент
 * @param[in,out] b         Указатель на синий компонент
 */
static void apply_color_order(uint8_t ch_order, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (ch_order >= CH_ORDER_COUNT) return;
    uint8_t raw[3] = {*r, *g, *b};
    *r = raw[s_order_map[ch_order][0]];
    *g = raw[s_order_map[ch_order][1]];
    *b = raw[s_order_map[ch_order][2]];
}

void dmx_led_apply_fallback(void) {
    /**
     * Применяет fallback-цвет ко всей LED-ленте (порт 0).
     *
     * Алгоритм:
     *   1. Проверяем, что устройство в режиме сниффера
     *      (DMX_MODE_SNIFFER) — в тестере и патче fallback
     *      не применяется.
     *   2. Проверяем, что есть пиксели и нет активных эффектов.
     *   3. Переставляем каналы fallback-цвета по channel_order.
     *   4. Записываем одинаковый цвет во все пиксели ленты.
     *   5. Даём семафор для обновления физических LED.
     *
     * Функция безопасна: не записывает при пустой ленте
     * или активных эффектах (effects_is_active() == true).
     */
    dmx_lock();
    bool is_sniffer = (g_dmx.mode == DMX_MODE_SNIFFER);
    uint8_t fb_r = g_led_settings.fallback_r, fb_g = g_led_settings.fallback_g, fb_b = g_led_settings.fallback_b;
    uint8_t ch_order = g_led_settings.channel_order;
    dmx_unlock();

    if (!is_sniffer || led_get_count(0) == 0 || effects_is_active()) return;

    apply_color_order(ch_order, &fb_r, &fb_g, &fb_b);

    led_lock(0);
    led_color_t *back = led_get_colors(0);
    uint16_t cnt = led_get_count(0);
    for (uint16_t i = 0; i < cnt; i++) {
        back[i].r = fb_r;
        back[i].g = fb_g;
        back[i].b = fb_b;
    }
    led_unlock(0);

    s_fallback_pending = true;
    xSemaphoreGive(s_led_refresh_sem);
}

/* ===== Обработка DMX-кадра → буфер LED ===== */

/**
 * @brief Основная функция маппинга DMX → LED
 *
 * Вызывается из on_dmx_frame при получении каждого
 * DMX-кадра. Выполняет полный цикл:
 *
 * 1. Чтение параметров (режим, патч, настройки) под мьютексом.
 * 2. Проверка: режим должен быть SNIFFER, эффекты не активны.
 * 3. Для каждой фикстуры из патча:
 *    - Проверка флага skip (пропуск неактивных фикстур).
 *    - Проверка, что DMX-адрес + 3 канала не выходят за
 *      пределы принятого кадра (max_slot).
 *    - Извлечение 3 каналов R, G, B и перестановка
 *      по channel_order.
 * 4. Запись цветов в буфер LED:
 *
 *    **Режим SEQUENTIAL** (последовательный):
 *      Все фикстуры маппятся на общую цепочку пикселей
 *      (порт 0 + порт 1). Если интерполяция включена,
 *      между фикстурами выполняется линейное смешивание.
 *      Если выключена — каждая фикстура занимает равный
 *      диапазон пикселей (s_ws_start / s_ws_count).
 *
 *    **Режим PARALLEL** (параллельный):
 *      Каждый DMX-порт (0 и 1) независимо получает
 *      все фикстуры. Каждая фикстура маппится на равный
 *      диапазон пикселей данного порта. Интерполяция
 *      работает аналогично SEQUENTIAL.
 *
 * 5. Interpolация (детали):
 *      Позиция пикселя → взвешенная позиция фикстуры:
 *        pos_fp = pixel * (n_entries - 1) * 256 / (total - 1)
 *      Целая часть pos_fp → индекс нижней фикстуры (lo),
 *      младший байт → вес (w) для линейной интерполяции:
 *        color = color_lo * (256 - w) + color_hi * w
 *
 * 6. Переключение банков (led_swap_banks) для двойной буферизации.
 *
 * 7. Сброс fallback-таймера: при успешном приёме кадра
 *    таймер перезапускается. Если кадр не приходит
 *    дольше fallback_timeout_ms — сработает fallback.
 *
 * @param[in] port     Номер DMX-порта (0 или 1)
 * @param[in] slots    Указатель на DMX-слоты (без старт-кода)
 * @param[in] max_slot Количество принятых слотов
 */
static void do_led_processing(int port, const uint8_t *slots, uint16_t max_slot) {
    dmx_lock();
    dmx_mode_t mode = g_dmx.mode;
    uint8_t cpf = DMX_FIXTURE_CH;
    uint8_t n_entries = g_patch.count;
    led_mode_t led_mode = g_led_mode;
    dmx_unlock();

    if (mode != DMX_MODE_SNIFFER) return;
    if (effects_is_active()) return;

    /* Извлечение цветов из DMX-слотов для каждой фикстуры */
    for (uint8_t f = 0; f < n_entries; f++) {
        dmx_lock();
        bool skip = g_patch.entries[f].skip;
        uint16_t addr = g_patch.entries[f].dmx_addr;
        uint8_t co = g_led_settings.channel_order;
        dmx_unlock();

        if (skip) continue;
        if (addr + cpf - 1 > max_slot) continue;

        dmx_lock();
        uint8_t raw[3] = { slots[addr - 1], slots[addr], slots[addr + 1] };
        dmx_unlock();
        s_fixture_colors[f][0] = raw[s_order_map[co][0]];
        s_fixture_colors[f][1] = raw[s_order_map[co][1]];
        s_fixture_colors[f][2] = raw[s_order_map[co][2]];
    }

    uint16_t count1 = led_get_count(0);
    uint16_t count2 = led_get_count(1);
    led_color_t *back1 = led_get_colors(0);
    led_color_t *back2 = led_get_colors(1);

    dmx_lock();
    bool interp = g_led_settings.interpolate;
    bool rev = g_led_settings.reverse;
    uint16_t shft = g_led_settings.shift;
    dmx_unlock();

    led_set_reverse(0, rev); led_set_shift(0, shft);
    led_set_reverse(1, rev); led_set_shift(1, shft);

    if (led_mode == LED_MODE_SEQUENTIAL) {
        /* ===== Последовательный режим =====
         * Все фикстуры → общая лента (порт 0 + порт 1).
         * Если count2 > 0 — используется временный буфер,
         * затем копируется в оба порта. */
        uint16_t total = count1 + count2;
        if (total == 0) goto do_fallback;

        led_color_t *tmp = (count2 > 0) ? calloc(total, sizeof(led_color_t)) : back1;
        if (!tmp) goto do_fallback;

        if (interp) {
            /* Интерполяция: плавное смешивание между фикстурами.
             * Для каждого пикселя вычисляется «взвешенная позиция»
             * pos_fp = pixel * (n-1) * 256 / (total-1).
             * Целая часть → индекс lo, младший байт → вес w.
             * color = color[lo] * (256-w) + color[hi] * w. */
            if (n_entries < 2 || total <= 1) {
                led_color_t color = {0, 0, 0};
                if (n_entries > 0) {
                    color.r = s_fixture_colors[0][0];
                    color.g = s_fixture_colors[0][1];
                    color.b = s_fixture_colors[0][2];
                }
                for (uint16_t i = 0; i < total; i++) tmp[i] = color;
            } else {
                for (uint16_t i = 0; i < total; i++) {
                    uint32_t pos_fp = (uint32_t)i * (n_entries - 1) * 256 / (total - 1);
                    uint16_t lo = pos_fp >> 8, hi = lo + 1;
                    if (hi >= n_entries) hi = n_entries - 1;
                    uint8_t w = pos_fp & 0xFF;
                    tmp[i].r = ((uint32_t)s_fixture_colors[lo][0] * (256 - w) + (uint32_t)s_fixture_colors[hi][0] * w) >> 8;
                    tmp[i].g = ((uint32_t)s_fixture_colors[lo][1] * (256 - w) + (uint32_t)s_fixture_colors[hi][1] * w) >> 8;
                    tmp[i].b = ((uint32_t)s_fixture_colors[lo][2] * (256 - w) + (uint32_t)s_fixture_colors[hi][2] * w) >> 8;
                }
            }
        } else {
            /* Без интерполяции: каждая фикстура — равный блок пикселей.
             * Локальные таблицы start/count пересчитываются для общего
             * количества пикселей (count1 + count2). */
            uint16_t ws_start_l[PATCH_MAX_ENTRIES], ws_count_l[PATCH_MAX_ENTRIES];
            for (uint16_t f = 0; f < n_entries; f++)
                ws_start_l[f] = (uint32_t)f * total / n_entries;
            for (uint16_t f = 0; f < n_entries; f++) {
                uint16_t next = (f + 1 < n_entries) ? ws_start_l[f + 1] : total;
                ws_count_l[f] = next - ws_start_l[f];
            }
            for (uint16_t pe = 0; pe < n_entries; pe++) {
                led_color_t c = { s_fixture_colors[pe][0], s_fixture_colors[pe][1], s_fixture_colors[pe][2] };
                for (uint16_t j = 0; j < ws_count_l[pe]; j++) {
                    uint16_t px = ws_start_l[pe] + j;
                    if (px < total) tmp[px] = c;
                }
            }
        }

        /* Если эффекты активны — отменяем запись в LED */
        if (effects_is_active()) {
            if (tmp != back1) free(tmp);
            return;
        }

        /* Копирование в физические буферы и переключение банков */
        if (count1 > 0) {
            led_lock(0);
            memcpy(back1, tmp, count1 * sizeof(led_color_t));
            led_unlock(0);
            led_swap_banks(0);
        }
        if (count2 > 0) {
            led_lock(1);
            memcpy(back2, tmp + count1, count2 * sizeof(led_color_t));
            led_unlock(1);
            led_swap_banks(1);
        }
        if (tmp != back1) free(tmp);

    } else {
        /* ===== Параллельный режим =====
         * Каждый DMX-порт (0 и 1) независимо маппит
         * все фикстуры на свою полосу пикселей. */
        if (n_entries == 0) goto do_fallback;

        /* Порт 0 */
        if (count1 > 0) {
            if (interp) {
                /* Интерполяция для порта 0:
                 * Аналогична SEQUENTIAL, но с count1 пикселями */
                if (n_entries < 2 || count1 <= 1) {
                    led_color_t c = {0, 0, 0};
                    if (n_entries > 0) {
                        c.r = s_fixture_colors[0][0];
                        c.g = s_fixture_colors[0][1];
                        c.b = s_fixture_colors[0][2];
                    }
                    for (uint16_t i = 0; i < count1; i++) back1[i] = c;
                } else {
                    for (uint16_t i = 0; i < count1; i++) {
                        uint32_t pos_fp = (uint32_t)i * (n_entries - 1) * 256 / (count1 - 1);
                        uint16_t lo = pos_fp >> 8, hi = lo + 1;
                        if (hi >= n_entries) hi = n_entries - 1;
                        uint8_t w = pos_fp & 0xFF;
                        back1[i].r = ((uint32_t)s_fixture_colors[lo][0] * (256 - w) + (uint32_t)s_fixture_colors[hi][0] * w) >> 8;
                        back1[i].g = ((uint32_t)s_fixture_colors[lo][1] * (256 - w) + (uint32_t)s_fixture_colors[hi][1] * w) >> 8;
                        back1[i].b = ((uint32_t)s_fixture_colors[lo][2] * (256 - w) + (uint32_t)s_fixture_colors[hi][2] * w) >> 8;
                    }
                }
            } else {
                /* Без интерполяции для порта 0:
                 * Каждая фикстура — равный блок пикселей */
                for (uint16_t f = 0; f < n_entries; f++) {
                    uint16_t start = (uint32_t)f * count1 / n_entries;
                    uint16_t next = (f + 1 < n_entries) ? (uint32_t)(f + 1) * count1 / n_entries : count1;
                    led_color_t c = { s_fixture_colors[f][0], s_fixture_colors[f][1], s_fixture_colors[f][2] };
                    for (uint16_t j = start; j < next; j++) back1[j] = c;
                }
            }
        }

        /* Порт 1 */
        if (count2 > 0) {
            if (interp) {
                /* Интерполяция для порта 1:
                 * Аналогична порту 0, но с count2 пикселями */
                if (n_entries < 2 || count2 <= 1) {
                    led_color_t c = {0, 0, 0};
                    if (n_entries > 0) {
                        c.r = s_fixture_colors[0][0];
                        c.g = s_fixture_colors[0][1];
                        c.b = s_fixture_colors[0][2];
                    }
                    for (uint16_t i = 0; i < count2; i++) back2[i] = c;
                } else {
                    for (uint16_t i = 0; i < count2; i++) {
                        uint32_t pos_fp = (uint32_t)i * (n_entries - 1) * 256 / (count2 - 1);
                        uint16_t lo = pos_fp >> 8, hi = lo + 1;
                        if (hi >= n_entries) hi = n_entries - 1;
                        uint8_t w = pos_fp & 0xFF;
                        back2[i].r = ((uint32_t)s_fixture_colors[lo][0] * (256 - w) + (uint32_t)s_fixture_colors[hi][0] * w) >> 8;
                        back2[i].g = ((uint32_t)s_fixture_colors[lo][1] * (256 - w) + (uint32_t)s_fixture_colors[hi][1] * w) >> 8;
                        back2[i].b = ((uint32_t)s_fixture_colors[lo][2] * (256 - w) + (uint32_t)s_fixture_colors[hi][2] * w) >> 8;
                    }
                }
            } else {
                /* Без интерполяции для порта 1 */
                for (uint16_t f = 0; f < n_entries; f++) {
                    uint16_t start = (uint32_t)f * count2 / n_entries;
                    uint16_t next = (f + 1 < n_entries) ? (uint32_t)(f + 1) * count2 / n_entries : count2;
                    led_color_t c = { s_fixture_colors[f][0], s_fixture_colors[f][1], s_fixture_colors[f][2] };
                    for (uint16_t j = start; j < next; j++) back2[j] = c;
                }
            }
        }

        /* Если эффекты активны — отменяем переключение банков */
        if (effects_is_active()) return;
        if (count1 > 0) led_swap_banks(0);
        if (count2 > 0) led_swap_banks(1);
    }

do_fallback:
    /* Сброс флага fallback и перезапуск таймера.
     * Если кадр получен — таймер перезапускается.
     * Если таймер сработает — включится fallback-цвет. */
    s_fallback_pending = false;
    xSemaphoreGive(s_led_refresh_sem);

    dmx_lock();
    g_dmx.last_rx_ms[port] = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t fb_timeout = g_led_settings.fallback_timeout_ms;
    dmx_unlock();

    if (fb_timeout > 0 && s_fallback_timer) {
        esp_timer_stop(s_fallback_timer);
        esp_timer_start_once(s_fallback_timer, (uint64_t)fb_timeout * 1000);
    }
}

const uint8_t (*dmx_led_get_fixture_colors(void))[3] {
    return s_fixture_colors;
}

/* ===== Callback DMX-кадра ===== */

/**
 * @brief Callback, вызываемый при получении DMX-кадра
 *
 * Подписывается через dmx_on_frame() в dmx_led_init().
 * Извлекает данные кадра (без старт-кода) и передаёт
 * в do_led_processing() для маппинга на LED.
 *
 * @param[in] port  Номер DMX-порта (0 или 1)
 */
static void on_dmx_frame(int port) {
    dmx_raw_frame_t *f = &g_raw_frames[port];
    if (f->len > 1)
        do_led_processing(port, f->data + 1, f->len - 1);
}

/* ===== Внутренние обработчики (вызываются из web_server/main) ===== */

/**
 * @brief Обработчик смены режима работы
 *
 * Переключает между режимами DMX:
 *   0 → DMX_MODE_SNIFFER  (приём DMX, мост на LED)
 *   1 → DMX_MODE_TESTER   (передача DMX-теста)
 *   2 → DMX_MODE_PATCH    (настройка патча)
 *
 * @param[in] mode  Код режима (0, 1 или 2)
 */
static void handle_mode(uint8_t mode) {
    if (mode == 0) dmx_set_mode(DMX_MODE_SNIFFER);
    else if (mode == 1) dmx_set_mode(DMX_MODE_TESTER);
    else if (mode == 2) dmx_set_mode(DMX_MODE_PATCH);
}

/**
 * @brief Обработчик тестирования LED
 *
 * При mode == 1: заливает указанный диапазон пикселей
 * одним цветом (с учётом channel_order).
 * При mode != 1: запускает эффект (fx_state_t) через
 * модуль Effects.
 *
 * @param[in] mode   Режим тестирования (0=выкл, 1=заливка, 2+=эффекты)
 * @param[in] r      Красный компонент
 * @param[in] g      Зелёный компонент
 * @param[in] b      Синий компонент
 * @param[in] speed  Скорость эффекта (для режимов != 1)
 * @param[in] pixel  Начальный пиксель (для режима 1)
 * @param[in] count  Количество пикселей (для режима 1)
 */
static void handle_led_test(uint8_t mode, uint8_t r, uint8_t g, uint8_t b,
                            uint8_t speed, uint16_t pixel, uint16_t count) {
    if (mode == 1) {
        effects_clear();
        uint8_t co;
        dmx_lock(); co = g_led_settings.channel_order; dmx_unlock();
        uint8_t cr = r, cg = g, cb = b;
        apply_color_order(co, &cr, &cg, &cb);
        led_lock(0);
        led_color_t *back = led_get_colors(0);
        uint16_t cnt = led_get_count(0);
        memset(back, 0, cnt * sizeof(led_color_t));
        for (int i = 0; i < count && pixel + i < cnt; i++)
            led_set_pixel(0, pixel + i, cr, cg, cb);
        led_unlock(0);
        led_refresh(0);
        return;
    }
    fx_state_t fx = {
        .mode  = mode,
        .r     = r,
        .g     = g,
        .b     = b,
        .speed = speed,
        .pixel = pixel,
        .count = count
    };
    effects_set(&fx);
}

/**
 * @brief Обработчик очистки LED
 *
 * Останавливает все активные эффекты и применяет
 * fallback-цвет (если включён).
 */
static void handle_led_clear(void) {
    effects_clear();
    dmx_led_apply_fallback();
}

/**
 * @brief Обработчик изменения количества пикселей
 *
 * Устанавливает новое количество пикселей для указанной
 * полосы, пересчитывает распределение фикстур
 * (dmx_led_recompute) и обновляет ленту.
 *
 * @param[in] strip  Номер полосы (0 или 1)
 * @param[in] count  Новое количество пикселей
 */
static void handle_led_count(uint8_t strip, uint16_t count) {
    led_set_count(strip, count);
    dmx_led_recompute();
    led_refresh(strip);
}

/**
 * @brief Обработчик переключения обратного порядка пикселей
 *
 * @param[in] on  true — обратный порядок, false — прямой
 */
static void handle_led_reverse(bool on) {
    dmx_lock();
    g_led_settings.reverse = on;
    dmx_unlock();
}

/**
 * @brief Обработчик переключения режима LED
 *
 * @param[in] sequential  true — последовательный режим
 *                        false — параллельный режим
 */
static void handle_led_mode(bool sequential) {
    g_led_mode = sequential ? LED_MODE_SEQUENTIAL : LED_MODE_PARALLEL;
}

/**
 * @brief Обработчик изменения сдвига пикселей
 *
 * @param[in] shift  Смещение начального пикселя (в пикселях)
 */
static void handle_led_shift(uint8_t shift) {
    dmx_lock();
    g_led_settings.shift = shift;
    dmx_unlock();
}

/**
 * @brief Обработчик переключения интерполяции
 *
 * @param[in] on  true — интерполяция включена,
 *                false — жёсткие границы между фикстурами
 */
static void handle_led_interpolate(bool on) {
    dmx_lock();
    g_led_settings.interpolate = on;
    dmx_unlock();
}

/**
 * @brief Обработчик DMX-теста (передатчик)
 *
 * Настраивает параметры передачи DMX-тестового сигнала:
 * порт, канал, цвет, режим (точечный/заливка) и
 * количество каналов. Переключает устройство в режим
 * DMX_MODE_TESTER.
 *
 * @param[in] port     Номер DMX-порта для передачи
 * @param[in] channel  Начальный DMX-канал
 * @param[in] r        Красный компонент
 * @param[in] g        Зелёный компонент
 * @param[in] b        Синий компонент
 * @param[in] fill     true — заливка (count каналов подряд),
 *                     false — точечный (один канал)
 * @param[in] count    Количество каналов (для режима fill)
 */
static void handle_dmx_test(uint8_t port, uint8_t channel,
                            uint8_t r, uint8_t g, uint8_t b,
                            bool fill, uint16_t count) {
    dmx_lock();
    g_dmx.tx_port = port;
    g_dmx.tx_channel = channel;
    g_dmx.tx_r = r;
    g_dmx.tx_g = g;
    g_dmx.tx_b = b;
    g_dmx.tx_mode = fill ? TX_MODE_FILL : TX_MODE_POINT;
    g_dmx.tx_count = count;
    dmx_unlock();
    dmx_set_mode(DMX_MODE_TESTER);
}

/**
 * @brief Обработчик настроек фикстуры
 *
 * Обновляет порядок каналов, цвет fallback и таймаут.
 * После изменения применяет fallback-цвет на ленту.
 *
 * @param[in] channel_order       Индекс порядка каналов (channel_order_t)
 * @param[in] fallback_r          Красный компонент fallback
 * @param[in] fallback_g          Зелёный компонент fallback
 * @param[in] fallback_b          Синий компонент fallback
 * @param[in] fallback_timeout_ms Таймаут fallback в мс (0 = отключён)
 */
static void handle_fixture_settings(uint8_t channel_order,
                                    uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b,
                                    uint16_t fallback_timeout_ms) {
    dmx_lock();
    g_led_settings.channel_order = channel_order;
    g_led_settings.fallback_r = fallback_r;
    g_led_settings.fallback_g = fallback_g;
    g_led_settings.fallback_b = fallback_b;
    g_led_settings.fallback_timeout_ms = fallback_timeout_ms;
    dmx_unlock();
    dmx_led_apply_fallback();
}

/* ===== Инициализация ===== */

void dmx_led_init(const dmx_led_cbs_t *cbs) {
    /**
     * Инициализация DMX→LED моста.
     *
     * Выполняет:
     *   1. Сохранение указателя на callback-структуру.
     *   2. Создание семафора для задачи led_refresh_task.
     *   3. Подписку на DMX-кадры (on_dmx_frame).
     *   4. Создание и запуск одноразового таймера fallback.
     *   5. Запуск задачи обновления LED на ядре 1.
     *
     * @param[in] cbs  Структура callback-функций (on_mode,
     *                 on_led_test и т.д.)
     */
    s_cbs = cbs;
    s_led_refresh_sem = xSemaphoreCreateCounting(10, 0);

    /* Подписка на DMX-кадры */
    dmx_on_frame(on_dmx_frame);

    /* Таймер fallback: одноразовый, перезапускается
     * при каждом успешном приёме кадра */
    const esp_timer_create_args_t timer_args = {
        .callback = &fallback_timer_callback,
        .name     = "dmx_fallback"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_fallback_timer));
    uint16_t fb_timeout = g_led_settings.fallback_timeout_ms;
    if (fb_timeout > 0)
        ESP_ERROR_CHECK(esp_timer_start_once(s_fallback_timer, (uint64_t)fb_timeout * 1000));

    /* Задача обновления LED (ядро 1, приоритет 5, стек 4 КБ) */
    xTaskCreatePinnedToCore(led_refresh_task, "led_ref", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "DMX→LED bridge initialized");
}
