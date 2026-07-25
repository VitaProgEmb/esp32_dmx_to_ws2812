/**
 * @file main.c
 * @brief Тест DMX RX модуля — приём данных + логи
 *
 * Тестирует:
 *   - Два DMX порта (GPIO15, GPIO16)
 *   - Event notification (DMX_EVT_FRAME0/1)
 *   - Raw буфер (g_raw_frames)
 *   - Включение/отключение портов
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_ota_ops.h"

#include "settings.h"
#include "dmx/dmx_bus.h"
#include "dmx/dmx_rx.h"
#include "wifi_ap.h"

extern volatile uint32_t g_isr_count[2];
extern volatile uint32_t g_break_count[2];
#include "udp_test.h"

static const char *TAG = "MAIN";

/** Счётчики кадров для мониторинга */
static volatile uint32_t s_frame_count[2] = {0, 0};
static volatile uint32_t s_last_frame_tick[2] = {0, 0};

/**
 * @brief Мониторинг DMX — ждёт событий и логирует
 *
 * Выводит:
 *   - Количество кадров с каждого порта
 *   - Первые 12 каналов (4 прибора RGB)
 *   - FPS
 */
static void dmx_monitor_task(void *arg) {
    ESP_LOGI(TAG, "=== DMX Monitor Started ===");
    ESP_LOGI(TAG, "Waiting for DMX data on GPIO%d (port0) and GPIO%d (port1)...",
             DMX_GPIO_RX1, DMX_GPIO_RX2);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            g_dmx_events,
            DMX_EVT_FRAME0 | DMX_EVT_FRAME1,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(1000)
        );

        if (bits & DMX_EVT_FRAME0) s_frame_count[0]++;
        if (bits & DMX_EVT_FRAME1) s_frame_count[1]++;

        if (bits == 0) {
            ESP_LOGD(TAG, "No DMX: isr0=%lu brk0=%lu isr1=%lu brk1=%lu",
                     (unsigned long)g_isr_count[0], (unsigned long)g_break_count[0],
                     (unsigned long)g_isr_count[1], (unsigned long)g_break_count[1]);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * @brief Статусный LED — мигает при WiFi, горит при DMX
 */
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
            gpio_set_level(STATUS_LED_GPIO, 0);  /* WiFi ON → LED ON */
        } else {
            gpio_set_level(STATUS_LED_GPIO, 1);  /* WiFi OFF → LED OFF */
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== DMX RX Module Test ===");
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    /* --- NVS --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* --- WiFi --- */
    wifi_init();

    /* --- DMX RX: порт 0 (GPIO15) + порт 1 (GPIO16) --- */
    dmx_rx_init(0, &(dmx_rx_cfg_t){
        .rx_pin  = DMX_GPIO_RX1,
        .tx_pin  = DMX_GPIO_TX1,
        .dir_pin = -1,
        .uart_num = UART_NUM_1,
    });

    dmx_rx_init(1, &(dmx_rx_cfg_t){
        .rx_pin  = DMX_GPIO_RX2,
        .tx_pin  = DMX_GPIO_TX2,
        .dir_pin = DMX_GPIO_DIR2,
        .uart_num = UART_NUM_2,
    });

    dmx_rx_start();

    ESP_LOGI(TAG, "DMX RX started. Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    /* --- Задачи --- */
    xTaskCreatePinnedToCore(dmx_monitor_task, "dmx_mon", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 1, NULL, 0);

    /* UDP debug socket */
    udp_test_init();

    ESP_LOGI(TAG, "=== Init complete, monitoring DMX... ===");

    /* Подтверждаем что прошивка рабочая — отменяем автоматический rollback */
    esp_ota_mark_app_valid_cancel_rollback();

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        ESP_LOGI(TAG, "OTA state: %s",
                 state == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY" :
                 state == ESP_OTA_IMG_VALID ? "VALID" :
                 state == ESP_OTA_IMG_INVALID ? "INVALID" :
                 state == ESP_OTA_IMG_ABORTED ? "ABORTED" : "UNKNOWN");
    }
}
