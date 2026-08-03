/**
 * @file settings_manager.c
 * @brief Реализация менеджера настроек (NVS)
 *
 * Модуль отвечает за сохранение и загрузку настроек LED-индикации
 * в энергонезависимую память (NVS — Non-Volatile Storage) ESP32.
 *
 * Настройки хранятся в пространстве имён "dmx_cfg" и включают:
 *   - Количество светодиодов (P0 и P1)
 *   - Направление порядка светодиодов (reverse)
 *   - Порядок каналов (RGB, GRB и т.д.)
 *   - Цвет fallback-режима (R, G, B)
 *   - Таймаут fallback-режима (мс)
 *   - Смещение LED (shift)
 *   - Режим интерполяции
 *   - Режим работы LED (mono, dual, sequential)
 *
 * @note При первом запуске NVS форматируется автоматически.
 * @note Сохранение защищено от быстрого износа (debounce 500 мс).
 */

#include "settings_manager.h"
#include "dmx/dmx_led.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

/** @brief Тег для логирования ESP-IDF */
static const char *TAG = "SETTINGS";

/** @brief Пространство имён NVS для хранения настроек */
static const char *NVS_NAMESPACE = "dmx_cfg";

/** @brief Интервал debounce для сохранения (500 мс = 500000 мкс) */
#define SETTINGS_DEBOUNCE_US 500000

/** @brief Временная метка последнего сохранения (мкс) */
static int64_t s_last_save_us = -SETTINGS_DEBOUNCE_US;

/**
 * @brief Загрузка настроек из NVS
 *
 * Читает сохранённые настройки из NVS и применяет их к глобальным
 * структурам. Каждый ключ проверяется на наличие ошибки чтения,
 * и только при успешном чтении значение применяется.
 *
 * Используемые NVS-ключи:
 *   - "led_cnt"    (u16)  — количество светодиодов для порта P0
 *   - "led_rev"    (u8)   — флаг реверса порядка светодиодов (0/1)
 *   - "ch_order"   (u8)   — порядок каналов (channel_order_t)
 *   - "fb_r"       (u8)   — красный канал fallback-цвета
 *   - "fb_g"       (u8)   — зелёный канал fallback-цвета
 *   - "fb_b"       (u8)   — синий канал fallback-цвета
 *   - "fb_timeout" (u16)  — таймаут fallback-режима (мс, 10..5000)
 *   - "led_shft"   (u16)  — смещение LED (shift)
 *   - "interpolate"(i8)   — флаг интерполяции (0/1)
 *   - "led_mode"   (u8)   — режим работы LED (led_mode_t)
 *   - "led_cnt2"   (u16)  — количество светодиодов для порта P1
 *
 * @note Если ключ не найден или значение некорректно — используется
 *       значение по умолчанию (не модифицируется).
 */
static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8_val;
    uint16_t u16_val;

    /* Количество светодиодов для порта P0 (1..LED_STRIP_MAX_LEDS) */
    if (nvs_get_u16(h, "led_cnt", &u16_val) == ESP_OK)
        if (u16_val >= 1 && u16_val <= LED_STRIP_MAX_LEDS) led_set_count(0, u16_val);

    /* Флаг реверса порядка светодиодов */
    if (nvs_get_u8(h, "led_rev", &u8_val) == ESP_OK)
        g_led_settings.reverse = (u8_val != 0);

    /* Порядок каналов (RGB, GRB, BRG и т.д.) */
    if (nvs_get_u8(h, "ch_order", &u8_val) == ESP_OK)
        if (u8_val < CH_ORDER_COUNT) g_led_settings.channel_order = (channel_order_t)u8_val;

    /* Цвет fallback-режима (R, G, B) */
    if (nvs_get_u8(h, "fb_r", &u8_val) == ESP_OK) g_led_settings.fallback_r = u8_val;
    if (nvs_get_u8(h, "fb_g", &u8_val) == ESP_OK) g_led_settings.fallback_g = u8_val;
    if (nvs_get_u8(h, "fb_b", &u8_val) == ESP_OK) g_led_settings.fallback_b = u8_val;

    /* Таймаут fallback-режима (10..5000 мс) */
    if (nvs_get_u16(h, "fb_timeout", &u16_val) == ESP_OK)
        if (u16_val >= 10 && u16_val <= 5000) g_led_settings.fallback_timeout_ms = u16_val;

    /* Смещение LED (shift) */
    if (nvs_get_u16(h, "led_shft", &u16_val) == ESP_OK)
        if (u16_val < LED_STRIP_MAX_LEDS) g_led_settings.shift = u16_val;

    /* Флаг интерполяции */
    int8_t i8_val;
    if (nvs_get_i8(h, "interpolate", &i8_val) == ESP_OK)
        g_led_settings.interpolate = (i8_val != 0);

    /* Режим работы LED */
    uint8_t u8_led_mode;
    if (nvs_get_u8(h, "led_mode", &u8_led_mode) == ESP_OK)
        if (u8_led_mode <= LED_MODE_SEQUENTIAL) g_led_mode = (led_mode_t)u8_led_mode;

    /* Количество светодиодов для порта P1 */
    if (nvs_get_u16(h, "led_cnt2", &u16_val) == ESP_OK)
        if (u16_val <= LED_STRIP_MAX_LEDS) led_set_count(1, u16_val);

    nvs_close(h);
}

/**
 * @brief Инициализация NVS Flash и загрузка настроек
 *
 * Выполняет:
 *   1. Инициализацию NVS Flash (nvs_flash_init)
 *   2. При ошибке ESP_ERR_NVS_NO_FREE_PAGES или
 *      ESP_ERR_NVS_NEW_VERSION_FOUND — форматирование NVS
 *      (nvs_flash_erase) и повторная инициализация
 *   3. Загрузку настроек из NVS через settings_load()
 *
 * @note При первом запуске (или повреждении NVS) раздел форматируется,
 *       и используются значения по умолчанию из структур.
 * @note После форматирования все ранее сохранённые настройки теряются.
 */
void settings_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_load();
}

/**
 * @brief Сохранение текущих настроек в NVS
 *
 * Сохраняет текущие значения всех настроек в NVS Flash.
 *
 * Защита от быстрого износа (debounce):
 *   Если с момента последнего сохранения прошло менее 500 мс,
 *   сохранение пропускается. Это предотвращает чрезмерное использование
 *   NVS Flash при частых вызовах settings_save().
 *
 * Формат сохранения (все значения в big-endian для NVS):
 *   - "led_cnt"    (u16)  — количество светодиодов для порта P0
 *   - "led_rev"    (u8)   — флаг реверса (0/1)
 *   - "ch_order"   (u8)   — порядок каналов
 *   - "fb_r"       (u8)   — красный канал fallback-цвета
 *   - "fb_g"       (u8)   — зелёный канал fallback-цвета
 *   - "fb_b"       (u8)   — синий канал fallback-цвета
 *   - "fb_timeout" (u16)  — таймаут fallback (мс)
 *   - "led_shft"   (u16)  — смещение LED
 *   - "interpolate"(i8)   — флаг интерполяции (0/1)
 *   - "led_mode"   (u8)   — режим работы LED
 *   - "led_cnt2"   (u16)  — количество светодиодов для порта P1
 *
 * @note Функция безопасна для вызова из любого контекста (ISR не рекомендуется).
 * @note NVS автоматически коммитится после записи всех ключей.
 */
void settings_save(void) {
    /* Debounce: пропуск сохранения если прошло менее 500 мс */
    int64_t now = esp_timer_get_time();
    if ((now - s_last_save_us) < SETTINGS_DEBOUNCE_US) return;
    s_last_save_us = now;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }

    /* Сохранение всех настроек в NVS */
    nvs_set_u16(h, "led_cnt", led_get_count(0));
    nvs_set_u8(h, "led_rev", g_led_settings.reverse ? 1 : 0);
    nvs_set_u8(h, "ch_order", (uint8_t)g_led_settings.channel_order);
    nvs_set_u8(h, "fb_r", g_led_settings.fallback_r);
    nvs_set_u8(h, "fb_g", g_led_settings.fallback_g);
    nvs_set_u8(h, "fb_b", g_led_settings.fallback_b);
    nvs_set_u16(h, "fb_timeout", g_led_settings.fallback_timeout_ms);
    nvs_set_u16(h, "led_shft", g_led_settings.shift);
    int8_t i8_val = g_led_settings.interpolate ? 1 : 0;
    nvs_set_i8(h, "interpolate", i8_val);
    nvs_set_u8(h, "led_mode", (uint8_t)g_led_mode);
    nvs_set_u16(h, "led_cnt2", led_get_count(1));

    /* Фиксация изменений в NVS */
    nvs_commit(h);
    nvs_close(h);
}
