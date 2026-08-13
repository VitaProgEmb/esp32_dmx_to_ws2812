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
#include <string.h>

#if USE_W5500
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#endif

/** @brief Тег логирования ESP-IDF для модуля Wi-Fi */
static const char *TAG = "WIFI";

/** @brief Флаг текущего состояния Wi-Fi (true=включён, false=выключен) */
static bool s_wifi_enabled = false;

/**
 * @brief Обработчик событий Wi-Fi и IP
 *
 * Обрабатывает два типа событий:
 * - WIFI_EVENT: STA_START → автоматическое подключение;
 *   STA_DISCONNECTED → повторная попытка подключения (если WiFi включён)
 * - IP_EVENT: STA_GOT_IP → логирование полученного IP-адреса
 *
 * @param[in] arg   пользовательский аргумент (не используется)
 * @param[in] base  база события (WIFI_EVENT или IP_EVENT)
 * @param[in] id    идентификатор события
 * @param[in] data  данные события (ip_event_got_ip_t для IP_EVENT)
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_wifi_enabled) esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

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

#if USE_W5500
/**
 * @brief Инициализирует Ethernet W5500 через SPI
 *
 * Настраивает SPI-шину и инициализирует Ethernet MAC через esp_eth.
 * W5500 подключается к ESP32-S3 по SPI (MOSI/MISO/SCK/CS/INT).
 *
 * @return ESP_OK при успешной инициализации
 */
static esp_err_t w5500_init(void) {
    ESP_LOGI(TAG, "W5500 Ethernet init");

    /* 1. Конфигурация SPI-шины */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = W5500_SPI_MOSI,
        .miso_io_num   = W5500_SPI_MISO,
        .sclk_io_num   = W5500_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 2. Конфигурация SPI-устройства (CS) */
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 25 * 1000 * 1000,  /* 25 МГц */
        .spics_io_num = W5500_SPI_CS,
        .queue_size = 20,
    };

    /* 3. Создание MAC W5500 */
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_cfg.int_gpio_num = W5500_SPI_INT;
    w5500_cfg.poll_period_ms = 10;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    mac_cfg.rx_task_prio = 5;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);

    /* 4. Создание PHY W5500 */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);

    /* 5. Установка драйвера Ethernet */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    /* 6. Привязка к TCP/IP стеку */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    /* 7. Запуск Ethernet */
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "W5500 Ethernet started");
    return ESP_OK;
}
#endif

#if USE_W5500
esp_err_t network_init(void) {
    w5500_init();
    xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 1, NULL, 0);
    web_server_init();
    return ESP_OK;
}
#else
esp_err_t network_init(void) {
    wifi_init();
    xTaskCreatePinnedToCore(status_led_task, "status_led", 2048, NULL, 1, NULL, 0);
    web_server_init();
    return ESP_OK;
}
#endif
