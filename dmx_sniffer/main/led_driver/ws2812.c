/**
 * @file ws2812.c
 * @brief WS2812B LED-драйвер через RMT — двойная буферизация, два порта
 *
 * Буферы:
 *   colors = указатель на display-буфер (передаётся в RMT)
 *   led_get_colors() = указатель на back-буфер (безопасен для записи)
 *   led_show() = отправка back → RMT, затем swap
 *
 * Публичный API:  led_init / led_set_pixel / led_show / led_set_count / led_fill / led_clear
 * Внутренний API: led_lock / led_unlock / led_get_colors / led_get_count / led_swap_banks / led_refresh
 */

#include "ws2812.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WS2812";

#define WS2812_T0H_NS   400
#define WS2812_T0L_NS   850
#define WS2812_T1H_NS   800
#define WS2812_T1L_NS   450
#define WS2812_RESET_US 50
#define STRIP_COUNT     2

typedef struct {
    led_color_t          bank_a[LED_STRIP_MAX_LEDS];
    led_color_t          bank_b[LED_STRIP_MAX_LEDS];
    led_color_t         *colors;    /* display-буфер (отправляется в RMT) */
    uint16_t             count;
    rmt_channel_handle_t rmt_chan;
    rmt_encoder_handle_t encoder;
    uint8_t              gpio;
    bool                 reverse;
    uint16_t             shift;
} strip_t;

static strip_t      s_strips[STRIP_COUNT];
static uint8_t     *s_grb[STRIP_COUNT];
static SemaphoreHandle_t s_mutex = NULL;

volatile uint16_t g_total_leds = 0;
volatile led_mode_t g_led_mode = LED_DEFAULT_MODE;

/* ===== RMT LED Encoder ===== */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_encoder_t;

static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                    const void *primary_data, size_t data_size,
                                    rmt_encode_state_t *ret_state) {
    rmt_led_encoder_t *enc = __containerof(encoder, rmt_led_encoder_t, base);
    rmt_encoder_handle_t bytes_enc = enc->bytes_encoder;
    rmt_encoder_handle_t copy_enc = enc->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (enc->state) {
    case 0:
        encoded_symbols += bytes_enc->encode(bytes_enc, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session_state & RMT_ENCODING_MEM_FULL) { state |= RMT_ENCODING_MEM_FULL; goto out; }
        /* falls through */
    case 1:
        encoded_symbols += copy_enc->encode(copy_enc, channel, &enc->reset_code,
                                            sizeof(enc->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) { enc->state = RMT_ENCODING_RESET; state |= RMT_ENCODING_COMPLETE; }
        if (session_state & RMT_ENCODING_MEM_FULL) state |= RMT_ENCODING_MEM_FULL;
        break;
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_encoder(rmt_encoder_t *encoder) {
    rmt_led_encoder_t *enc = __containerof(encoder, rmt_led_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t rmt_led_encoder_reset(rmt_encoder_t *encoder) {
    rmt_led_encoder_t *enc = __containerof(encoder, rmt_led_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t create_led_encoder(rmt_encoder_handle_t *ret) {
    rmt_led_encoder_t *enc = calloc(1, sizeof(rmt_led_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->base.encode = rmt_encode_led_strip;
    enc->base.del = rmt_del_led_encoder;
    enc->base.reset = rmt_led_encoder_reset;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H_NS / 100, .level1 = 0, .duration1 = WS2812_T0L_NS / 100 },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H_NS / 100, .level1 = 0, .duration1 = WS2812_T1L_NS / 100 },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
    if (err != ESP_OK) { free(enc); return err; }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
    if (err != ESP_OK) { rmt_del_encoder(enc->bytes_encoder); free(enc); return err; }

    uint32_t reset_ticks = 10000000 / 1000000 * WS2812_RESET_US / 2;
    enc->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = reset_ticks, .level1 = 0, .duration1 = reset_ticks,
    };

    *ret = &enc->base;
    return ESP_OK;
}

/* ===== GRB conversion ===== */

static void fill_grb(strip_t *s, uint8_t *grb, uint16_t num, const led_color_t *src) {
    for (uint16_t i = 0; i < num; i++) {
        uint16_t base = s->reverse ? (num - 1 - i) : i;
        uint16_t src_idx = (base + s->shift) % num;
        grb[i * 3 + 0] = src[src_idx].g;
        grb[i * 3 + 1] = src[src_idx].r;
        grb[i * 3 + 2] = src[src_idx].b;
    }
}

/* ===== Public API ===== */

void led_init(uint8_t strip, uint8_t gpio, uint16_t count) {
    if (strip >= STRIP_COUNT) return;
    if (count > LED_STRIP_MAX_LEDS) count = LED_STRIP_MAX_LEDS;

    strip_t *s = &s_strips[strip];
    s->gpio = gpio;
    s->count = count;
    s->reverse = false;
    s->shift = 0;
    memset(s->bank_a, 0, sizeof(s->bank_a));
    memset(s->bank_b, 0, sizeof(s->bank_b));
    s->colors = s->bank_a;

    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .trans_queue_depth = 4,
        .mem_block_symbols = 64,
        .flags.invert_out = false,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s->rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Strip %d RMT failed: %s", strip, esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(create_led_encoder(&s->encoder));
    ESP_ERROR_CHECK(rmt_enable(s->rmt_chan));

    s_grb[strip] = calloc(LED_STRIP_MAX_LEDS * 3, 1);
    if (!s_grb[strip]) ESP_LOGE(TAG, "Strip %d GRB alloc failed", strip);

    g_total_leds = s_strips[0].count + s_strips[1].count;
    ESP_LOGI(TAG, "Strip %d: GPIO%d, %d LEDs", strip, gpio, count);
}

void led_set_pixel(uint8_t strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    if (idx < s->count) { back[idx].r = r; back[idx].g = g; back[idx].b = b; }
    if (s_mutex) xSemaphoreGive(s_mutex);
}

void led_show(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    uint16_t num = s->count;
    if (num == 0 || !s->encoder || !s_grb[strip]) { if (s_mutex) xSemaphoreGive(s_mutex); return; }
    fill_grb(s, s_grb[strip], num, back);
    s->colors = back;
    if (s_mutex) xSemaphoreGive(s_mutex);

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s->rmt_chan, s->encoder, s_grb[strip], num * 3, &tx_cfg);
}

void led_set_count(uint8_t strip, uint16_t count) {
    if (strip >= STRIP_COUNT) return;
    if (count > LED_STRIP_MAX_LEDS) count = LED_STRIP_MAX_LEDS;
    s_strips[strip].count = count;
    g_total_leds = s_strips[0].count + s_strips[1].count;
}

void led_fill(uint8_t strip, uint8_t r, uint8_t g, uint8_t b) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    for (uint16_t i = 0; i < s->count; i++) { back[i].r = r; back[i].g = g; back[i].b = b; }
    if (s_mutex) xSemaphoreGive(s_mutex);
    led_show(strip);
}

void led_clear(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    memset(back, 0, sizeof(led_color_t) * s->count);
    if (s_mutex) xSemaphoreGive(s_mutex);
    led_show(strip);
}

void led_deinit(void) {
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (s_grb[i]) { free(s_grb[i]); s_grb[i] = NULL; }
        if (s_strips[i].encoder) { rmt_del_encoder(s_strips[i].encoder); s_strips[i].encoder = NULL; }
        if (s_strips[i].rmt_chan) { rmt_del_channel(s_strips[i].rmt_chan); s_strips[i].rmt_chan = NULL; }
    }
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
}

/* ===== Internal API (for dmx.c — прямой доступ к back-буферу) ===== */

void led_lock(uint8_t strip) { (void)strip; if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
void led_unlock(uint8_t strip) { (void)strip; if (s_mutex) xSemaphoreGive(s_mutex); }

led_color_t *led_get_colors(uint8_t strip) {
    if (strip >= STRIP_COUNT) return NULL;
    strip_t *s = &s_strips[strip];
    return (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
}

uint16_t led_get_count(uint8_t strip) {
    return (strip < STRIP_COUNT) ? s_strips[strip].count : 0;
}

void led_swap_banks(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s->colors = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

void led_refresh(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t num = s->count;
    if (num == 0 || !s->encoder || !s_grb[strip]) { if (s_mutex) xSemaphoreGive(s_mutex); return; }
    fill_grb(s, s_grb[strip], num, s->colors);
    if (s_mutex) xSemaphoreGive(s_mutex);
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s->rmt_chan, s->encoder, s_grb[strip], num * 3, &tx_cfg);
}

void led_set_reverse(uint8_t strip, bool on) { if (strip < STRIP_COUNT) s_strips[strip].reverse = on; }
void led_set_shift(uint8_t strip, uint16_t shift) { if (strip < STRIP_COUNT) s_strips[strip].shift = shift; }
