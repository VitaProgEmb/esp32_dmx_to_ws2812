#pragma once

#include <stdint.h>
#include "led_driver/ws2812.h"

typedef enum {
    FX_OFF      = 0,
    FX_STATIC   = 1,
    FX_RAINBOW  = 2,
    FX_RUNNING  = 3,
    FX_BREATHE  = 4,
    FX_WAVE     = 5,
} fx_mode_t;

typedef struct {
    uint8_t  mode;     /* fx_mode_t */
    uint8_t  r, g, b;  /* цвет для running/breathe */
    uint8_t  speed;    /* не используется в рендере (frame counter) */
    uint16_t pixel;    /* начальный пиксель для static */
    uint16_t count;    /* количество пикселей для static */
} fx_state_t;

void effects_init(void);
void effects_set(const fx_state_t *state);
void effects_clear(void);
void effects_render(void);
bool effects_is_active(void);
