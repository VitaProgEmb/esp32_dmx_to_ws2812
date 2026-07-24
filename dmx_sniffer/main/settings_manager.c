/**
 * @file settings_manager.c
 * @brief Сохранение/загрузка настроек в NVS (Non-Volatile Storage)
 *
 * Хранит ТОЛЬКО постоянные настройки (переживают перезагрузку):
 *   - Количество WS2812-LED
 *   - Реверс порядка LED
 *   - Порядок каналов (RGB/GRB/...)
 *   - Fallback цвет и таймаут
 *   - Флаг интерполяции
 *
 * НЕ сохраняет (тестовые параметры):
 *   - Тестовый цвет (tx_r/g/b)
 *   - Тестовый адрес (tx_channel)
 *   - Режим тестера (tx_mode)
 *   - FPS, скорость передачи, длина пакета
 *
 * Debounce:
 *   - settings_save() вызывается при каждом API-запросе,
 *     но реально пишет в NVS не чаще 1 раза в 500мс
 *   - Это защищает flash-паметь от исчерпания при быстром
 *     перемещении слайдеров (~30 вызовов/сек → 2 записи/сек)
 *
 * Первый вызов:
 *   - s_last_save_us инициализирован в -SETTINGS_DEBOUNCE_US,
 *     поэтому первый save всегда проходит (даже в первые 500мс старта)
 */

#include "settings_manager.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "SETTINGS";
/** Пространство имён NVS для хранения настроек */
static const char *NVS_NAMESPACE = "dmx_cfg";

/** Внешняя ссылка на общее количество LED (нужно для загрузки из NVS) */
extern uint16_t g_total_leds;

/** Интервал debounce для записи в NVS (500мс = 500000мкс) */
#define SETTINGS_DEBOUNCE_US 500000

/**
 * Время последней записи в NVS (мкс).
 * Инициализирован в -SETTINGS_DEBOUNCE_US, чтобы первый вызов settings_save()
 * гарантированно прошёл (even если вызван в первые 500мс после старта).
 */
static int64_t s_last_save_us = -SETTINGS_DEBOUNCE_US;

/**
 * Загрузка настроек из NVS в g_dmx.
 *
 * Вызывается ОДИН раз в app_main() перед dmx_init().
 * Читает каждое значение отдельно — если ключа нет (первый запуск),
 * остаются дефолтные значения из g_dmx.
 *
 * Каждое значение проверяется на валидность диапазона
 * (защита от повреждённых данных NVS).
 */
void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    uint8_t u8_val;
    uint16_t u16_val;

    if (nvs_get_u16(h, "led_cnt", &u16_val) == ESP_OK) {
        if (u16_val >= 1 && u16_val <= LED_STRIP_MAX_LEDS) {
            g_led_strip.count = u16_val;
            g_total_leds = u16_val;
        }
    }
    if (nvs_get_u8(h, "led_rev", &u8_val) == ESP_OK) {
        g_dmx.led_reverse = (u8_val != 0);
    }
    if (nvs_get_u8(h, "ch_order", &u8_val) == ESP_OK) {
        if (u8_val < CH_ORDER_COUNT) g_dmx.channel_order = (channel_order_t)u8_val;
    }
    if (nvs_get_u8(h, "fb_r", &u8_val) == ESP_OK) {
        g_dmx.fallback_r = u8_val;
    }
    if (nvs_get_u8(h, "fb_g", &u8_val) == ESP_OK) {
        g_dmx.fallback_g = u8_val;
    }
    if (nvs_get_u8(h, "fb_b", &u8_val) == ESP_OK) {
        g_dmx.fallback_b = u8_val;
    }
    if (nvs_get_u16(h, "fb_timeout", &u16_val) == ESP_OK) {
        if (u16_val >= 10 && u16_val <= 5000) g_dmx.fallback_timeout_ms = u16_val;
    }
    if (nvs_get_u16(h, "led_shft", &u16_val) == ESP_OK) {
        if (u16_val < LED_STRIP_MAX_LEDS) g_dmx.led_shift = u16_val;
    }
    int8_t i8_val;
    if (nvs_get_i8(h, "interpolate", &i8_val) == ESP_OK) {
        g_dmx.interpolate = (i8_val != 0);
    }
    uint8_t u8_led_mode;
    if (nvs_get_u8(h, "led_mode", &u8_led_mode) == ESP_OK) {
        if (u8_led_mode <= LED_MODE_SEQUENTIAL) g_led_mode = (led_mode_t)u8_led_mode;
    }
    if (nvs_get_u16(h, "led_cnt2", &u16_val) == ESP_OK) {
        if (u16_val <= LED_STRIP_MAX_LEDS) g_led_strip2.count = u16_val;
    }

    nvs_close(h);
}

/**
 * Сохранение текущих настроек из g_dmx в NVS.
 *
 * Защита от flash-износа (debounce):
 *   - Если прошло < 500мс с последней записи — пропускаем
 *   - Защищает flash-память при быстрых изменениях через UI
 *     (слайдеры генерируют ~30 вызовов/сек, debounce снижает до 2/сек)
 *
 * Примечание: НЕ использует dmx_lock() т.к. вызывается из HTTP task,
 * который не должен блокировать надолго. Чтение полей g_dmx
 * происходит через volatile — допустимо для простых типов.
 */
void settings_save(void) {
    int64_t now = esp_timer_get_time();
    if ((now - s_last_save_us) < SETTINGS_DEBOUNCE_US) return;
    s_last_save_us = now;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }

    nvs_set_u16(h, "led_cnt", g_led_strip.count);
    nvs_set_u8(h, "led_rev", g_dmx.led_reverse ? 1 : 0);
    nvs_set_u8(h, "ch_order", (uint8_t)g_dmx.channel_order);
    nvs_set_u8(h, "fb_r", g_dmx.fallback_r);
    nvs_set_u8(h, "fb_g", g_dmx.fallback_g);
    nvs_set_u8(h, "fb_b", g_dmx.fallback_b);
    nvs_set_u16(h, "fb_timeout", g_dmx.fallback_timeout_ms);
    nvs_set_u16(h, "led_shft", g_dmx.led_shift);
    // Save interpolation flag (bool stored as int8)
    int8_t i8_val = g_dmx.interpolate ? 1 : 0;
    nvs_set_i8(h, "interpolate", i8_val);
    nvs_set_u8(h, "led_mode", (uint8_t)g_led_mode);
    nvs_set_u16(h, "led_cnt2", g_led_strip2.count);

    nvs_commit(h);
    nvs_close(h);
}
