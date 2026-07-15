#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DMX_PORT_1      0
#define DMX_PORT_2      1
#define DMX_PORT_COUNT  2
#define DMX_CHANNELS    512
#define DMX_FRAME_LEN   513

void dmx_hal_init(void);
void dmx_hal_start(void);
bool dmx_hal_send(int port, const uint8_t *data, int len, int break_len);
