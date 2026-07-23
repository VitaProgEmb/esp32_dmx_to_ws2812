/**
 * @file main.c
 * @brief Точка входа в программу, инициализация всех подсистем устройства
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "settings.h"
#include "dmx.h"
#include "led_strip.h"
#include "patch_manager.h"
#include "wifi_ap.h"
#include "web_server.h"
#include "settings_manager.h"

static const char *TAG = "MAIN";

#define LED_TEST_OFF     0
#define LED_TEST_STATIC  1
#define LED_TEST_RAINBOW 2

static void led_rainbow_task(void *arg) {
    uint32_t step = 0;
    while (1) {
        if (g_led_test_mode == LED_TEST_RAINBOW) {
            int count = g_led_test_count;
            if (count > g_led_strip.count) {
                count = g_led_strip.count;
            }

            led_strip_lock();
            for (int i = 0; i < count; i++) {
                uint8_t r, g, b;
                int hue = (step + i * 360 / count) % 360;
                int region = hue / 60;
                int f = hue % 60;
                uint8_t q = 255 - (255 * f / 60);

                switch (region) {
                    case 0:  r = 255; g = q;   b = 0;   break;
                    case 1:  r = q;   g = 255; b = 0;   break;
                    case 2:  r = 0;   g = 255; b = 255 - q; break;
                    case 3:  r = 0;   g = q;   b = 255; break;
                    case 4:  r = 255 - q; g = 0;   b = 255; break;
                    default: r = 255; g = 0;   b = q;   break;
                }
                g_led_strip.colors[i].r = r;
                g_led_strip.colors[i].g = g;
                g_led_strip.colors[i].b = b;
            }
            led_strip_unlock();

            led_strip_refresh();
            step = (step + 3) % 360;
            vTaskDelay(pdMS_TO_TICKS(25));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void boot_button_task(void *arg) {
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    bool was_pressed = false;
    int press_duration = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));

        bool pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);

        if (pressed && !was_pressed) {
            was_pressed = true;
            press_duration = 0;
        } else if (pressed && was_pressed) {
            press_duration++;
        } else if (!pressed && was_pressed) {
            if (press_duration >= 2) {
                wifi_toggle();
            }
            was_pressed = false;
            press_duration = 0;
        }
    }
}

static void status_led_task(void *arg) {
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);

    while (1) {
        if (wifi_is_on()) {
            gpio_set_level(STATUS_LED_GPIO, 0);
        } else {
            gpio_set_level(STATUS_LED_GPIO, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void) {

    gpio_config_t dir_cfg = {
        .pin_bit_mask = (1ULL << DMX_GPIO_DIR2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dir_cfg);
    gpio_set_level(DMX_GPIO_DIR2, 0);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    settings_load();
    if (g_led_strip.count < 1) g_led_strip.count = LED_DEFAULT_COUNT;
    g_total_leds = g_led_strip.count;

    led_strip_init(g_led_strip.count);
    led_strip_clear();

    patch_init();

    dmx_init();
    dmx_recompute_lookups();

    wifi_init();
    dmx_hal_start();

    xTaskCreatePinnedToCore(led_rainbow_task, "led_rainbow", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(boot_button_task, "boot_btn", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 1, NULL, 0);


    

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI("MAIN", "Free heap: %lu, min: %lu", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    web_server_init();

    dmx_start_rx_task();
    dmx_start_tx_task();
}
