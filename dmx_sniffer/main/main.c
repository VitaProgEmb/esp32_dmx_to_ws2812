/**
 * @file main.c
 * @brief Точка входа — только init вызовы
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
