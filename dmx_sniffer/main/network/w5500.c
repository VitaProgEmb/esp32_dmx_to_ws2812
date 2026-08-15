/**
 * @file w5500.c
 * @brief Реализация модуля Ethernet W5500 (SPI)
 *
 * Инициализация и управление Ethernet-модулем W5500 через SPI.
 * Поддерживает горячее подключение/отключение кабеля (hot plug)
 * через периодический опрос PHYCFGR и перезапуск DHCP.
 */

#include "w5500.h"
#include "settings.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_spi.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"

/** @brief Тег логирования ESP-IDF для модуля W5500 */
static const char *TAG = "W5500";

/** @brief Ethernet netif — доступен из обработчика событий для управления DHCP */
static esp_netif_t *s_eth_netif = NULL;

/** @brief Ethernet handle — нужен для опроса линка в keep-alive задаче */
static esp_eth_handle_t s_eth_handle = NULL;

/** @brief Интервал keep-alive опроса (мс) — 10 секунд для поддержания ARP */
#define W5500_KEEPALIVE_INTERVAL_MS  10000

/**
 * @brief Чтение PHY-регистра W5500 через esp_eth_ioctl
 *
 * @param reg Адрес PHY-регистра (0..31)
 * @return Значение регистра или -1 при ошибке
 */
static int w5500_read_phy_reg(uint32_t reg) {
    if (!s_eth_handle) return -1;
    uint32_t val = reg;
    esp_err_t err = esp_eth_ioctl(s_eth_handle, ETH_CMD_READ_PHY_REG, &val);
    return (err == ESP_OK) ? (int)val : -1;
}

/**
 * @brief Отправка 1 байта на шлюз по UDP для обновления ARP-таблицы роутера
 *
 * Генерирует реальный Ethernet-трафик, не даёт роутеру забыть MAC W5500.
 * Решает проблему 10-секундной задержки первого запроса после простоя.
 */
static void w5500_arp_keepalive(void) {
    if (!s_eth_netif) return;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_eth_netif, &ip_info) != ESP_OK) return;
    if (ip4_addr_isany(&ip_info.gw)) return;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(33434),
        .sin_addr.s_addr = ip_info.gw.addr,
    };

    uint8_t dummy = 0;
    sendto(sock, &dummy, 1, 0, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);
}

/**
 * @brief Keep-alive задача — поддержание линка и ARP W5500
 *
 * Каждые 10 секунд:
 *   1) Читает PHY-регистр BMSR — проверяет link status
 *   2) Отправляет UDP-пакет на шлюз — обновляет ARP-таблицу роутера
 *   3) Перезапускает DHCP при восстановлении линка
 */
static void w5500_keepalive_task(void *arg) {
    bool prev_link_up = true;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(W5500_KEEPALIVE_INTERVAL_MS));

        if (!s_eth_handle || !s_eth_netif) continue;

        /* BMSR (Basic Mode Status Register) = 0x01, бит 2 = Link Status */
        int bmsr = w5500_read_phy_reg(0x01);
        if (bmsr < 0) {
            ESP_LOGW(TAG, "Keep-alive: PHY read failed");
            continue;
        }

        bool link_up = (bmsr >> 2) & 1;

        if (link_up && !prev_link_up) {
            ESP_LOGI(TAG, "Keep-alive: link recovered — restarting DHCP");
            esp_netif_dhcpc_stop(s_eth_netif);
            esp_netif_dhcpc_start(s_eth_netif);
        } else if (!link_up && prev_link_up) {
            ESP_LOGW(TAG, "Keep-alive: link lost");
        }

        prev_link_up = link_up;

        /* Генерация реального Ethernet-трафика для обновления ARP роутера */
        if (link_up) {
            w5500_arp_keepalive();
        }
    }
}

/**
 * @brief Обработчик событий Ethernet (hot plug) и IP
 *
 * Управляет DHCP-клиентом при отключении/подключении кабеля:
 * - ETHERNET_EVENT_CONNECTED → перезапуск DHCP
 * - ETHERNET_EVENT_DISCONNECTED → остановка DHCP
 * - IP_EVENT_ETH_GOT_IP → логирование полученного IP-адреса
 */
static void w5500_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == ETH_EVENT) {
        if (id == ETHERNET_EVENT_CONNECTED) {
            ESP_LOGI(TAG, "ETH link UP");
            if (s_eth_netif) {
                esp_netif_dhcpc_stop(s_eth_netif);
                esp_netif_dhcpc_start(s_eth_netif);
                ESP_LOGI(TAG, "DHCP restarted");
            }
        } else if (id == ETHERNET_EVENT_DISCONNECTED) {
            ESP_LOGW(TAG, "ETH link DOWN");
            if (s_eth_netif) {
                esp_netif_dhcpc_stop(s_eth_netif);
                ESP_LOGW(TAG, "DHCP stopped");
            }
        } else {
            ESP_LOGI(TAG, "ETH event: %ld", (long)id);
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_ETH_GOT_IP) {
            ip_event_got_ip_t *ev = data;
            ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        }
    }
}

/**
 * @brief Инициализирует Ethernet W5500 через SPI
 *
 * Алгоритм:
 * 1. Регистрация обработчиков ETH_EVENT и IP_EVENT_ETH_GOT_IP
 * 2. Настройка GPIO ISR service
 * 3. Инициализация SPI-шины (MOSI/MISO/SCK/CS)
 * 4. Создание MAC и PHY W5500
 * 5. Установка драйвера Ethernet с опросом линка каждые 500 мс
 * 6. Проверка MAC (fallback 02:00:11:22:33:44 при нулевом)
 * 7. Привязка к TCP/IP стеку
 * 8. Запуск Ethernet и DHCP-клиента
 *
 * @return ESP_OK при успешной инициализации
 */
esp_err_t w5500_init(void) {
    ESP_LOGI(TAG, "W5500 Ethernet init");

    /* 1. Регистрация обработчиков событий */
    esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_event_handler, NULL, NULL);
    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &w5500_event_handler, NULL, NULL);

    /* 2. gpio_install_isr_service может быть уже вызван другим драйвером */
    gpio_install_isr_service(0);

    esp_err_t err;

    /* 3. Конфигурация SPI-шины */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = W5500_SPI_MOSI,
        .miso_io_num   = W5500_SPI_MISO,
        .sclk_io_num   = W5500_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Конфигурация SPI-устройства (CS) */
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 40 * 1000 * 1000,  /* 40 МГц — максимальная скорость W5500 */
        .spics_io_num = W5500_SPI_CS,
        .queue_size = 20,
    };

    /* 5. Создание MAC W5500 */
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_cfg.int_gpio_num = W5500_SPI_INT;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 8192;
    mac_cfg.rx_task_prio = 5;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC");
        return ESP_FAIL;
    }

    /* 6. Создание PHY W5500 */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY");
        mac->deinit(mac);
        return ESP_FAIL;
    }

    /* 7. Установка драйвера Ethernet */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    eth_cfg.check_link_period_ms = 500;  /* Быстрое обнаружение кабеля (по умолч. 2000) */
    s_eth_handle = NULL;
    err = esp_eth_driver_install(&eth_cfg, &s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        phy->del(phy);
        mac->deinit(mac);
        return err;
    }

    /* 7.1 Проверка MAC после software reset — driver_install() очищает SHAR */
    {
        uint8_t mac_check[6] = {0};
        mac->get_addr(mac, mac_check);
        bool mac_zero = true;
        for (int i = 0; i < 6; i++) { if (mac_check[i] != 0) { mac_zero = false; break; } }
        if (mac_zero) {
            uint8_t fallback_mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
            mac->set_addr(mac, fallback_mac);
            ESP_LOGW(TAG, "W5500 MAC=00:00:00:00:00:00 — set fallback 02:00:11:22:33:44");
        } else {
            ESP_LOGI(TAG, "W5500 MAC=%02X:%02X:%02X:%02X:%02X:%02X",
                     mac_check[0], mac_check[1], mac_check[2],
                     mac_check[3], mac_check[4], mac_check[5]);
        }
    }

    /* 8. Привязка к TCP/IP стеку */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    err = esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Netif attach failed: %s", esp_err_to_name(err));
        esp_eth_driver_uninstall(s_eth_handle);
        return err;
    }

    /* 9. Запуск Ethernet */
    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        esp_eth_driver_uninstall(s_eth_handle);
        return err;
    }

    /* 10. Запуск DHCP-клиента после запуска Ethernet */
    esp_netif_dhcpc_start(s_eth_netif);

    /* 11. Keep-alive задача — предотвращает засыпание W5500 */
    xTaskCreatePinnedToCore(w5500_keepalive_task, "w5500_keep", 2048, NULL, 1, NULL, 1);

    ESP_LOGI(TAG, "W5500 Ethernet started — waiting for DHCP");

    return ESP_OK;
}
