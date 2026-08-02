/**
 * @file wifi_ap.h
 * @brief Модуль Wi-Fi (STA + AP с toggle)
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t network_init(void);
esp_err_t wifi_toggle(void);
esp_err_t wifi_stop(void);
esp_err_t wifi_start(void);
bool wifi_is_on(void);
