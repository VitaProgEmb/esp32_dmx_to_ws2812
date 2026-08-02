#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "settings.h"

/* ===== Типы ===== */

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

/* ===== API ===== */

/**
 * @brief Инициализация ленты
 * @param strip Индекс ленты (0 или 1)
 * @param gpio  Номер GPIO
 * @param count Количество LED (макс LED_STRIP_MAX_LEDS)
 */
void led_init(uint8_t strip, uint8_t gpio, uint16_t count);

/**
 * @brief Установить цвет одного пикселя (в back-буфер)
 * @param strip Индекс ленты (0 или 1)
 * @param idx   Индекс LED (0-based)
 * @param r, g, b Цвет
 */
void led_set_pixel(uint8_t strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Отправить back-буфер в RMT (swap + refresh)
 * @param strip Индекс ленты (0 или 1)
 */
void led_show(uint8_t strip);

/**
 * @brief Изменить количество LED на лету
 * @param strip Индекс ленты (0 или 1)
 * @param count Новое количество
 */
void led_set_count(uint8_t strip, uint16_t count);

/**
 * @brief Установить одинаковый цвет на все LED
 * @param strip Индекс ленты (0 или 1)
 */
void led_fill(uint8_t strip, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Очистить все LED + show
 * @param strip Индекс ленты (0 или 1)
 */
void led_clear(uint8_t strip);

/**
 * @brief Освобождение ресурсов
 */
void led_deinit(void);

/* ===== Внутренний API (для dmx.c — прямой доступ к буферу) ===== */

/* Блокировка/разблокировка буфера (нужна для прямой записи из dmx.c) */
void led_lock(uint8_t strip);
void led_unlock(uint8_t strip);

/* Прямой доступ к цветовому буферу (для dmx.c — interleave/ effects) */
led_color_t *led_get_colors(uint8_t strip);
uint16_t     led_get_count(uint8_t strip);

/* Поменять banks местами (для dmx.c — двойная буферизация) */
void led_swap_banks(uint8_t strip);

/* Refresh без swap (для dmx.c — когда цвета уже в active bank) */
void led_refresh(uint8_t strip);

/* Свойства */
extern volatile uint16_t g_total_leds;
extern volatile led_mode_t g_led_mode;

void led_set_reverse(uint8_t strip, bool on);
void led_set_shift(uint8_t strip, uint16_t shift);
