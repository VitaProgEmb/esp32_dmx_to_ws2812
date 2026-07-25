/**
 * @file ws2812.c
 * @brief Драйвер WS2812B через RMT — двойная буферизация
 *
 * Поток данных:
 *   set_pixel() ──► active bank ──swap──► refresh bank ──GRB convert──► RMT TX → GPIO
 */

#include "ws2812.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include <string.h>

static const char *TAG = "WS2812";

#define WS2812_T0H_NS  400
#define WS2812_T0L_NS  850
#define WS2812_T1H_NS  800
#define WS2812_T1L_NS  450
#define WS2812_RESET_US 50

ws2812_strip_t g_strip1;
ws2812_strip_t g_strip2;

/* ===== RMT LED Encoder ===== */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_ws2812_encoder_t;

static size_t rmt_encode_ws2812(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                 const void *primary_data, size_t data_size,
                                 rmt_encode_state_t *ret_state) {
    rmt_ws2812_encoder_t *enc = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (enc->state) {
    case 0:
        encoded_symbols += enc->bytes_encoder->encode(enc->bytes_encoder, channel,
                                                      primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* falls through */
    case 1:
        encoded_symbols += enc->copy_encoder->encode(enc->copy_encoder, channel,
                                                     &enc->reset_code, sizeof(enc->reset_code),
                                                     &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            enc->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_ws2812_encoder(rmt_encoder_t *encoder) {
    rmt_ws2812_encoder_t *enc = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t rmt_ws2812_encoder_reset(rmt_encoder_t *encoder) {
    rmt_ws2812_encoder_t *enc = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t create_ws2812_encoder(rmt_encoder_handle_t *ret) {
    rmt_ws2812_encoder_t *enc = calloc(1, sizeof(rmt_ws2812_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->base.encode = rmt_encode_ws2812;
    enc->base.del = rmt_del_ws2812_encoder;
    enc->base.reset = rmt_ws2812_encoder_reset;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H_NS / 100,
                  .level1 = 0, .duration1 = WS2812_T0L_NS / 100 },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H_NS / 100,
                  .level1 = 0, .duration1 = WS2812_T1L_NS / 100 },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
    if (err != ESP_OK) { free(enc); return err; }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
    if (err != ESP_OK) { rmt_del_encoder(enc->bytes_encoder); free(enc); return err; }

    uint32_t reset_ticks = 10000000 / 1000000 * WS2812_RESET_US / 2;
    enc->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = reset_ticks,
        .level1 = 0, .duration1 = reset_ticks,
    };

    *ret = &enc->base;
    return ESP_OK;
}

/* ===== GRB conversion ===== */

static void fill_grb(ws2812_strip_t *strip, uint8_t *grb, uint16_t num) {
    ws2812_color_t *bank = (strip->active == strip->bank_a)
                         ? strip->bank_b : strip->bank_a;
    for (uint16_t i = 0; i < num; i++) {
        grb[i * 3 + 0] = bank[i].g;
        grb[i * 3 + 1] = bank[i].r;
        grb[i * 3 + 2] = bank[i].b;
    }
}

/* ===== Public API ===== */

void ws2812_init(ws2812_strip_t *strip, uint8_t gpio, uint16_t num_leds) {
    if (num_leds > LED_STRIP_MAX_LEDS) num_leds = LED_STRIP_MAX_LEDS;

    strip->gpio = gpio;
    strip->count = num_leds;
    strip->active = strip->bank_a;
    memset(strip->bank_a, 0, sizeof(strip->bank_a));
    memset(strip->bank_b, 0, sizeof(strip->bank_b));

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .trans_queue_depth = 4,
        .mem_block_symbols = 64,
        .flags.invert_out = false,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &strip->rmt_chan));
    ESP_ERROR_CHECK(create_ws2812_encoder(&strip->encoder));
    ESP_ERROR_CHECK(rmt_enable(strip->rmt_chan));

    ESP_LOGI(TAG, "GPIO%d: %d LEDs", gpio, num_leds);
}

void ws2812_set_pixel(ws2812_strip_t *strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= strip->count) return;
    strip->active[idx].r = r;
    strip->active[idx].g = g;
    strip->active[idx].b = b;
}

void ws2812_set_all(ws2812_strip_t *strip, uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < strip->count; i++) {
        strip->active[i].r = r;
        strip->active[i].g = g;
        strip->active[i].b = b;
    }
}

void ws2812_clear(ws2812_strip_t *strip) {
    memset(strip->bank_a, 0, sizeof(strip->bank_a));
    memset(strip->bank_b, 0, sizeof(strip->bank_b));
    ws2812_refresh(strip);
}

void ws2812_swap(ws2812_strip_t *strip) {
    strip->active = (strip->active == strip->bank_a) ? strip->bank_b : strip->bank_a;
}

void ws2812_refresh(ws2812_strip_t *strip) {
    if (strip->count == 0 || !strip->encoder || !strip->rmt_chan) return;

    uint8_t *grb = calloc(strip->count * 3, 1);
    if (!grb) return;

    fill_grb(strip, grb, strip->count);

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(strip->rmt_chan, strip->encoder, grb, strip->count * 3, &tx_cfg);

    free(grb);
}
