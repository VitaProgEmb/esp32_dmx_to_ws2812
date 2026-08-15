/**
 * @file w5500.h
 * @brief Модуль Ethernet W5500 (SPI)
 *
 * Инициализация и управление Ethernet-модулем W5500 через SPI.
 * Поддерживает горячее подключение/отключение кабеля (hot plug)
 * через периодический опрос PHYCFGR и перезапуск DHCP.
 */

#pragma once

#include "esp_err.h"

/**
 * @brief Инициализирует Ethernet W5500
 *
 * Настраивает SPI-шину, создаёт MAC/PHY, привязывает к TCP/IP стеку,
 * запускает Ethernet и DHCP-клиент. Регистрирует обработчик событий
 * ETH_EVENT для управления DHCP при hot plug.
 *
 * @return ESP_OK при успешной инициализации, ESP_FAIL при ошибке
 */
esp_err_t w5500_init(void);
