#include "effects.h"
#include <math.h>

static fx_state_t s_fx = { .mode = FX_OFF };
static uint32_t   s_frame = 0;

static void hsv_to_rgb(int h, int s, int v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h %= 360;
    int c = (v * s) / 255;
    int x = c * (1 - abs((h / 60) % 2 - 1));
    int m = v - c;
    if      (h < 60)  { *r = c; *g = x; *b = 0; }
    else if (h < 120) { *r = x; *g = c; *b = 0; }
    else if (h < 180) { *r = 0; *g = c; *b = x; }
    else if (h < 240) { *r = 0; *g = x; *b = c; }
    else if (h < 300) { *r = x; *g = 0; *b = c; }
    else              { *r = c; *g = 0; *b = x; }
    *r += m; *g += m; *b += m;
}

void effects_init(void) {
    s_fx.mode = FX_OFF;
    s_frame = 0;
}

void effects_set(const fx_state_t *state) {
    s_fx = *state;
    s_frame = 0;
}

void effects_clear(void) {
    s_fx.mode = FX_OFF;
}

bool effects_is_active(void) {
    return s_fx.mode >= 2;
}

void effects_render(void) {
    if (s_fx.mode < 2) return;

    uint16_t count = led_get_count(0);
    if (count == 0) return;

    s_frame++;

    led_lock(0);
    led_color_t *back = led_get_colors(0);

    switch (s_fx.mode) {
    case FX_RAINBOW:
        for (uint16_t i = 0; i < count; i++) {
            int hue = (i * 360 / count + s_frame * 3) % 360;
            hsv_to_rgb(hue, 255, 255, &back[i].r, &back[i].g, &back[i].b);
        }
        break;

    case FX_RUNNING: {
        int pos = s_frame % (count + 10);
        for (uint16_t i = 0; i < count; i++) {
            int dist = abs(i - pos);
            if (dist < 5) {
                uint8_t br = 255 - (dist * 51);
                back[i].r = (s_fx.r * br) / 255;
                back[i].g = (s_fx.g * br) / 255;
                back[i].b = (s_fx.b * br) / 255;
            } else {
                back[i].r = 0;
                back[i].g = 0;
                back[i].b = 0;
            }
        }
        break;
    }

    case FX_BREATHE: {
        float phase = (float)(s_frame % 120) / 120.0f * 3.14159f * 2.0f;
        float br = (sinf(phase) + 1.0f) / 2.0f;
        uint8_t bv = (uint8_t)(br * 255);
        for (uint16_t i = 0; i < count; i++) {
            back[i].r = (s_fx.r * bv) / 255;
            back[i].g = (s_fx.g * bv) / 255;
            back[i].b = (s_fx.b * bv) / 255;
        }
        break;
    }

    case FX_WAVE:
        for (uint16_t i = 0; i < count; i++) {
            int hue = (i * 5 + s_frame * 4) % 360;
            hsv_to_rgb(hue, 200, 255, &back[i].r, &back[i].g, &back[i].b);
        }
        break;

    default:
        break;
    }

    led_unlock(0);
    led_refresh(0);
    led_refresh(1);
}
