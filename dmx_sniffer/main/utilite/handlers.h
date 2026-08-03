/**
 * @file handlers.h
 * @brief Интерфейс между web-сервером и мостом DMX→LED
 *
 * Заголовок определяет структуру回调ов @c dmx_led_cbs_t, через которую
 * модуль @b web_server передаёт команды пользователя в подсистему
 * DMX→LED (@c dmx_led), а также набор функций-обработчиков,
 * реализующих каждую такую команду.
 *
 * Все функции-обработчики вызываются из контекста web-сервера
 * (HTTP-запрос) и должны быть потокобезопасны.  Глобальные
 * настройки защищаются мьютексом @c dmx_lock() / @c dmx_unlock().
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Набор функций обратного вызова для связи web-сервера с DMX→LED.
 *
 * Структура заполняется в@app_handlers_init и передаётся в
 * @c dmx_led_init().  Каждое поле — указатель на функцию-обработчик,
 * которую подсистема @c dmx_led вызовет при получении
 * соответствующей команды от web-сервера.
 */
typedef struct {
    /**
     * @brief Обработчик смены режима работы устройства.
     * @param mode  Идентификатор режима:
     *              - 0 — @c DMX_MODE_SNIFFER (перехват DMX),
     *              - 1 — @c DMX_MODE_TESTER  (тест DMX-передатчика),
     *              - 2 — @c DMX_MODE_PATCH   (патчинг каналов).
     */
    void (*on_mode)(uint8_t mode);

    /**
     * @brief Обработчик запуска тестового эффекта на LED-ленте.
     * @param mode   Тип эффекта (1 — статический цвет, остальные — динамические).
     * @param r      Красный компонент цвета (0–255).
     * @param g      Зелёный компонент цвета (0–255).
     * @param b      Синий компонент цвета (0–255).
     * @param speed  Скорость эффекта (0–255).
     * @param pixel  Номер начального пикселя.
     * @param count  Количество затрагиваемых пикселей.
     */
    void (*on_led_test)(uint8_t mode, uint8_t r, uint8_t g, uint8_t b,
                        uint8_t speed, uint16_t pixel, uint16_t count);

    /**
     * @brief Обработчик очистки всех LED-лент (сброс в чёрный цвет).
     *
     * После очистки применяется fallback-цвет из настроек.
     */
    void (*on_led_clear)(void);

    /**
     * @brief Обработчик изменения количества пикселей на указанной ленте.
     * @param strip  Индекс LED-ленты (0-based).
     * @param count  Новое количество пикселей.
     */
    void (*on_led_count)(uint8_t strip, uint16_t count);

    /**
     * @brief Обработчик включения/выключения реверса порядка пикселей.
     * @param on  @c true — порядок пикселей инвертируется,
     *            @c false — порядок стандартный.
     */
    void (*on_led_reverse)(bool on);

    /**
     * @brief Обработчик выбора режима сканирования LED-лент.
     * @param sequential  @c true — последовательное сканирование
     *                    (@c LED_MODE_SEQUENTIAL),
     *                    @c false — параллельное (@c LED_MODE_PARALLEL).
     */
    void (*on_led_mode)(bool sequential);

    /**
     * @brief Обработчик установки сдвига (offset) для LED-ленты.
     * @param shift  Значение сдвига в пикселях.
     */
    void (*on_led_shift)(uint8_t shift);

    /**
     * @brief Обработчик включения/выключения интерполяции цвета.
     * @param on  @c true — интерполяция включена,
     *            @c false — интерполяция выключена.
     */
    void (*on_led_interpolate)(bool on);

    /**
     * @brief Обработчик запуска DMX-теста на указанном порту/канале.
     * @param port     Номер DMX-порта.
     * @param channel  Номер DMX-канала (1–512).
     * @param r        Красный компонент (0–255).
     * @param g        Зелёный компонент (0–255).
     * @param b        Синий компонент (0–255).
     * @param fill     @c true — заполнение (fill) от начального канала,
     *                 @c false — точечный (point) режим.
     * @param count    Количество каналов для заполнения.
     */
    void (*on_dmx_test)(uint8_t port, uint8_t channel,
                        uint8_t r, uint8_t g, uint8_t b,
                        bool fill, uint16_t count);

    /**
     * @brief Обработчик сохранения настроек светильника.
     * @param channel_order       Порядок каналов RGB
     *                            (см. @c CH_ORDER_*).
     * @param fallback_r          Красный компонент fallback-цвета.
     * @param fallback_g          Зелёный компонент fallback-цвета.
     * @param fallback_b          Синий компонент fallback-цвета.
     * @param fallback_timeout_ms Тайм-аут fallback в миллисекундах.
     */
    void (*on_fixture_settings)(uint8_t channel_order,
            uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b,
            uint16_t fallback_timeout_ms);
} dmx_led_cbs_t;

/**
 * @brief Регистрация обработчиков команд в подсистеме DMX→LED.
 *
 * Функция заполняет структуру @c dmx_led_cbs_t указателями на
 * функции-обработчики и передаёт её в @c dmx_led_init(),
 * устанавливая связь между web-сервером и мостом DMX→LED.
 *
 * @note Вызывается один раз при старте системы.
 */
void app_handlers_init(void);

/**
 * @brief Обработчик смены режима работы устройства.
 * @param mode  Идентификатор режима (0 — sniffer, 1 — tester, 2 — patch).
 */
void handler_mode(uint8_t mode);

/**
 * @brief Обработчик запуска тестового эффекта на LED-ленте.
 * @param mode   Тип эффекта (1 — статический, иные — динамические).
 * @param r      Красный компонент (0–255).
 * @param g      Зелёный компонент (0–255).
 * @param b      Синий компонент (0–255).
 * @param speed  Скорость эффекта (0–255).
 * @param pixel  Номер начального пикселя.
 * @param count  Количество пикселей.
 */
void handler_led_test(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint8_t speed, uint16_t pixel, uint16_t count);

/**
 * @brief Обработчик очистки всех LED-лент.
 *
 * Сбрасывает все пиксели в чёрный цвет и применяет fallback-цвет.
 */
void handler_led_clear(void);

/**
 * @brief Обработчик изменения количества пикселей на ленте.
 * @param strip  Индекс ленты (0-based).
 * @param count  Новое количество пикселей.
 */
void handler_led_count(uint8_t strip, uint16_t count);

/**
 * @brief Обработчик включения/выключения реверса порядка пикселей.
 * @param on  @c true — реверс включён, @c false — выключен.
 */
void handler_led_reverse(bool on);

/**
 * @brief Обработчик выбора режима сканирования LED-лент.
 * @param sequential  @c true — последовательный, @c false — параллельный.
 */
void handler_led_mode(bool sequential);

/**
 * @brief Обработчик установки сдвига (offset) для LED-ленты.
 * @param shift  Величина сдвига в пикселях.
 */
void handler_led_shift(uint8_t shift);

/**
 * @brief Обработчик включения/выключения интерполяции цвета.
 * @param on  @c true — интерполяция включена, @c false — выключена.
 */
void handler_led_interpolate(bool on);

/**
 * @brief Обработчик запуска DMX-теста.
 * @param port     Номер DMX-порта.
 * @param channel  Номер DMX-канала (1–512).
 * @param r        Красный компонент (0–255).
 * @param g        Зелёный компонент (0–255).
 * @param b        Синий компонент (0–255).
 * @param fill     @c true — fill-режим, @c false — point-режим.
 * @param count    Количество каналов.
 */
void handler_dmx_test(uint8_t port, uint8_t channel, uint8_t r, uint8_t g, uint8_t b, bool fill, uint16_t count);

/**
 * @brief Обработчик сохранения настроек светильника.
 * @param channel_order       Порядок каналов RGB (см. @c CH_ORDER_*).
 * @param fallback_r          Красный компонент fallback-цвета.
 * @param fallback_g          Зелёный компонент fallback-цвета.
 * @param fallback_b          Синий компонент fallback-цвета.
 * @param fallback_timeout_ms Тайм-аут fallback в миллисекундах.
 */
void handler_fixture_settings(uint8_t channel_order, uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b, uint16_t fallback_timeout_ms);
