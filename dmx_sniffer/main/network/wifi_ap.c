/**
 * @file wifi_ap.c
 * @brief Реализация модуля Wi-Fi (STA + AP с toggle)
 *
 * Управляет Wi-Fi подключением, статусной LED и HTTP-сервером.
 * Поддерживает два режима работы, выбираемые при компиляции:
 * - WIFI_MODE_AP_RELEASE=1: режим AP (своя точка доступа 192.168.4.1)
 * - WIFI_MODE_AP_RELEASE=0: режим STA (подключение к домашней сети)
 *
 * При STA-режиме автоматическое переподключение при обрыве связи.
 * Статусная LED мигает: включена если Wi-Fi активен, выключена если нет.
 */

#include "wifi_ap.h"
#include "web_server.h"
#include "settings.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "mdns.h"
#include <string.h>

#if USE_W5500
#include "w5500.h"
#endif

/** @brief Тег логирования ESP-IDF для модуля Wi-Fi */
static const char *TAG = "WIFI";

/** @brief Флаг текущего состояния Wi-Fi (true=включён, false=выключен) */
static bool s_wifi_enabled = false;

/**
 * @brief Обработчик событий Wi-Fi и IP
 *
 * Обрабатывает WIFI_EVENT и IP_EVENT (только STA):
 * - STA_START → автоматическое подключение
 * - STA_DISCONNECTED → повторная попытка (если WiFi включён)
 * - STA_GOT_IP → логирование IP-адреса
 *
 * @note ETH_EVENT и IP_EVENT_ETH_GOT_IP обрабатываются в w5500.c
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_wifi_enabled) esp_wifi_connect();
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *ev = data;
            ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        }
    }
}

/**
 * @brief Инициализирует Wi-Fi драйвер и настраивает режим сети
 *
 * Алгоритм:
 * 1. Инициализирует стек TCP/IP (esp_netif_init)
 * 2. Создаёт event loop по умолчанию
 * 3. Инициализирует Wi-Fi драйвер с дефолтными настройками
 * 4. Регистрирует обработчики событий WIFI_EVENT и IP_EVENT
 * 5. В зависимости от WIFI_MODE_AP_RELEASE:
 *    - AP: создаёт netif AP, настраивает IP 192.168.4.1, DHCP-сервер,
 *      SSID/пароль из Kconfig, запускает AP
 *    - STA: создаёт netif STA, настраивает SSID/пароль из Kconfig,
 *      порог аутентификации WPA2, запускает STA
 *
 * @note После инициализации WiFi остаётся выключенным (s_wifi_enabled=false).
 *       Для запуска необходимо вызвать wifi_start().
 *
 * @return ESP_OK при успешной инициализации
 */
static esp_err_t wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

#if WIFI_MODE_AP_RELEASE
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = strlen(WIFI_AP_PASS) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
#else
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_cfg = {
        .sta = {
            .ssid     = WIFI_STA_SSID,
            .password = WIFI_STA_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
#endif

    s_wifi_enabled = false;
    return ESP_OK;
}

/**
 * @brief Переключает Wi-Fi: вкл→выкл, выкл→вкл
 *
 * Простейший toggle-механизм: проверяет текущий флаг
 * и вызывает соответствующую функцию wifi_stop() или wifi_start().
 *
 * @return ESP_OK при успешном переключении
 */
esp_err_t wifi_toggle(void) {
    if (s_wifi_enabled) {
        return wifi_stop();
    } else {
        return wifi_start();
    }
}

/**
 * @brief Останавливает Wi-Fi
 *
 * 1. Сбрасывает флаг s_wifi_enabled
 * 2. Отключается от текущей точки доступа (esp_wifi_disconnect)
 * 3. Полностью останавливает Wi-Fi драйвер (esp_wifi_stop)
 *
 * @note В STA-режиме обработчик events автоматически
 *       прекратит попытки переподключения при s_wifi_enabled=false.
 *
 * @return ESP_OK при успешной остановке
 */
esp_err_t wifi_stop(void) {
    ESP_LOGI(TAG, "WiFi STOP");
    s_wifi_enabled = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    return ESP_OK;
}

/**
 * @brief Запускает Wi-Fi
 *
 * 1. Устанавливает флаг s_wifi_enabled
 * 2. Запускает Wi-Fi драйвер (esp_wifi_start)
 *
 * В STA-режиме: event_handler автоматически вызовет esp_wifi_connect()
 * при WIFI_EVENT_STA_START. При обрыве — переподключится автоматически.
 *
 * В AP-режиме: начинает вещание собственной точки доступа.
 *
 * @return ESP_OK при успешном запуске
 */
esp_err_t wifi_start(void) {
    ESP_LOGI(TAG, "WiFi START");
    s_wifi_enabled = true;
    esp_wifi_start();
    return ESP_OK;
}

/**
 * @brief Возвращает текущее состояние Wi-Fi
 *
 * @return true если Wi-Fi активен (включён и подключён/вещает),
 *         false если остановлен
 */
bool wifi_is_on(void) {
    return s_wifi_enabled;
}

/**
 * @brief Задача мигания статусной LED
 *
 * Настраивает GPIO статусной LED как выход и мигает с периодом 200 мс:
 * - LED включена (LOW) если Wi-Fi активен
 * - LED выключена (HIGH) если Wi-Fi остановлен
 *
 * Запускается как FreeRTOS-задача на ядре 0 с минимальным приоритетом (1).
 * Работает бесконечно — не завершается никогда.
 *
 * @param[in] arg пользовательский аргумент (не используется)
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
        gpio_set_level(STATUS_LED_GPIO, wifi_is_on() ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/**
 * @brief Инициализирует mDNS и регистрирует HTTP-сервис
 *
 * Устройство доступно по адресу http://<MDNS_HOSTNAME>.local
 */
static void mdns_init_service(void) {
    mdns_init();
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("DMX Sniffer / Tester");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
}

/**
 * @brief Инициализирует сеть: Ethernet/Wi-Fi, mDNS, LED-статус, HTTP-сервер
 *
 * Главная точка входа для сетевого стека. Вызывается из app_main()
 * после инициализации DMX и LED. Порядок важен.
 *
 * При USE_W5500=1:
 *   1. Инициализирует TCP/IP стек и event loop
 *   2. Пытается запустить W5500 Ethernet
 *   3. При ошибке — fallback на WiFi
 *   4. Запускает mDNS, статусную LED, HTTP-сервер
 *
 * При USE_W5500=0:
 *   1. Инициализирует WiFi
 *   2. Запускает mDNS, статусную LED, HTTP-сервер
 */
#if USE_W5500
esp_err_t network_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t err = w5500_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "W5500 init failed, falling back to WiFi");
        wifi_init();
    }

    mdns_init_service();
    xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 1, NULL, 0);
    web_server_init();
    return ESP_OK;
}
#else
esp_err_t network_init(void) {
    wifi_init();

    mdns_init_service();
    xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 1, NULL, 0);
    web_server_init();
    return ESP_OK;
}
#endif
