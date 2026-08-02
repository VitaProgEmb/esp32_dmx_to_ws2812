/**
 * @file settings_manager.c
 * @brief Сохранение/загрузка настроек в NVS (Non-Volatile Storage)
 */

#include "settings_manager.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "SETTINGS";
static const char *NVS_NAMESPACE = "dmx_cfg";

#define SETTINGS_DEBOUNCE_US 500000
static int64_t s_last_save_us = -SETTINGS_DEBOUNCE_US;

static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8_val;
    uint16_t u16_val;

    if (nvs_get_u16(h, "led_cnt", &u16_val) == ESP_OK)
        if (u16_val >= 1 && u16_val <= LED_STRIP_MAX_LEDS) led_set_count(0, u16_val);
    if (nvs_get_u8(h, "led_rev", &u8_val) == ESP_OK)
        g_led_settings.reverse = (u8_val != 0);
    if (nvs_get_u8(h, "ch_order", &u8_val) == ESP_OK)
        if (u8_val < CH_ORDER_COUNT) g_led_settings.channel_order = (channel_order_t)u8_val;
    if (nvs_get_u8(h, "fb_r", &u8_val) == ESP_OK) g_led_settings.fallback_r = u8_val;
    if (nvs_get_u8(h, "fb_g", &u8_val) == ESP_OK) g_led_settings.fallback_g = u8_val;
    if (nvs_get_u8(h, "fb_b", &u8_val) == ESP_OK) g_led_settings.fallback_b = u8_val;
    if (nvs_get_u16(h, "fb_timeout", &u16_val) == ESP_OK)
        if (u16_val >= 10 && u16_val <= 5000) g_led_settings.fallback_timeout_ms = u16_val;
    if (nvs_get_u16(h, "led_shft", &u16_val) == ESP_OK)
        if (u16_val < LED_STRIP_MAX_LEDS) g_dmx.led_shift = u16_val;
    int8_t i8_val;
    if (nvs_get_i8(h, "interpolate", &i8_val) == ESP_OK)
        g_dmx.interpolate = (i8_val != 0);
    uint8_t u8_led_mode;
    if (nvs_get_u8(h, "led_mode", &u8_led_mode) == ESP_OK)
        if (u8_led_mode <= LED_MODE_SEQUENTIAL) g_led_mode = (led_mode_t)u8_led_mode;
    if (nvs_get_u16(h, "led_cnt2", &u16_val) == ESP_OK)
        if (u16_val <= LED_STRIP_MAX_LEDS) led_set_count(1, u16_val);

    nvs_close(h);
}

void settings_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_load();
}

void settings_save(void) {
    int64_t now = esp_timer_get_time();
    if ((now - s_last_save_us) < SETTINGS_DEBOUNCE_US) return;
    s_last_save_us = now;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }

    nvs_set_u16(h, "led_cnt", led_get_count(0));
    nvs_set_u8(h, "led_rev", g_led_settings.reverse ? 1 : 0);
    nvs_set_u8(h, "ch_order", (uint8_t)g_led_settings.channel_order);
    nvs_set_u8(h, "fb_r", g_led_settings.fallback_r);
    nvs_set_u8(h, "fb_g", g_led_settings.fallback_g);
    nvs_set_u8(h, "fb_b", g_led_settings.fallback_b);
    nvs_set_u16(h, "fb_timeout", g_led_settings.fallback_timeout_ms);
    nvs_set_u16(h, "led_shft", g_dmx.led_shift);
    int8_t i8_val = g_dmx.interpolate ? 1 : 0;
    nvs_set_i8(h, "interpolate", i8_val);
    nvs_set_u8(h, "led_mode", (uint8_t)g_led_mode);
    nvs_set_u16(h, "led_cnt2", led_get_count(1));

    nvs_commit(h);
    nvs_close(h);
}
