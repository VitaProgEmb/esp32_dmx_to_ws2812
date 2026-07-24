/**
 * @file led_strip.c
 * @brief Драйвер WS2812B через RMT — двойной канал для двух LED-лент
 *
 * Поддерживает два независимых RMT TX канала:
 *   - Strip 1: GPIO23, RMT канал 0
 *   - Strip 2: GPIO5,  RMT канал 1
 *
 * Два режима работы (led_mode_t):
 *   PARALLEL:   strip1 ← DMX port0, strip2 ← DMX port1 (независимо)
 *   SEQUENTIAL: strip1+strip2 = единая лента 2000 LED (fixtures 0..N-1)
 *
 * Память на ленту: 2 × bank_a + 2 × bank_b + GRB = ~18KB
 */

#include "led_strip.h"
#include "dmx.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LED_STRIP";

/* ======================================================================
 * ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
 * ====================================================================== */

led_strip_t g_led_strip;    /* Strip 1 (GPIO23, RMT ch0) */
led_strip_t g_led_strip2;   /* Strip 2 (GPIO5,  RMT ch1) */
volatile led_mode_t g_led_mode = LED_DEFAULT_MODE;

static rmt_encoder_handle_t s_led_encoder = NULL;
static rmt_encoder_handle_t s_led_encoder2 = NULL;

static SemaphoreHandle_t s_colors_mutex = NULL;

/** GRB byte buffer — формат который ожидает WS2812B (G, R, B на каждый LED) */
static uint8_t *s_grb_buf = NULL;
static uint8_t *s_grb_buf2 = NULL;

/* ======================================================================
 * КОНСТАНТЫ
 * ====================================================================== */

#define RESET_SYMBOLS 200

/* ======================================================================
 * CUSTOM LED STRIP ENCODER (по примеру ESP-IDF)
 * ====================================================================== */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_strip_encoder_t;

static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                    const void *primary_data, size_t data_size,
                                    rmt_encode_state_t *ret_state) {
    rmt_led_strip_encoder_t *led_enc = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_handle_t bytes_enc = led_enc->bytes_encoder;
    rmt_encoder_handle_t copy_enc = led_enc->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_enc->state) {
    case 0:
        encoded_symbols += bytes_enc->encode(bytes_enc, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_enc->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* falls through */
    case 1:
        encoded_symbols += copy_enc->encode(copy_enc, channel, &led_enc->reset_code,
                                            sizeof(led_enc->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_enc->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_encoder(rmt_encoder_t *encoder) {
    rmt_led_strip_encoder_t *led_enc = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_enc->bytes_encoder);
    rmt_del_encoder(led_enc->copy_encoder);
    free(led_enc);
    return ESP_OK;
}

static esp_err_t rmt_led_encoder_reset(rmt_encoder_t *encoder) {
    rmt_led_strip_encoder_t *led_enc = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_enc->bytes_encoder);
    rmt_encoder_reset(led_enc->copy_encoder);
    led_enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t create_led_strip_encoder(rmt_encoder_handle_t *ret) {
    rmt_led_strip_encoder_t *enc = calloc(1, sizeof(rmt_led_strip_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->base.encode = rmt_encode_led_strip;
    enc->base.del = rmt_del_led_encoder;
    enc->base.reset = rmt_led_encoder_reset;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = LED_T0H_NS / 100,
            .level1 = 0,
            .duration1 = LED_T0L_NS / 100,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = LED_T1H_NS / 100,
            .level1 = 0,
            .duration1 = LED_T1L_NS / 100,
        },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
    if (err != ESP_OK) { free(enc); return err; }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
    if (err != ESP_OK) { rmt_del_encoder(enc->bytes_encoder); free(enc); return err; }

    uint32_t reset_ticks = 10000000 / 1000000 * 50 / 2;
    enc->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    *ret = &enc->base;
    return ESP_OK;
}

/* ======================================================================
 * ИНИЦИАЛИЗАЦИЯ
 * ====================================================================== */

/**
 * @brief Инициализация Strip 1 (GPIO23, RMT канал 0)
 */
void led_strip_init(uint16_t num_leds) {
    if (num_leds > LED_STRIP_MAX_LEDS)
        num_leds = LED_STRIP_MAX_LEDS;

    g_led_strip.gpio = LED_STRIP_GPIO;
    g_led_strip.count = num_leds;
    memset(g_led_strip.bank_a, 0, sizeof(g_led_strip.bank_a));
    memset(g_led_strip.bank_b, 0, sizeof(g_led_strip.bank_b));
    g_led_strip.colors = g_led_strip.bank_a;

    s_colors_mutex = xSemaphoreCreateMutex();

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num     = LED_STRIP_GPIO,
        .clk_src      = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .trans_queue_depth = 4,
        .mem_block_symbols = 64,
        .flags.invert_out  = false,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &g_led_strip.rmt_chan));
    ESP_ERROR_CHECK(create_led_strip_encoder(&s_led_encoder));
    ESP_ERROR_CHECK(rmt_enable(g_led_strip.rmt_chan));

    s_grb_buf = calloc(LED_STRIP_MAX_LEDS * 3, 1);
    if (!s_grb_buf) ESP_LOGE(TAG, "Failed to allocate GRB buffer 1");
}

/**
 * @brief Инициализация Strip 2 (GPIO5, RMT канал 1)
 *
 * Вызывается из app_main() после led_strip_init().
 * Использует отдельный RMT канал и отдельный GRB-буфер.
 */
void led_strip_init2(uint16_t num_leds) {
    if (num_leds > LED_STRIP_MAX_LEDS)
        num_leds = LED_STRIP_MAX_LEDS;

    g_led_strip2.gpio = LED_STRIP_GPIO2;
    g_led_strip2.count = num_leds;
    memset(g_led_strip2.bank_a, 0, sizeof(g_led_strip2.bank_a));
    memset(g_led_strip2.bank_b, 0, sizeof(g_led_strip2.bank_b));
    g_led_strip2.colors = g_led_strip2.bank_a;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num     = LED_STRIP_GPIO2,
        .clk_src      = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .trans_queue_depth = 4,
        .mem_block_symbols = 64,
        .flags.invert_out  = false,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &g_led_strip2.rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT channel 2: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(create_led_strip_encoder(&s_led_encoder2));
    ESP_ERROR_CHECK(rmt_enable(g_led_strip2.rmt_chan));

    s_grb_buf2 = calloc(LED_STRIP_MAX_LEDS * 3, 1);
    if (!s_grb_buf2) ESP_LOGE(TAG, "Failed to allocate GRB buffer 2");

    ESP_LOGI(TAG, "Strip 2: GPIO%d, %d LEDs", LED_STRIP_GPIO2, num_leds);
}

/* ======================================================================
 * УПРАВЛЕНИЕ ПИКСЕЛЯМИ
 * ====================================================================== */

void led_strip_set_pixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    led_strip_lock();
    if (idx < g_led_strip.count) {
        g_led_strip.colors[idx].r = r;
        g_led_strip.colors[idx].g = g;
        g_led_strip.colors[idx].b = b;
    }
    led_strip_unlock();
}

void led_strip_set_all(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_lock();
    for (uint16_t i = 0; i < g_led_strip.count; i++) {
        g_led_strip.colors[i].r = r;
        g_led_strip.colors[i].g = g;
        g_led_strip.colors[i].b = b;
    }
    led_strip_unlock();
}

void led_strip_set_pixels(led_color_t *colors, uint16_t count) {
    led_strip_lock();
    uint16_t n = (count > g_led_strip.count) ? g_led_strip.count : count;
    memcpy(g_led_strip.colors, colors, n * sizeof(led_color_t));
    led_strip_unlock();
}

/* ======================================================================
 * ДВОЙНАЯ БУФЕРИЗАЦИЯ
 * ====================================================================== */

void led_strip_swap_banks(void) {
    led_strip_lock();
    led_color_t *other = (g_led_strip.colors == g_led_strip.bank_a)
                       ? g_led_strip.bank_b : g_led_strip.bank_a;
    g_led_strip.colors = other;
    led_strip_unlock();
}

void led_strip_swap_banks2(void) {
    led_strip_lock();
    led_color_t *other = (g_led_strip2.colors == g_led_strip2.bank_a)
                       ? g_led_strip2.bank_b : g_led_strip2.bank_a;
    g_led_strip2.colors = other;
    led_strip_unlock();
}

void led_strip_set_pixel_front(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= g_led_strip.count) return;
    led_color_t *front = (g_led_strip.colors == g_led_strip.bank_a)
                       ? g_led_strip.bank_b : g_led_strip.bank_a;
    front[idx].r = r;
    front[idx].g = g;
    front[idx].b = b;
}

void led_strip_swap(void) {
    led_color_t *front = (g_led_strip.colors == g_led_strip.bank_a)
                       ? g_led_strip.bank_b : g_led_strip.bank_a;
    g_led_strip.colors = front;
    led_strip_refresh();
}

/* ======================================================================
 * ОБНОВЛЕНИЕ ЛЕНТЫ
 * ====================================================================== */

static void fill_grb_buf(led_strip_t *strip, uint8_t *grb, uint16_t num) {
    bool reverse = g_dmx.led_reverse;
    uint16_t shift = g_dmx.led_shift;
    for (uint16_t i = 0; i < num; i++) {
        uint16_t base = reverse ? (num - 1 - i) : i;
        uint16_t src_idx = (base + shift) % num;
        led_color_t c = strip->colors[src_idx];
        grb[i * 3 + 0] = c.g;
        grb[i * 3 + 1] = c.r;
        grb[i * 3 + 2] = c.b;
    }
}

void led_strip_refresh(void) {
    led_strip_lock();
    uint16_t num = g_led_strip.count;
    led_strip_unlock();

    if (num == 0 || !s_led_encoder || !s_grb_buf) return;

    led_strip_lock();
    fill_grb_buf(&g_led_strip, s_grb_buf, num);
    led_strip_unlock();

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(g_led_strip.rmt_chan, s_led_encoder, s_grb_buf, num * 3, &tx_cfg);
}

void led_strip_refresh2(void) {
    led_strip_lock();
    uint16_t num = g_led_strip2.count;
    led_strip_unlock();

    if (num == 0 || !s_led_encoder2 || !s_grb_buf2) return;

    led_strip_lock();
    fill_grb_buf(&g_led_strip2, s_grb_buf2, num);
    led_strip_unlock();

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(g_led_strip2.rmt_chan, s_led_encoder2, s_grb_buf2, num * 3, &tx_cfg);
}

/* ======================================================================
 * УТИЛИТЫ
 * ====================================================================== */

void led_strip_clear(void) {
    led_strip_lock();
    memset(g_led_strip.bank_a, 0, sizeof(g_led_strip.bank_a));
    memset(g_led_strip.bank_b, 0, sizeof(g_led_strip.bank_b));
    if (g_led_strip2.count > 0) {
        memset(g_led_strip2.bank_a, 0, sizeof(g_led_strip2.bank_a));
        memset(g_led_strip2.bank_b, 0, sizeof(g_led_strip2.bank_b));
    }
    led_strip_unlock();
    led_strip_refresh();
    led_strip_refresh2();
}

void led_strip_deinit(void) {
    if (s_grb_buf)     { free(s_grb_buf);  s_grb_buf = NULL; }
    if (s_grb_buf2)    { free(s_grb_buf2); s_grb_buf2 = NULL; }
    if (s_led_encoder)  { rmt_del_encoder(s_led_encoder); s_led_encoder = NULL; }
    if (s_led_encoder2) { rmt_del_encoder(s_led_encoder2); s_led_encoder2 = NULL; }
    if (g_led_strip.rmt_chan)  { rmt_del_channel(g_led_strip.rmt_chan); g_led_strip.rmt_chan = NULL; }
    if (g_led_strip2.rmt_chan) { rmt_del_channel(g_led_strip2.rmt_chan); g_led_strip2.rmt_chan = NULL; }
    if (s_colors_mutex) { vSemaphoreDelete(s_colors_mutex); s_colors_mutex = NULL; }
}

/* ======================================================================
 * БЛОКИРОВКИ
 * ====================================================================== */

void led_strip_lock(void) {
    if (s_colors_mutex) xSemaphoreTake(s_colors_mutex, portMAX_DELAY);
}

void led_strip_unlock(void) {
    if (s_colors_mutex) xSemaphoreGive(s_colors_mutex);
}
