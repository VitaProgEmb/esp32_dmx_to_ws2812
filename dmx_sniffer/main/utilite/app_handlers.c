/**
 * @file app_handlers.c
 * @brief Реализация обработчиков команд от web-сервера
 *
 * Модуль содержит функции-обработчики, которые web-сервер вызывает
 * при получении HTTP-запросов от клиента.  Каждая функция
 * транслирует параметры запроса в вызовы API подсистем DMX, LED
 * и эффектов, обеспечивая связь между веб-интерфейсом и
 * аппаратурой.
 *
 * Потокобезопасность:
 * - Глобальные структуры настроек (@c g_led_settings, @c g_dmx)
 *   защищаются мьютексом @c dmx_lock() / @c dmx_unlock().
 * - Операции с LED-лентами защищаются @c led_lock() / @c led_unlock().
 * - Функции вызываются из контекста HTTP-обработчика (lwIP),
 *   поэтому каждая операция должна быть атомарной или защищённой.
 */

#include "handlers.h"
#include "settings.h"
#include "dmx/dmx.h"
#include "dmx/dmx_led.h"
#include "led_strip.h"
#include "utilite/effects.h"

/* ===== Handlers ===== */

/**
 * @brief Обработчик смены режима работы устройства.
 *
 * Переключает между режимами sniffer (перехват DMX), tester
 * (передача тестовых данных) и patch (патчинг каналов).
 *
 * @param mode  Идентификатор режима:
 *              - 0 — @c DMX_MODE_SNIFFER,
 *              - 1 — @c DMX_MODE_TESTER,
 *              - 2 — @c DMX_MODE_PATCH.
 *
 * @note Функция не требует блокировки — @c dmx_set_mode()
 *       является атомарной.
 */
void handler_mode(uint8_t mode) {
    if (mode == 0) dmx_set_mode(DMX_MODE_SNIFFER);
    else if (mode == 1) dmx_set_mode(DMX_MODE_TESTER);
    else if (mode == 2) dmx_set_mode(DMX_MODE_PATCH);
}

/**
 * @brief Обработчик запуска тестового эффекта на LED-ленте.
 *
 * Если @p mode == 1 (статический цвет), функция очищает все эффекты,
 * устанавливает указанный цвет на диапазон пикселей с учётом
 * порядка каналов (@c channel_order) и обновляет ленту.
 * Для иных значений @p mode запускается динамический эффект через
 * подсистему @c effects.
 *
 * @param mode   Тип эффекта (1 — статический цвет).
 * @param r      Красный компонент (0–255).
 * @param g      Зелёный компонент (0–255).
 * @param b      Синий компонент (0–255).
 * @param speed  Скорость эффекта (0–255).
 * @param pixel  Номер начального пикселя.
 * @param count  Количество пикселей.
 *
 * @note Потокобезопасность: используется @c led_lock() / @c led_unlock()
 *       для защиты буфера цветов и @c dmx_lock() / @c dmx_unlock()
 *       для чтения @c g_led_settings.
 */
void handler_led_test(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint8_t speed, uint16_t pixel, uint16_t count) {
    if (mode == 1) {
        effects_clear();
        uint8_t co;
        dmx_lock(); co = g_led_settings.channel_order; dmx_unlock();
        uint8_t cr = r, cg = g, cb = b;
        static const uint8_t omap[][3] = {
            [CH_ORDER_RGB]={0,1,2},[CH_ORDER_RBG]={0,2,1},
            [CH_ORDER_GRB]={1,0,2},[CH_ORDER_GBR]={1,2,0},
            [CH_ORDER_BRG]={2,0,1},[CH_ORDER_BGR]={2,1,0},
        };
        if (co < CH_ORDER_COUNT) { uint8_t raw[3]={cr,cg,cb}; cr=raw[omap[co][0]]; cg=raw[omap[co][1]]; cb=raw[omap[co][2]]; }
        led_lock(0);
        led_color_t *back = led_get_colors(0);
        uint16_t cnt = led_get_count(0);
        for (uint16_t i = 0; i < cnt; i++) { back[i].r = 0; back[i].g = 0; back[i].b = 0; }
        for (int i = 0; i < count && pixel + i < cnt; i++)
            led_set_pixel(0, pixel + i, cr, cg, cb);
        led_unlock(0);
        led_refresh(0);
        return;
    }
    fx_state_t fx = { .mode = mode, .r = r, .g = g, .b = b, .speed = speed, .pixel = pixel, .count = count };
    effects_set(&fx);
}

/**
 * @brief Обработчик очистки всех LED-лент.
 *
 * Сбрасывает все пиксели в чёрный цвет (через @c effects_clear)
 * и применяет fallback-цвет из текущих настроек светильника.
 *
 * @note Не требует параметров; потокобезопасность обеспечивается
 *       вызываемыми функциями.
 */
void handler_led_clear(void) { effects_clear(); dmx_led_apply_fallback(); }

/**
 * @brief Обработчик изменения количества пикселей на LED-ленте.
 *
 * Устанавливает новое количество пикселей для указанной ленты,
 * пересчитывает DMX-маппинг и обновляет.hardware.
 *
 * @param strip  Индекс ленты (0-based).
 * @param count  Новое количество пикселей.
 *
 * @note После изменения количества пикселей автоматически
 *       вызывается @c dmx_led_recompute() для обновления
 *       привязки DMX-каналов к пикселям.
 */
void handler_led_count(uint8_t strip, uint16_t count) {
    led_set_count(strip, count);
    dmx_led_recompute();
    led_refresh(strip);
}

/**
 * @brief Обработчик включения/выключения реверса порядка пикселей.
 *
 * Инвертирует порядок пикселей на LED-ленте (полезно при
 * физическом расположении ленты «задом наперёд»).
 *
 * @param on  @c true — реверс включён, @c false — выключен.
 *
 * @note Потокобезопасность: защищено @c dmx_lock() / @c dmx_unlock().
 */
void handler_led_reverse(bool on)      { dmx_lock(); g_led_settings.reverse = on; dmx_unlock(); }

/**
 * @brief Обработчик выбора режима сканирования LED-лент.
 *
 * Переключает между последовательным и параллельным режимами
 * обновления нескольких LED-лент.
 *
 * @param sequential  @c true — последовательный режим,
 *                    @c false — параллельный режим.
 *
 * @note Глобальная переменная @c g_led_mode не защищена мьютексом,
 *       так как изменяется атомарно (одно присваивание).
 */
void handler_led_mode(bool sequential)  { g_led_mode = sequential ? LED_MODE_SEQUENTIAL : LED_MODE_PARALLEL; }

/**
 * @brief Обработчик установки сдвига (offset) для LED-ленты.
 *
 * Сдвигает начало привязки DMX-каналов на указанное количество
 * пикселей.
 *
 * @param shift  Величина сдвига в пикселях.
 *
 * @note Потокобезопасность: защищено @c dmx_lock() / @c dmx_unlock().
 */
void handler_led_shift(uint8_t shift)   { dmx_lock(); g_led_settings.shift = shift; dmx_unlock(); }

/**
 * @brief Обработчик включения/выключения интерполяции цвета.
 *
 * При включённой интерполяции цвет плавно изменяется между
 * DMX-кадрами, обеспечивая мягкое.transition.
 *
 * @param on  @c true — интерполяция включена,
 *            @c false — интерполяция выключена.
 *
 * @note Потокобезопасность: защищено @c dmx_lock() / @c dmx_unlock().
 */
void handler_led_interpolate(bool on)   { dmx_lock(); g_led_settings.interpolate = on; dmx_unlock(); }

/**
 * @brief Обработчик запуска DMX-теста на указанном порту/канале.
 *
 * Настраивает параметры DMX-передачи (порт, канал, цвет, режим)
 * и переключает устройство в режим @c DMX_MODE_TESTER.
 *
 * @param port     Номер DMX-порта.
 * @param channel  Номер DMX-канала (1–512).
 * @param r        Красный компонент (0–255).
 * @param g        Зелёный компонент (0–255).
 * @param b        Синий компонент (0–255).
 * @param fill     @c true — fill-режим (заполнение от канала),
 *                 @c false — point-режим (один канал).
 * @param count    Количество каналов для fill-режима.
 *
 * @note Потокобезопасность: защищено @c dmx_lock() / @c dmx_unlock().
 *       Вызов @c dmx_set_mode() erfolgt после разблокировки.
 */
void handler_dmx_test(uint8_t port, uint8_t channel, uint8_t r, uint8_t g, uint8_t b, bool fill, uint16_t count) {
    dmx_lock();
    g_dmx.tx_port = port; g_dmx.tx_channel = channel;
    g_dmx.tx_r = r; g_dmx.tx_g = g; g_dmx.tx_b = b;
    g_dmx.tx_mode = fill ? TX_MODE_FILL : TX_MODE_POINT;
    g_dmx.tx_count = count;
    dmx_unlock();
    dmx_set_mode(DMX_MODE_TESTER);
}

/**
 * @brief Обработчик сохранения настроек светильника.
 *
 * Обновляет порядок каналов RGB и параметры fallback-цвета
 * в глобальных настройках, затем применяет fallback-цвет
 * к LED-лентам.
 *
 * @param channel_order       Порядок каналов RGB
 *                            (см. @c CH_ORDER_RGB, @c CH_ORDER_GRB и т.д.).
 * @param fallback_r          Красный компонент fallback-цвета.
 * @param fallback_g          Зелёный компонент fallback-цвета.
 * @param fallback_b          Синий компонент fallback-цвета.
 * @param fallback_timeout_ms Тайм-аут fallback в миллисекундах
 *                            (время до возврата к fallback-цвету
 *                            при отсутствии DMX-данных).
 *
 * @note Потокобезопасность: защищено @c dmx_lock() / @c dmx_unlock().
 */
void handler_fixture_settings(uint8_t channel_order, uint8_t fb_r, uint8_t fb_g, uint8_t fb_b, uint16_t fb_timeout) {
    dmx_lock();
    g_led_settings.channel_order = channel_order;
    g_led_settings.fallback_r = fb_r; g_led_settings.fallback_g = fb_g; g_led_settings.fallback_b = fb_b;
    g_led_settings.fallback_timeout_ms = fb_timeout;
    dmx_unlock();
    dmx_led_apply_fallback();
}

/* ===== Init ===== */

/**
 * @brief Регистрация обработчиков команд в подсистеме DMX→LED.
 *
 * Функция создаёт структуру @c dmx_led_cbs_t, заполняя её
 * указателями на все реализованные функции-обработчики, и
 * передаёт её в @c dmx_led_init().  Это устанавливает связь:
 * web-сервер → @c dmx_led → обработчики.
 *
 * @note Вызывается один раз при старте системы, до начала
 *       обработки HTTP-запросов.  Потокобезопасность не требуется,
 *       так как функция выполняется в контексте инициализации.
 */
void app_handlers_init(void) {
    dmx_led_init(&(dmx_led_cbs_t){
        .on_mode             = handler_mode,
        .on_led_test         = handler_led_test,
        .on_led_clear        = handler_led_clear,
        .on_led_count        = handler_led_count,
        .on_led_reverse      = handler_led_reverse,
        .on_led_mode         = handler_led_mode,
        .on_led_shift        = handler_led_shift,
        .on_led_interpolate  = handler_led_interpolate,
        .on_dmx_test         = handler_dmx_test,
        .on_fixture_settings = handler_fixture_settings,
    });
}
