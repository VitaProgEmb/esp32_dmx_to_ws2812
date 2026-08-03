/**
 * @file main.c
 * @brief Точка входа — только init вызовы
 *
 * Инициализирует все подсистемы сниффера DMX в строгом порядке.
 * Порядок важен из-за зависимостей между модулями:
 *
 * 1. LED-драйверы (ws2812) — инициализируются первыми, т.к. используются
 *    практически всеми остальными модулями для индикации и вывода
 *
 * 2. Настройки (NVS) — читаются из persistent storage и нужны
 *    для корректной настройки DMX, патча и эффектов
 *
 * 3. Патч-таблица — загружается из NVS, нужна для маппинга
 *    DMX-каналов на LED-пиксели в режиме patch
 *
 * 4. Эффекты — инициализируются после патча, т.к. используют
 *    патч-таблицу для вычисления цветов LED
 *
 * 5. DMX — аппаратный драйвер UART с TX/RX/DIR пинами.
 *    Запускается после LED, т.к. начинает отправлять данные
 *    на LED-ленту при старте
 *
 * 6. Обработчики (handlers) — инициализируются после DMX,
 *    т.к. обращаются к глобальным структурам g_dmx, g_patch
 *
 * 7. Сеть (Wi-Fi + HTTP) — запускается последней, т.к. webhook'и
 *    и API-эндпоинты обращаются ко всем выше перечисленным модулям
 *
 * 8. UDP-тест — вспомогательный модуль для отладки, запускается
 *    после сети для тестирования UDP-соединений
 *
 * 9. OTA-разметка — отмечает текущую прошивку как валидную,
 *    отменяя автоматический rollback при нестабильной работе
 *
 * @note Все модули работают в одном потоке app_main().
 *       Асинхронные задачи (сеть, LED, UDP) создаются внутри
 *       своих модулей через xTaskCreate.
 */

#include "esp_log.h"
#include "esp_ota_ops.h"

#include "settings.h"
#include "utilite/settings_manager.h"
#include "utilite/patch_manager.h"
#include "utilite/effects.h"
#include "dmx/dmx.h"
#include "led_driver/ws2812.h"
#include "utilite/handlers.h"
#include "network/wifi_ap.h"
#include "utilite/udp_test.h"

/**
 * @brief Главная функция приложения — точка входа ESP-IDF
 *
 * Вызывается системой после загрузки прошики. Инициализирует
 * все подсистемы в строгом порядке (см. описание файла).
 *
 * Шаги:
 * 1. led_init() × 2 — инициализация двух LED-лент (GPIO и GPIO2)
 * 2. settings_init() — загрузка настроек из NVS
 * 3. patch_init() — загрузка патч-таблицы из NVS
 * 4. effects_init() — инициализация генератора эффектов
 * 5. dmx_init() — запуск DMX-драйвера (4 GPIO: TX1, TX2, RX1, RX2 + DIR2)
 * 6. app_handlers_init() — регистрация пользовательских обработчиков
 * 7. network_init() — Wi-Fi + HTTP-сервер + статусная LED
 * 8. udp_test_init() — запуск UDP-тестового клиента
 * 9. esp_ota_mark_app_valid_cancel_rollback() — подтверждение OTA
 */
void app_main(void) {
    led_init(0, LED_STRIP_GPIO, LED_DEFAULT_COUNT);
    led_init(1, LED_STRIP_GPIO2, LED_DEFAULT_COUNT);

    settings_init();
    patch_init();
    effects_init();

    dmx_init(DMX_GPIO_TX1, DMX_GPIO_TX2, DMX_GPIO_RX1, DMX_GPIO_RX2, DMX_GPIO_DIR2);
    app_handlers_init();

    network_init();
    udp_test_init();

    esp_ota_mark_app_valid_cancel_rollback();
}
