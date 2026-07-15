#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

esp_err_t web_server_init(void);
void      web_server_stop(void);

extern volatile int g_led_test_mode;
extern volatile int g_led_test_px;
extern volatile int g_led_test_count;
extern volatile int g_led_test_fps;
