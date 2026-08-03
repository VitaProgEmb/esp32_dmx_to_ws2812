/**
 * @file ws2812.c
 * @brief Реализация драйвера WS2812B через RMT — двойная буферизация, два порта
 *
 * Данный модуль реализует управление двумя независимыми адресными светодиодными
 * лентами WS2812B с использованием аппаратного RMT-модуля ESP32.
 *
 * Архитектура буферизации (двойной буфер / double buffering):
 *   - Каждая лента (strip_t) содержит два цветовых банка: bank_a и bank_b.
 *   - Указатель colors определяет текущий "display-банк", данные которого
 *     конвертируются в GRB и передаются через RMT на физическую ленту.
 *   - "Back-банк" используется для записи новых данных (через led_set_pixel
 *     или прямой доступ через led_get_colors). Запись безопасна, пока
 *     display-банк занят передачей.
 *   - Мьютекс s_mutex защищает доступ к указателю colors и банкам.
 *
 * Конвейер передачи:
 *   back-банк (RGB) → fill_grb (конвертация в GRB + reverse/shift) →
 *   → RMT encoder (кодирование в WS2812 timings) → RMT TX → лента
 *
 * Публичный API:
 *   led_init, led_set_pixel, led_show, led_set_count, led_fill, led_clear, led_deinit
 *
 * Внутренний API (для dmx.c):
 *   led_lock, led_unlock, led_get_colors, led_get_count, led_swap_banks, led_refresh
 *
 * @note Протокол WS2812B: 800 кбит/с, формат GRB, reset > 50 мкс.
 * @note РАЗРЕШЕНИЕ RMT: 10 МГц (100 нс/тик), поэтому длительности
 *       кодируются как количество тиков = нс / 100.
 */

#include "ws2812.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

/** @brief Тег для логирования ESP-IDF (используется в ESP_LOGI/ESP_LOGE) */
static const char *TAG = "WS2812";

/* ===== Тайминги WS2812B (в наносекундах) ===== */

/**
 * @name Тайминги протокола WS2812B
 * @{
 */
#define WS2812_T0H_NS   400    /**< @brief Длительность HIGH-импульса для логического «0» (400 нс) */
#define WS2812_T0L_NS   850    /**< @brief Длительность LOW-импульса для логического «0» (850 нс) */
#define WS2812_T1H_NS   800    /**< @brief Длительность HIGH-импульса для логической «1» (800 нс) */
#define WS2812_T1L_NS   450    /**< @brief Длительность LOW-импульса для логической «1» (450 нс) */
#define WS2812_RESET_US 50     /**< @brief Минимальная длительность reset-импульса (50 мкс) */
/** @} */

/** @brief Количество поддерживаемых светодиодных лент (максимум 2) */
#define STRIP_COUNT     2

/* ===== Внутренняя структура ленты ===== */

/**
 * @struct strip_t
 * @brief Внутренняя структура, описывающая одну светодиодную ленту.
 *
 * Содержит два цветовых банка для двойной буферизации, указатель на текущий
 * display-банк, параметры RMT-канала и кодировщика, а также опции
 * реверса и сдвига.
 */
typedef struct {
    led_color_t          bank_a[LED_STRIP_MAX_LEDS]; /**< @brief Цветовой банк A (первый буфер) */
    led_color_t          bank_b[LED_STRIP_MAX_LEDS]; /**< @brief Цветовой банк B (второй буфер) */
    led_color_t         *colors;    /**< @brief Указатель на текущий display-банк (отправляется в RMT) */
    uint16_t             count;     /**< @brief Количество активных светодиодов в ленте */
    rmt_channel_handle_t rmt_chan;  /**< @brief Дескриптор RMT TX-канала */
    rmt_encoder_handle_t encoder;   /**< @brief Дескриптор программного кодировщика WS2812B */
    uint8_t              gpio;      /**< @brief Номер GPIO-вывода ленты */
    bool                 reverse;   /**< @brief Флаг реверса порядка LED (true = инверсия порядка) */
    uint16_t             shift;     /**< @brief Смещение начала вывода (в позициях) */
} strip_t;

/** @brief Массив данных лент (индексы 0 и 1) */
static strip_t      s_strips[STRIP_COUNT];

/** @brief GRB-буферы для каждой ленты (конвертируется из RGB перед отправкой в RMT) */
static uint8_t     *s_grb[STRIP_COUNT];

/** @brief Мьютекс для синхронизации доступа к буферам LED (создаётся при первом led_init) */
static SemaphoreHandle_t s_mutex = NULL;

/**
 * @brief Глобальное количество активных светодиодов на обеих лентах.
 * @see g_total_leds в ws2812.h
 */
volatile uint16_t g_total_leds = 0;

/**
 * @brief Глобальный режим работы двух LED-лент.
 * @see g_led_mode в ws2812.h
 */
volatile led_mode_t g_led_mode = LED_DEFAULT_MODE;

/* ===== RMT LED Encoder ===== */

/**
 * @struct rmt_led_encoder_t
 * @brief Пользовательский RMT-кодировщик для светодиодных лент WS2812B.
 *
 * Наследуется от rmt_encoder_t и добавляет два вложенных кодировщика:
 *   - bytes_encoder: кодирует байты данных (GRB) в WS2812 timing-символы.
 *   - copy_encoder: генерирует reset-импульс после передачи всех данных.
 *
 * Состояние.encoder работает как конечный автомат:
 *   state 0 → передача данных через bytes_encoder
 *   state 1 → передача reset-импульса через copy_encoder
 *   сброс → кодирование завершено
 */
typedef struct {
    rmt_encoder_t base;            /**< @brief Базовый интерфейс RMT-кодировщика */
    rmt_encoder_t *bytes_encoder;  /**< @brief Кодировщик байт (WS2812 timing для 0/1) */
    rmt_encoder_t *copy_encoder;   /**< @brief Кодировщик reset-импульса (копирование) */
    int state;                     /**< @brief Текущее состояние конечного автомата (0 или 1) */
    rmt_symbol_word_t reset_code;  /**< @brief Слово-символ reset-импульса (LOW > 50 мкс) */
} rmt_led_encoder_t;

/**
 * @brief Функция кодирования RMT-кодировщика WS2812B.
 *
 * Реализует конечный автомат для формирования выходного RMT-потока:
 *   1. Передача массива байт (GRB-данные) через bytes_encoder.
 *   2. Генерация reset-импульса через copy_encoder.
 *
 * @param encoder    Указатель на базовый rmt_encoder_t (приводится к rmt_led_encoder_t).
 * @param channel    RMT TX-канал для записи символов.
 * @param primary_data  Указатель на входные данные (массив байт GRB).
 * @param data_size  Размер входных данных в байтах.
 * @param ret_state  Выходной параметр: состояние кодирования (RMT_ENCODING_*).
 *
 * @return Количество закодированных RMT-символов.
 */
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
        /* Этап 1: кодирование данных (GRB-байты → WS2812 timing-символы) */
        encoded_symbols += bytes_enc->encode(bytes_enc, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session_state & RMT_ENCODING_MEM_FULL) { state |= RMT_ENCODING_MEM_FULL; goto out; }
        /* falls through */
    case 1:
        /* Этап 2: генерация reset-импульса (LOW > 50 мкс) */
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

/**
 * @brief Удаление RMT-кодировщика WS2812B и освобождение памяти.
 *
 * Удаляет вложенные bytes_encoder и copy_encoder, затем освобождает
 * память самой структуры rmt_led_encoder_t.
 *
 * @param encoder  Указатель на базовый rmt_encoder_t.
 * @return ESP_OK всегда.
 */
static esp_err_t rmt_del_led_encoder(rmt_encoder_t *encoder) {
    rmt_led_encoder_t *enc = __containerof(encoder, rmt_led_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

/**
 * @brief Сброс состояния RMT-кодировщика WS2812B.
 *
 * Сбрасывает состояние конечного автомата и сбрасывает оба вложенных
 * кодировщика (bytes_encoder и copy_encoder).
 *
 * @param encoder  Указатель на базовый rmt_encoder_t.
 * @return ESP_OK всегда.
 */
static esp_err_t rmt_led_encoder_reset(rmt_encoder_t *encoder) {
    rmt_led_encoder_t *enc = __containerof(encoder, rmt_led_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

/**
 * @brief Создание RMT-кодировщика WS2812B.
 *
 * Выделяет память для rmt_led_encoder_t, настраивает три функции-обработчика
 * (encode, del, reset), создаёт bytes_encoder с таймингами WS2812B
 * и copy_encoder для reset-импульса. Время reset-импульса рассчитывается
 * на основе WS2812_RESET_US при разрешении RMT 10 МГц.
 *
 * @param ret  Выходной параметр: указатель на созданный rmt_encoder_t.
 * @return ESP_OK при успехе, ESP_ERR_NO_MEM при нехватке памяти.
 */
static esp_err_t create_led_encoder(rmt_encoder_handle_t *ret) {
    rmt_led_encoder_t *enc = calloc(1, sizeof(rmt_led_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->base.encode = rmt_encode_led_strip;
    enc->base.del = rmt_del_led_encoder;
    enc->base.reset = rmt_led_encoder_reset;

    /* Настройка bytes_encoder: WS2812B timing для логических 0 и 1 */
    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H_NS / 100, .level1 = 0, .duration1 = WS2812_T0L_NS / 100 },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H_NS / 100, .level1 = 0, .duration1 = WS2812_T1L_NS / 100 },
        .flags.msb_first = 1,  /* MSB-first: старший бит первого байта передаётся первым */
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
    if (err != ESP_OK) { free(enc); return err; }

    /* Настройка copy_encoder: для генерации reset-импульса (копирование одного слова) */
    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
    if (err != ESP_OK) { rmt_del_encoder(enc->bytes_encoder); free(enc); return err; }

    /* Расчёт reset-импульса: разрешение 10 МГц → 1 тик = 100 нс.
     * reset_ticks = (10 МГц / 1 МГц) * WS2812_RESET_US / 2 = 10 * 50 / 2 = 250 тиков.
     * Два полупериода по reset_ticks формируют LOW-импульс нужной длительности. */
    uint32_t reset_ticks = 10000000 / 1000000 * WS2812_RESET_US / 2;
    enc->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = reset_ticks, .level1 = 0, .duration1 = reset_ticks,
    };

    *ret = &enc->base;
    return ESP_OK;
}

/* ===== Конвертация RGB → GRB ===== */

/**
 * @brief Конвертация цветового буфера из формата RGB в формат GRB.
 *
 * Функция выполняет три операции для каждого светодиода:
 *   1. Реверс порядка: если s->reverse == true, индексы инвертируются
 *      (последний элемент массива становится первым при выводе).
 *   2. Циклический смещение (shift): начало вывода сдвигается на s->shift позиций.
 *   3. Конвертация порядка компонент: RGB → GRB (G записывается первым,
 *      затем R, затем B — как требует протокол WS2812B).
 *
 * @param s     Указатель на структуру strip_t (содержит опции reverse/shift).
 * @param grb   Выходной буфер GRB-данных (размер num * 3 байт).
 * @param num   Количество светодиодов для конвертации.
 * @param src   Входной массив цветов в формате RGB (led_color_t[]).
 */
static void fill_grb(strip_t *s, uint8_t *grb, uint16_t num, const led_color_t *src) {
    for (uint16_t i = 0; i < num; i++) {
        /* Реверс: если reverse=true, то i-й LED на ленте получает данные из (num-1-i) позиции */
        uint16_t base = s->reverse ? (num - 1 - i) : i;
        /* Циклическое смещение: начало вывода сдвигается на shift позиций */
        uint16_t src_idx = (base + s->shift) % num;
        /* Конвертация RGB → GRB: WS2812B требует порядок [G, R, B] */
        grb[i * 3 + 0] = src[src_idx].g;
        grb[i * 3 + 1] = src[src_idx].r;
        grb[i * 3 + 2] = src[src_idx].b;
    }
}

/* ===== Public API ===== */

/**
 * @brief Инициализация одной светодиодной ленты WS2812B.
 *
 * Выполняет полную настройку ленты:
 *   - Инициализирует структуру strip_t (банк A, банк B, параметры).
 *   - Создаёт RMT TX-канал с разрешением 10 МГц и глубиной очереди 4.
 *   - Создаёт программный кодировщик WS2812B (rmt_led_encoder_t).
 *   - Включает RMT TX-канал.
 *   - Выделяет GRB-буфер для конвертации данных.
 *   - Обновляет g_total_leds (суммарное количество LED на обеих лентах).
 *   - Создаёт глобальный мьютекс (при первом вызове).
 *
 * @param strip  Индекс ленты: 0 — первая лента, 1 — вторая лента.
 * @param gpio   Номер GPIO-вывода для данных ленты.
 * @param count  Количество светодиодов (не более LED_STRIP_MAX_LEDS).
 *
 * @note При count > LED_STRIP_MAX_LEDS значение автоматически обрезается.
 * @note При ошибке создания RMT-канала лента не будет работать (лог ошибки).
 */
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
    s->colors = s->bank_a;  /* Начальный display-банк = bank_a */

    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    /* Настройка RMT TX-канала */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,  /* 10 МГц → 1 тик = 100 нс */
        .trans_queue_depth = 4,     /* Глубина очереди передач: 4 */
        .mem_block_symbols = 64,    /* Размер блока памяти RMT: 64 символа */
        .flags.invert_out = false,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s->rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Strip %d RMT failed: %s", strip, esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(create_led_encoder(&s->encoder));
    ESP_ERROR_CHECK(rmt_enable(s->rmt_chan));

    /* Выделение GRB-буфера: 3 байта на каждый LED */
    s_grb[strip] = calloc(LED_STRIP_MAX_LEDS * 3, 1);
    if (!s_grb[strip]) ESP_LOGE(TAG, "Strip %d GRB alloc failed", strip);

    /* Пересчёт общего количества LED */
    g_total_leds = s_strips[0].count + s_strips[1].count;
    ESP_LOGI(TAG, "Strip %d: GPIO%d, %d LEDs", strip, gpio, count);
}

/**
 * @brief Установка цвета одного светодиода в back-буфере (без вывода на ленту).
 *
 * Определяет back-банк (банк, который НЕ является текущим display-банком),
 * записывает RGB-значение по указанному индексу. Данные не выводятся
 * на ленту до вызова led_show(). Доступ к буферу защищён мьютексом.
 *
 * @param strip  Индекс ленты: 0 или 1.
 * @param idx    Индекс светодиода (0-based, от 0 до count-1).
 * @param r      Красная компонента цвета (0–255).
 * @param g      Зелёная компонента цвета (0–255).
 * @param b      Синяя компонента цвета (0–255).
 *
 * @note Если idx >= count, запись игнорируется (защита от выхода за границы).
 * @note Потокобезопасно: используется мьютекс для синхронизации.
 */
void led_set_pixel(uint8_t strip, uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    if (idx < s->count) { back[idx].r = r; back[idx].g = g; back[idx].b = b; }
    if (s_mutex) xSemaphoreGive(s_mutex);
}

/**
 * @brief Отправка back-буфера на светодиодную ленту через RMT.
 *
 * Последовательно выполняет:
 *   1. Захват мьютекса и определение back-банка.
 *   2. Проверку валидности параметров (count > 0, encoder и grb-буфер существуют).
 *   3. Конвертацию back-банка из RGB в GRB (с учётом reverse/shift) в s_grb.
 *   4. Ротацию банков: back-банк становится display-банком (s->colors = back).
 *   5. Освобождение мьютекса.
 *   6. Запуск асинхронной передачи через rmt_transmit().
 *
 * @param strip  Индекс ленты: 0 или 1.
 *
 * @note RMT-передача работает асинхронно: функция возвращает управление
 *       сразу после запуска передачи, не ожидая её завершения.
 */
void led_show(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    uint16_t num = s->count;
    if (num == 0 || !s->encoder || !s_grb[strip]) { if (s_mutex) xSemaphoreGive(s_mutex); return; }
    fill_grb(s, s_grb[strip], num, back);
    s->colors = back;  /* Ротация: back-банк становится display-банком */
    if (s_mutex) xSemaphoreGive(s_mutex);

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s->rmt_chan, s->encoder, s_grb[strip], num * 3, &tx_cfg);
}

/**
 * @brief Изменение количества активных светодиодов на ленте «на лету».
 *
 * Обновляет внутренний счётчик count и пересчитывает g_total_leds
 * (суммарное количество LED на обеих лентах). Не влияет на выделенную
 * память (она фиксирована LED_STRIP_MAX_LEDS).
 *
 * @param strip  Индекс ленты: 0 или 1.
 * @param count  Новое количество светодиодов (не более LED_STRIP_MAX_LEDS).
 */
void led_set_count(uint8_t strip, uint16_t count) {
    if (strip >= STRIP_COUNT) return;
    if (count > LED_STRIP_MAX_LEDS) count = LED_STRIP_MAX_LEDS;
    s_strips[strip].count = count;
    g_total_leds = s_strips[0].count + s_strips[1].count;
}

/**
 * @brief Заполнение всех светодиодов ленты одинаковым цветом и вывод на ленту.
 *
 * Устанавливает заданный RGB-цвет для всех LED в back-буфере, затем
 * автоматически вызывает led_show() для обновления вывода.
 * Потокобезопасно: запись в back-банк защищена мьютексом.
 *
 * @param strip  Индекс ленты: 0 или 1.
 * @param r      Красная компонента цвета (0–255).
 * @param g      Зелёная компонента цвета (0–255).
 * @param b      Синяя компонента цвета (0–255).
 */
void led_fill(uint8_t strip, uint8_t r, uint8_t g, uint8_t b) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    for (uint16_t i = 0; i < s->count; i++) { back[i].r = r; back[i].g = g; back[i].b = b; }
    if (s_mutex) xSemaphoreGive(s_mutex);
    led_show(strip);
}

/**
 * @brief Полная очистка светодиодов ленты (установка чёрного цвета) и вывод.
 *
 * Заполняет back-буфер нулевыми значениями (RGB = 0,0,0 → чёрный),
 * затем вызывает led_show() для обновления физического вывода.
 *
 * @param strip  Индекс ленты: 0 или 1.
 */
void led_clear(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t *back = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    memset(back, 0, sizeof(led_color_t) * s->count);
    if (s_mutex) xSemaphoreGive(s_mutex);
    led_show(strip);
}

/**
 * @brief Освобождение всех ресурсов модуля LED-драйвера.
 *
 * Для каждой ленты:
 *   - Освобождает GRB-буфер (s_grb).
 *   - Удаляет RMT-кодировщик (encoder).
 *   - Удаляет RMT TX-канал (rmt_chan).
 * Удаляет глобальный мьютекс s_mutex.
 * Вызывается при завершении работы системы или перезагрузке.
 */
void led_deinit(void) {
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (s_grb[i]) { free(s_grb[i]); s_grb[i] = NULL; }
        if (s_strips[i].encoder) { rmt_del_encoder(s_strips[i].encoder); s_strips[i].encoder = NULL; }
        if (s_strips[i].rmt_chan) { rmt_del_channel(s_strips[i].rmt_chan); s_strips[i].rmt_chan = NULL; }
    }
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
}

/* ===== Internal API (для dmx.c — прямой доступ к back-буферу) ===== */

/**
 * @brief Захват мьютекса для безопасного доступа к LED-буферу.
 *
 * Блокирует мьютекс s_mutex на неограниченное время (portMAX_DELAY).
 * Используется перед прямой записью в back-банк через led_get_colors().
 *
 * @param strip  Индекс ленты (параметр зарезервирован, пока не используется).
 *
 * @warning Обязательно вызвать led_unlock() после завершения работы с буфером.
 */
void led_lock(uint8_t strip) { (void)strip; if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }

/**
 * @brief Освобождение мьютекса LED-буфера.
 *
 * Снимает блокировку, установленную led_lock(). Должен вызываться
 * в паре с led_lock() после завершения записи в back-банк.
 *
 * @param strip  Индекс ленты (параметр зарезервирован, пока не используется).
 */
void led_unlock(uint8_t strip) { (void)strip; if (s_mutex) xSemaphoreGive(s_mutex); }

/**
 * @brief Получение указателя на текущий back-буфер для прямой записи.
 *
 * Возвращает указатель на банк, который НЕ используется для вывода в RMT.
 * Это безопасный для записи банк — можно модифицировать цвета, пока
 * display-банк передаёт данные на ленту.
 *
 * @param strip  Индекс ленты: 0 или 1.
 *
 * @return Указатель на массив led_color_t[] размером LED_STRIP_MAX_LEDS,
 *         или NULL если strip недопустим.
 *
 * @warning Необходимо вызвать led_lock() перед вызовом этой функции
 *          и led_unlock() после завершения записи.
 */
led_color_t *led_get_colors(uint8_t strip) {
    if (strip >= STRIP_COUNT) return NULL;
    strip_t *s = &s_strips[strip];
    return (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
}

/**
 * @brief Получение текущего количества активных светодиодов на ленте.
 *
 * @param strip  Индекс ленты: 0 или 1.
 *
 * @return Количество активных LED (0..LED_STRIP_MAX_LEDS), или 0 если strip недопустим.
 */
uint16_t led_get_count(uint8_t strip) {
    return (strip < STRIP_COUNT) ? s_strips[strip].count : 0;
}

/**
 * @brief Ротация банков (swap): поменять местами display и back-буферы.
 *
 * После вызова указатель s->colors переключается на противоположный банк.
 * Банк, который был back-банком, становится display-банком (готов к выводу).
 * Не выполняет передачу данных на ленту — только переключает указатели.
 * Потокобезопасно: доступ к указателю защищён мьютексом.
 *
 * @param strip  Индекс ленты: 0 или 1.
 */
void led_swap_banks(uint8_t strip) {
    if (strip >= STRIP_COUNT) return;
    strip_t *s = &s_strips[strip];
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s->colors = (s->colors == s->bank_a) ? s->bank_b : s->bank_a;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

/**
 * @brief Обновление вывода на ленту без ротации банков (refresh).
 *
 * Конвертирует текущий display-банк (s->colors) в GRB-формат и отправляет
 * через RMT TX. В отличие от led_show(), НЕ выполняет swap банков:
 * display-банк остаётся тем же. Используется, когда данные уже записаны
 * в display-банк (например, после led_swap_banks или прямой записи).
 *
 * @param strip  Индекс ленты: 0 или 1.
 *
 * @note Потокобезопасно: конвертация выполняется под мьютексом,
 *       передача — после его освобождения.
 */
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

/**
 * @brief Включение/выключение реверса порядка светодиодов на ленте.
 *
 * При включении (on = true) порядок LED инвертируется в функции fill_grb():
 * i-й физический LED получает данные из позиции (num - 1 - i) массива.
 * Полезно для компенсации направления подключения ленты.
 *
 * @param strip  Индекс ленты: 0 или 1.
 * @param on     true — реверс включён, false — реверс выключен.
 *
 * @note Не требует мьютекса: запись в bool атомарна на ESP32.
 */
void led_set_reverse(uint8_t strip, bool on) { if (strip < STRIP_COUNT) s_strips[strip].reverse = on; }

/**
 * @brief Установка смещения (shift) начала вывода на ленте.
 *
 * Сдвигает начало вывода на указанное количество позиций в функции fill_grb().
 * Используется для выравнивания начала данных при последовательном режиме
 * (sequential), когда приборы распределены между двумя DMX-портами.
 *
 * @param strip  Индекс ленты: 0 или 1.
 * @param shift  Количество позиций сдвига (0..count-1).
 *
 * @note Не требует мьютекса: запись в uint16_t атомарна на ESP32.
 */
void led_set_shift(uint8_t strip, uint16_t shift) { if (strip < STRIP_COUNT) s_strips[strip].shift = shift; }
