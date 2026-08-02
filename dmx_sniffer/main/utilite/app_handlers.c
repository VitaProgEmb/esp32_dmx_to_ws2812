/**
 * @file app_handlers.c
 * @brief Обработчики команд от web сервера
 */

#include "handlers.h"
#include "settings.h"
#include "dmx/dmx.h"
#include "dmx/dmx_led.h"
#include "led_strip.h"
#include "utilite/effects.h"

/* ===== Handlers ===== */

static void handle_mode(uint8_t mode) {
    if (mode == 0) dmx_set_mode(DMX_MODE_SNIFFER);
    else if (mode == 1) dmx_set_mode(DMX_MODE_TESTER);
    else if (mode == 2) dmx_set_mode(DMX_MODE_PATCH);
}

static void handle_led_test(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint8_t speed, uint16_t pixel, uint16_t count) {
    if (mode == 1) {
        effects_clear();
        uint8_t co;
        dmx_lock(); co = g_led_settings.channel_order; dmx_unlock();
        uint8_t cr = r, cg = g, cb = b;
        static const uint8_t omap[][3] = {
            [CH_ORDER_RGB]={0,1,2},[CH_ORDER_RBG]={0,2,1},
            [CH_ORDER_GRB]={1,0,2},[CH_ORDER_GBR]={1,2,0},
            [CH_ORDER_BRG]={2,0,1},[CH_ORDER_BGR]={2,1,0},
        };
        if (co < CH_ORDER_COUNT) { uint8_t raw[3]={cr,cg,cb}; cr=raw[omap[co][0]]; cg=raw[omap[co][1]]; cb=raw[omap[co][2]]; }
        led_lock(0);
        led_color_t *back = led_get_colors(0);
        uint16_t cnt = led_get_count(0);
        for (uint16_t i = 0; i < cnt; i++) { back[i].r = 0; back[i].g = 0; back[i].b = 0; }
        for (int i = 0; i < count && pixel + i < cnt; i++)
            led_set_pixel(0, pixel + i, cr, cg, cb);
        led_unlock(0);
        led_refresh(0);
        return;
    }
    fx_state_t fx = { .mode = mode, .r = r, .g = g, .b = b, .speed = speed, .pixel = pixel, .count = count };
    effects_set(&fx);
}

static void handle_led_clear(void) { effects_clear(); dmx_led_apply_fallback(); }

static void handle_led_count(uint8_t strip, uint16_t count) {
    led_set_count(strip, count);
    dmx_led_recompute();
    led_refresh(strip);
}

static void handle_led_reverse(bool on)      { dmx_lock(); g_led_settings.reverse = on; dmx_unlock(); }
static void handle_led_mode(bool sequential)  { g_led_mode = sequential ? LED_MODE_SEQUENTIAL : LED_MODE_PARALLEL; }
static void handle_led_shift(uint8_t shift)   { dmx_lock(); g_led_settings.shift = shift; dmx_unlock(); }
static void handle_led_interpolate(bool on)   { dmx_lock(); g_led_settings.interpolate = on; dmx_unlock(); }

static void handle_dmx_test(uint8_t port, uint8_t channel, uint8_t r, uint8_t g, uint8_t b, bool fill, uint16_t count) {
    dmx_lock();
    g_dmx.tx_port = port; g_dmx.tx_channel = channel;
    g_dmx.tx_r = r; g_dmx.tx_g = g; g_dmx.tx_b = b;
    g_dmx.tx_mode = fill ? TX_MODE_FILL : TX_MODE_POINT;
    g_dmx.tx_count = count;
    dmx_unlock();
    dmx_set_mode(DMX_MODE_TESTER);
}

static void handle_fixture_settings(uint8_t channel_order, uint8_t fb_r, uint8_t fb_g, uint8_t fb_b, uint16_t fb_timeout) {
    dmx_lock();
    g_led_settings.channel_order = channel_order;
    g_led_settings.fallback_r = fb_r; g_led_settings.fallback_g = fb_g; g_led_settings.fallback_b = fb_b;
    g_led_settings.fallback_timeout_ms = fb_timeout;
    dmx_unlock();
    dmx_led_apply_fallback();
}

/* ===== Init ===== */

void app_handlers_init(void) {
    dmx_led_init(&(dmx_led_cbs_t){
        .on_mode             = handle_mode,
        .on_led_test         = handle_led_test,
        .on_led_clear        = handle_led_clear,
        .on_led_count        = handle_led_count,
        .on_led_reverse      = handle_led_reverse,
        .on_led_mode         = handle_led_mode,
        .on_led_shift        = handle_led_shift,
        .on_led_interpolate  = handle_led_interpolate,
        .on_dmx_test         = handle_dmx_test,
        .on_fixture_settings = handle_fixture_settings,
    });
}
