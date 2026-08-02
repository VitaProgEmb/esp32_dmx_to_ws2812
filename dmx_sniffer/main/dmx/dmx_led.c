/**
 * @file dmx_led.c
 * @brief DMX → LED мост: патч, interpolation, fallback
 *
 * Чистый модуль — без signal.h, без event_bus.h.
 * Все действия через callback struct из dmx_led_init().
 */

#include "dmx/dmx.h"
#include "dmx/dmx_led.h"
#include "settings.h"
#include "led_driver/ws2812.h"
#include "utilite/patch_manager.h"
#include "utilite/effects.h"
#include "esp_log.h"

dmx_led_settings_t g_led_settings = { .reverse = false, .shift = 0, .interpolate = true, .channel_order = CH_ORDER_RGB };
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "DMX_LED";

/* ===== Callbacks (set by init) ===== */

static const dmx_led_cbs_t *s_cbs = NULL;

/* ===== Lookup tables ===== */

static uint16_t s_ws_start[PATCH_MAX_ENTRIES];
static uint16_t s_ws_count[PATCH_MAX_ENTRIES];
static uint8_t  s_fixture_colors[PATCH_MAX_ENTRIES][3];

static const uint8_t s_order_map[CH_ORDER_COUNT][3] = {
    [CH_ORDER_RGB] = {0, 1, 2}, [CH_ORDER_RBG] = {0, 2, 1},
    [CH_ORDER_GRB] = {1, 0, 2}, [CH_ORDER_GBR] = {1, 2, 0},
    [CH_ORDER_BRG] = {2, 0, 1}, [CH_ORDER_BGR] = {2, 1, 0},
};

/* ===== Fallback ===== */

static esp_timer_handle_t s_fallback_timer = NULL;
static SemaphoreHandle_t  s_led_refresh_sem = NULL;
static volatile bool      s_fallback_pending = false;

static void fallback_timer_callback(void *arg) {
    s_fallback_pending = true;
    xSemaphoreGive(s_led_refresh_sem);
}

/* ===== LED Refresh Task ===== */

static void led_refresh_task(void *arg) {
    while (1) {
        xSemaphoreTake(s_led_refresh_sem, pdMS_TO_TICKS(5));
        if (s_fallback_pending) {
            s_fallback_pending = false;
            dmx_led_apply_fallback();
        }
        effects_render();
        led_refresh(0);
        led_refresh(1);
    }
}

/* ===== Public API ===== */

void dmx_led_recompute(void) {
    dmx_lock();
    uint16_t n = g_patch.count;
    uint16_t m = g_total_leds;
    if (n > 0) {
        for (uint16_t f = 0; f < n; f++)
            s_ws_start[f] = (uint32_t)f * m / n;
        for (uint16_t f = 0; f < n; f++) {
            uint16_t next = (f + 1 < n) ? s_ws_start[f + 1] : m;
            s_ws_count[f] = next - s_ws_start[f];
        }
    }
    dmx_unlock();
}

static void apply_color_order(uint8_t ch_order, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (ch_order >= CH_ORDER_COUNT) return;
    uint8_t raw[3] = {*r, *g, *b};
    *r = raw[s_order_map[ch_order][0]];
    *g = raw[s_order_map[ch_order][1]];
    *b = raw[s_order_map[ch_order][2]];
}

void dmx_led_apply_fallback(void) {
    dmx_lock();
    bool is_sniffer = (g_dmx.mode == DMX_MODE_SNIFFER);
    uint8_t fb_r = g_led_settings.fallback_r, fb_g = g_led_settings.fallback_g, fb_b = g_led_settings.fallback_b;
    uint8_t ch_order = g_led_settings.channel_order;
    dmx_unlock();

    if (!is_sniffer || led_get_count(0) == 0 || effects_is_active()) return;

    apply_color_order(ch_order, &fb_r, &fb_g, &fb_b);

    led_lock(0);
    led_color_t *back = led_get_colors(0);
    uint16_t cnt = led_get_count(0);
    for (uint16_t i = 0; i < cnt; i++) { back[i].r = fb_r; back[i].g = fb_g; back[i].b = fb_b; }
    led_unlock(0);

    s_fallback_pending = true;
    xSemaphoreGive(s_led_refresh_sem);
}

/* ===== DMX frame → LED buffer ===== */

static void do_led_processing(int port, const uint8_t *slots, uint16_t max_slot) {
    dmx_lock();
    dmx_mode_t mode = g_dmx.mode;
    uint8_t cpf = DMX_FIXTURE_CH;
    uint8_t n_entries = g_patch.count;
    led_mode_t led_mode = g_led_mode;
    dmx_unlock();

    if (mode != DMX_MODE_SNIFFER) return;
    if (effects_is_active()) return;

    for (uint8_t f = 0; f < n_entries; f++) {
        dmx_lock();
        bool skip = g_patch.entries[f].skip;
        uint16_t addr = g_patch.entries[f].dmx_addr;
        uint8_t co = g_led_settings.channel_order;
        dmx_unlock();

        if (skip) continue;
        if (addr + cpf - 1 > max_slot) continue;

        dmx_lock();
        uint8_t raw[3] = { slots[addr - 1], slots[addr], slots[addr + 1] };
        dmx_unlock();
        s_fixture_colors[f][0] = raw[s_order_map[co][0]];
        s_fixture_colors[f][1] = raw[s_order_map[co][1]];
        s_fixture_colors[f][2] = raw[s_order_map[co][2]];
    }

    uint16_t count1 = led_get_count(0);
    uint16_t count2 = led_get_count(1);
    led_color_t *back1 = led_get_colors(0);
    led_color_t *back2 = led_get_colors(1);

    dmx_lock();
    bool interp = g_led_settings.interpolate;
    bool rev = g_led_settings.reverse;
    uint16_t shft = g_led_settings.shift;
    dmx_unlock();

    led_set_reverse(0, rev); led_set_shift(0, shft);
    led_set_reverse(1, rev); led_set_shift(1, shft);

    if (led_mode == LED_MODE_SEQUENTIAL) {
        uint16_t total = count1 + count2;
        if (total == 0) goto do_fallback;

        led_color_t *tmp = (count2 > 0) ? calloc(total, sizeof(led_color_t)) : back1;
        if (!tmp) goto do_fallback;

        if (interp) {
            if (n_entries < 2 || total <= 1) {
                led_color_t color = {0, 0, 0};
                if (n_entries > 0) { color.r = s_fixture_colors[0][0]; color.g = s_fixture_colors[0][1]; color.b = s_fixture_colors[0][2]; }
                for (uint16_t i = 0; i < total; i++) tmp[i] = color;
            } else {
                for (uint16_t i = 0; i < total; i++) {
                    uint32_t pos_fp = (uint32_t)i * (n_entries - 1) * 256 / (total - 1);
                    uint16_t lo = pos_fp >> 8, hi = lo + 1;
                    if (hi >= n_entries) hi = n_entries - 1;
                    uint8_t w = pos_fp & 0xFF;
                    tmp[i].r = ((uint32_t)s_fixture_colors[lo][0] * (256 - w) + (uint32_t)s_fixture_colors[hi][0] * w) >> 8;
                    tmp[i].g = ((uint32_t)s_fixture_colors[lo][1] * (256 - w) + (uint32_t)s_fixture_colors[hi][1] * w) >> 8;
                    tmp[i].b = ((uint32_t)s_fixture_colors[lo][2] * (256 - w) + (uint32_t)s_fixture_colors[hi][2] * w) >> 8;
                }
            }
        } else {
            uint16_t ws_start_l[PATCH_MAX_ENTRIES], ws_count_l[PATCH_MAX_ENTRIES];
            for (uint16_t f = 0; f < n_entries; f++) ws_start_l[f] = (uint32_t)f * total / n_entries;
            for (uint16_t f = 0; f < n_entries; f++) {
                uint16_t next = (f + 1 < n_entries) ? ws_start_l[f + 1] : total;
                ws_count_l[f] = next - ws_start_l[f];
            }
            for (uint16_t pe = 0; pe < n_entries; pe++) {
                led_color_t c = { s_fixture_colors[pe][0], s_fixture_colors[pe][1], s_fixture_colors[pe][2] };
                for (uint16_t j = 0; j < ws_count_l[pe]; j++) {
                    uint16_t px = ws_start_l[pe] + j;
                    if (px < total) tmp[px] = c;
                }
            }
        }

        if (effects_is_active()) { if (tmp != back1) free(tmp); return; }

        if (count1 > 0) { led_lock(0); memcpy(back1, tmp, count1 * sizeof(led_color_t)); led_unlock(0); led_swap_banks(0); }
        if (count2 > 0) { led_lock(1); memcpy(back2, tmp + count1, count2 * sizeof(led_color_t)); led_unlock(1); led_swap_banks(1); }
        if (tmp != back1) free(tmp);

    } else {
        if (n_entries == 0) goto do_fallback;

        if (count1 > 0) {
            if (interp) {
                if (n_entries < 2 || count1 <= 1) {
                    led_color_t c = {0, 0, 0};
                    if (n_entries > 0) { c.r = s_fixture_colors[0][0]; c.g = s_fixture_colors[0][1]; c.b = s_fixture_colors[0][2]; }
                    for (uint16_t i = 0; i < count1; i++) back1[i] = c;
                } else {
                    for (uint16_t i = 0; i < count1; i++) {
                        uint32_t pos_fp = (uint32_t)i * (n_entries - 1) * 256 / (count1 - 1);
                        uint16_t lo = pos_fp >> 8, hi = lo + 1;
                        if (hi >= n_entries) hi = n_entries - 1;
                        uint8_t w = pos_fp & 0xFF;
                        back1[i].r = ((uint32_t)s_fixture_colors[lo][0] * (256 - w) + (uint32_t)s_fixture_colors[hi][0] * w) >> 8;
                        back1[i].g = ((uint32_t)s_fixture_colors[lo][1] * (256 - w) + (uint32_t)s_fixture_colors[hi][1] * w) >> 8;
                        back1[i].b = ((uint32_t)s_fixture_colors[lo][2] * (256 - w) + (uint32_t)s_fixture_colors[hi][2] * w) >> 8;
                    }
                }
            } else {
                for (uint16_t f = 0; f < n_entries; f++) {
                    uint16_t start = (uint32_t)f * count1 / n_entries;
                    uint16_t next = (f + 1 < n_entries) ? (uint32_t)(f + 1) * count1 / n_entries : count1;
                    led_color_t c = { s_fixture_colors[f][0], s_fixture_colors[f][1], s_fixture_colors[f][2] };
                    for (uint16_t j = start; j < next; j++) back1[j] = c;
                }
            }
        }

        if (count2 > 0) {
            if (interp) {
                if (n_entries < 2 || count2 <= 1) {
                    led_color_t c = {0, 0, 0};
                    if (n_entries > 0) { c.r = s_fixture_colors[0][0]; c.g = s_fixture_colors[0][1]; c.b = s_fixture_colors[0][2]; }
                    for (uint16_t i = 0; i < count2; i++) back2[i] = c;
                } else {
                    for (uint16_t i = 0; i < count2; i++) {
                        uint32_t pos_fp = (uint32_t)i * (n_entries - 1) * 256 / (count2 - 1);
                        uint16_t lo = pos_fp >> 8, hi = lo + 1;
                        if (hi >= n_entries) hi = n_entries - 1;
                        uint8_t w = pos_fp & 0xFF;
                        back2[i].r = ((uint32_t)s_fixture_colors[lo][0] * (256 - w) + (uint32_t)s_fixture_colors[hi][0] * w) >> 8;
                        back2[i].g = ((uint32_t)s_fixture_colors[lo][1] * (256 - w) + (uint32_t)s_fixture_colors[hi][1] * w) >> 8;
                        back2[i].b = ((uint32_t)s_fixture_colors[lo][2] * (256 - w) + (uint32_t)s_fixture_colors[hi][2] * w) >> 8;
                    }
                }
            } else {
                for (uint16_t f = 0; f < n_entries; f++) {
                    uint16_t start = (uint32_t)f * count2 / n_entries;
                    uint16_t next = (f + 1 < n_entries) ? (uint32_t)(f + 1) * count2 / n_entries : count2;
                    led_color_t c = { s_fixture_colors[f][0], s_fixture_colors[f][1], s_fixture_colors[f][2] };
                    for (uint16_t j = start; j < next; j++) back2[j] = c;
                }
            }
        }

        if (effects_is_active()) return;
        if (count1 > 0) led_swap_banks(0);
        if (count2 > 0) led_swap_banks(1);
    }

do_fallback:
    s_fallback_pending = false;
    xSemaphoreGive(s_led_refresh_sem);

    dmx_lock();
    g_dmx.last_rx_ms[port] = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t fb_timeout = g_led_settings.fallback_timeout_ms;
    dmx_unlock();

    if (fb_timeout > 0 && s_fallback_timer) {
        esp_timer_stop(s_fallback_timer);
        esp_timer_start_once(s_fallback_timer, (uint64_t)fb_timeout * 1000);
    }
}

const uint8_t (*dmx_led_get_fixture_colors(void))[3] {
    return s_fixture_colors;
}

/* ===== DMX frame callback ===== */

static void on_dmx_frame(int port) {
    dmx_raw_frame_t *f = &g_raw_frames[port];
    if (f->len > 1)
        do_led_processing(port, f->data + 1, f->len - 1);
}

/* ===== Internal handlers (called from web_server/main via callbacks) ===== */

static void handle_mode(uint8_t mode) {
    if (mode == 0) dmx_set_mode(DMX_MODE_SNIFFER);
    else if (mode == 1) dmx_set_mode(DMX_MODE_TESTER);
    else if (mode == 2) dmx_set_mode(DMX_MODE_PATCH);
}

static void handle_led_test(uint8_t mode, uint8_t r, uint8_t g, uint8_t b,
                            uint8_t speed, uint16_t pixel, uint16_t count) {
    if (mode == 1) {
        effects_clear();
        uint8_t co;
        dmx_lock(); co = g_led_settings.channel_order; dmx_unlock();
        uint8_t cr = r, cg = g, cb = b;
        apply_color_order(co, &cr, &cg, &cb);
        led_lock(0);
        led_color_t *back = led_get_colors(0);
        uint16_t cnt = led_get_count(0);
        memset(back, 0, cnt * sizeof(led_color_t));
        for (int i = 0; i < count && pixel + i < cnt; i++)
            led_set_pixel(0, pixel + i, cr, cg, cb);
        led_unlock(0);
        led_refresh(0);
        return;
    }
    fx_state_t fx = { .mode = mode, .r = r, .g = g, .b = b,
                      .speed = speed, .pixel = pixel, .count = count };
    effects_set(&fx);
}

static void handle_led_clear(void) {
    effects_clear();
    dmx_led_apply_fallback();
}

static void handle_led_count(uint8_t strip, uint16_t count) {
    led_set_count(strip, count);
    dmx_led_recompute();
    led_refresh(strip);
}

static void handle_led_reverse(bool on) {
    dmx_lock(); g_led_settings.reverse = on; dmx_unlock();
}

static void handle_led_mode(bool sequential) {
    g_led_mode = sequential ? LED_MODE_SEQUENTIAL : LED_MODE_PARALLEL;
}

static void handle_led_shift(uint8_t shift) {
    dmx_lock(); g_led_settings.shift = shift; dmx_unlock();
}

static void handle_led_interpolate(bool on) {
    dmx_lock(); g_led_settings.interpolate = on; dmx_unlock();
}

static void handle_dmx_test(uint8_t port, uint8_t channel,
                            uint8_t r, uint8_t g, uint8_t b,
                            bool fill, uint16_t count) {
    dmx_lock();
    g_dmx.tx_port = port; g_dmx.tx_channel = channel;
    g_dmx.tx_r = r; g_dmx.tx_g = g; g_dmx.tx_b = b;
    g_dmx.tx_mode = fill ? TX_MODE_FILL : TX_MODE_POINT;
    g_dmx.tx_count = count;
    dmx_unlock();
    dmx_set_mode(DMX_MODE_TESTER);
}

static void handle_fixture_settings(uint8_t channel_order,
                                    uint8_t fallback_r, uint8_t fallback_g, uint8_t fallback_b,
                                    uint16_t fallback_timeout_ms) {
    dmx_lock();
    g_led_settings.channel_order = channel_order;
    g_led_settings.fallback_r = fallback_r; g_led_settings.fallback_g = fallback_g; g_led_settings.fallback_b = fallback_b;
    g_led_settings.fallback_timeout_ms = fallback_timeout_ms;
    dmx_unlock();
    dmx_led_apply_fallback();
}

/* ===== Init ===== */

void dmx_led_init(const dmx_led_cbs_t *cbs) {
    s_cbs = cbs;
    s_led_refresh_sem = xSemaphoreCreateCounting(10, 0);

    /* Subscribe to DMX frames */
    dmx_on_frame(on_dmx_frame);

    /* Fallback timer */
    const esp_timer_create_args_t timer_args = {
        .callback = &fallback_timer_callback,
        .name     = "dmx_fallback"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_fallback_timer));
    uint16_t fb_timeout = g_led_settings.fallback_timeout_ms;
    if (fb_timeout > 0)
        ESP_ERROR_CHECK(esp_timer_start_once(s_fallback_timer, (uint64_t)fb_timeout * 1000));

    /* LED refresh task */
    xTaskCreatePinnedToCore(led_refresh_task, "led_ref", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "DMX→LED bridge initialized");
}
