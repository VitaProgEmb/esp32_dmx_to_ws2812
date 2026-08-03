/**
 * @file wifi_ap.h
 * @brief Модуль Wi-Fi (STA + AP с toggle)
 *
 * Управление Wi-Fi подключением в двух режимах:
 * - STA (Station): подключение к существующей точке доступа
 * - AP (Access Point): собственная точка доступа для прямого подключения
 *
 * Режим выбирается макросом WIFI_MODE_AP_RELEASE при компиляции.
 * Поддерживается горячее переключение WiFi вкл/выкл через wifi_toggle().
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Инициализирует сеть: Wi-Fi, LED-статус, HTTP-сервер
 *
 * Главная точка входа для сетевого стека. Вызывается из app_main()
 * после инициализации DMX и LED. Порядок важен:
 * wifi_init() → status_led_task → web_server_init().
 *
 * @return ESP_OK при успешной инициализации
 */
esp_err_t network_init(void);

/**
 * @brief Переключает состояние Wi-Fi (вкл→выкл, выкл→вкл)
 *
 * Если Wi-Fi включён — отключает (wifi_stop),
 * если выключен — включает (wifi_start).
 *
 * @return ESP_OK при успешном переключении
 */
esp_err_t wifi_toggle(void);

/**
 * @brief Останавливает Wi-Fi и отключается от сети
 *
 * Сбрасывает флаг s_wifi_enabled, отключается от AP,
 * останавливает Wi-Fi драйвер. Соединения разрываются.
 *
 * @return ESP_OK при успешной остановке
 */
esp_err_t wifi_stop(void);

/**
 * @brief Запускает Wi-Fi и подключается к сети
 *
 * Устанавливает флаг s_wifi_enabled и запускает Wi-Fi драйвер.
 * В режиме STA начинает автоматическое подключение к AP.
 * В режиме AP начинает вещание собственной точки доступа.
 *
 * @return ESP_OK при успешном запуске
 */
esp_err_t wifi_start(void);

/**
 * @brief Возвращает текущее состояние Wi-Fi
 *
 * @return true если Wi-Fi активен (подключён или вещает),
 *         false если остановлен
 */
bool wifi_is_on(void);
