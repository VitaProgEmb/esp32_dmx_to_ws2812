/**
 * @file web_server.c
 * @brief HTTP-сервер управления устройством (REST API + веб-интерфейс)
 *
 * ========================================================================
 * АРХИТЕКТУРА
 * ========================================================================
 *
 * Встроенный HTTP-сервер ESP-IDF (esp_http_server) на порту 80.
 * Обрабатывает запросы от веб-интерфейса (web_page.h) и мобильных клиентов.
 *
 * ========================================================================
 * REST API (эндпоинты)
 * ========================================================================
 *
 * GET  /                    — отдаёт HTML-страницу веб-интерфейса
 * GET  /api/settings        — текущие настройки устройства (JSON)
 * GET  /api/status          — то же что /api/settings (совместимость)
 * GET  /api/channels        — сырые DMX-данные обоих портов (binary)
 * GET  /api/dmx_channels    — alias для /api/channels
 * GET  /api/led_preview     — текущие цвета LED-ленты (binary, N×3 байт)
 * GET  /api/patch           — таблица патчей (JSON массив)
 * GET  /api/patch.csv       — таблица патчей (CSV формат)
 *
 * POST /api/mode            — переключить режим ({"mode":"sniffer"/"tester"/"patch"})
 * POST /api/leds            — установить количество LED ({"count":100})
 * POST /api/test            — настроить DMX-тестер (port, channel, r/g/b, mode)
 * POST /api/led_test        — тест LED-ленты (pixel, r/g/b, mode: point/fill/rainbow/clear)
 * POST /api/led_reverse     — включить/выключить реверс LED ({"reverse":true})
 * POST /api/dmx_addr_test   — быстрый тест DMX-адреса (line, addr, r/g/b)
 * POST /api/fixture_settings — настройки прибора (channel_order, fallback_r/g/b, timeout)
 * POST /api/interpolate     — вкл/выкл интерполяцию ({"interpolate":true})
 * POST /api/speed           — заглушка (совместимость со старым UI)
 * POST /api/patch           — сохранить таблицу патчей (JSON или CSV)
 * POST /api/patch/nav       — навигация по патчу (cursor, add/skip/prev/next)
 * POST /api/patch/range     — массовое добавление/удаление диапазона патчей
 * POST /api/reboot          — перезагрузка ESP32
 */

#include "web_server.h"
#include "web_page.h"
#include "dmx.h"
#include "dmx_hal.h"
#include "led_strip.h"
#include "patch_manager.h"
#include "settings_manager.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WEB_SRV";
static httpd_handle_t s_server = NULL;

/* ======================================================================
 * ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ (доступны из main.c для радуги)
 * ====================================================================== */

/** Режим ручного теста LED: 0=выкл, 1=статический, 2=радуга */
volatile int g_led_test_mode = 0;

/** Начальный пиксель для теста */
volatile int g_led_test_px = 0;

/** Количество пикселей для теста (fill/rainbow) */
volatile int g_led_test_count = 100;

/* ======================================================================
 * ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
 * ====================================================================== */

/**
 * @brief Собрать JSON со всеми текущими настройками устройства
 *
 * Формирует JSON-строку с:
 *   - Текущий режим (sniffer/tester/patch)
 *   - Параметры LED (количество, реверс, порядок каналов)
 *   - Fallback-цвет и таймаут
 *   - Статус DMX-портов (есть ли сигнал, время последнего приёма)
 *   - Флаг интерполяции
 *   - Фиксированные значения (скорость, количество каналов)
 *
 * Используется эндпоинтами /api/settings и /api/status.
 *
 * @param json  Выходной буфер
 * @param size  Размер буфера
 */
static void build_settings_json(char *json, size_t size) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* Определяем наличие DMX-сигнала: если последний кадр был < 2 сек назад */
    int dmx0 = (g_dmx.last_rx_ms[0] != 0 && (now_ms - g_dmx.last_rx_ms[0]) < 2000) ? 1 : 0;
    int dmx1 = (g_dmx.last_rx_ms[1] != 0 && (now_ms - g_dmx.last_rx_ms[1]) < 2000) ? 1 : 0;

    snprintf(json, size,
        "{"
        "\"mode\":\"%s\","
        "\"led_count\":%d,"
        "\"tx_mode\":\"%s\","
        "\"led_reverse\":%s,"
        "\"led_shift\":%d,"
        "\"led_mode\":\"%s\","
        "\"led_count2\":%d,"
        "\"channel_order\":%d,"
        "\"fallback_r\":%d,\"fallback_g\":%d,\"fallback_b\":%d,"
        "\"fallback_timeout_ms\":%d,"
        "\"interpolate\":%s,"
        "\"dmx0\":%d,\"dmx1\":%d,"
        "\"rx_ms0\":%lu,\"rx_ms1\":%lu,"
        "\"speed0\":250000,\"speed1\":250000,"
        "\"max_channels\":512,"
        "\"channels_per_fixture\":3,"
        "\"tx_speed\":250000,\"tx_packet_len\":512"
        "}",
        g_dmx.mode == DMX_MODE_TESTER ? "tester" :
        g_dmx.mode == DMX_MODE_PATCH ? "patch" : "sniffer",
        g_led_strip.count,
        g_dmx.tx_mode == TX_MODE_FILL ? "fill" : "point",
        g_dmx.led_reverse ? "true" : "false",
        (int)g_dmx.led_shift,
        g_led_mode == LED_MODE_SEQUENTIAL ? "sequential" : "parallel",
        g_led_strip2.count,
        (int)g_dmx.channel_order,
        g_dmx.fallback_r, g_dmx.fallback_g, g_dmx.fallback_b,
        g_dmx.fallback_timeout_ms,
        g_dmx.interpolate ? "true" : "false",
        dmx0, dmx1,
        g_dmx.last_rx_ms[0], g_dmx.last_rx_ms[1]);
}

/* ======================================================================
 * ОБРАБОТЧИКИ HTTP-ЗАПРОСОВ
 * ====================================================================== */

/** GET / — отдаёт HTML веб-интерфейса */
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    const char *ptr = index_html;
    size_t remaining = strlen(index_html);
    const size_t chunk_size = 4096;
    while (remaining > 0) {
        size_t to_send = (remaining < chunk_size) ? remaining : chunk_size;
        esp_err_t ret = httpd_resp_send_chunk(req, ptr, to_send);
        if (ret != ESP_OK) return ret;
        ptr += to_send;
        remaining -= to_send;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/** POST /api/mode — переключение режима (sniffer/tester/patch) */
static esp_err_t api_mode_handler(httpd_req_t *req) {
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(mode)) {
        if (strcmp(mode->valuestring, "tester") == 0) dmx_set_mode(DMX_MODE_TESTER);
        else if (strcmp(mode->valuestring, "patch") == 0) dmx_set_mode(DMX_MODE_PATCH);
        else dmx_set_mode(DMX_MODE_SNIFFER);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** GET /api/settings — текущие настройки (JSON) */
static esp_err_t api_settings_handler(httpd_req_t *req) {
    char json[1024];
    build_settings_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/** GET /api/status — текущий статус (дублирует /api/settings для совместимости) */
static esp_err_t api_status_handler(httpd_req_t *req) {
    char json[1024];
    build_settings_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/** POST /api/leds — установить количество LED в ленте */
static esp_err_t api_leds_handler(httpd_req_t *req) {
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    if (cJSON_IsNumber(cnt)) {
        int n = cnt->valueint;
        if (n < 1) n = 1;
        if (n > LED_STRIP_MAX_LEDS) n = LED_STRIP_MAX_LEDS;
        led_strip_lock();
        g_led_strip.count = n;
        g_total_leds = g_led_strip.count + g_led_strip2.count;
        led_strip_unlock();
    }
    cJSON_Delete(root);
    dmx_recompute_lookups();
    led_strip_refresh();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/leds2 — количество LED Strip 2 (GPIO5) */
static esp_err_t api_leds2_handler(httpd_req_t *req) {
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    if (cJSON_IsNumber(cnt)) {
        int n = cnt->valueint;
        if (n < 0) n = 0;
        if (n > LED_STRIP_MAX_LEDS) n = LED_STRIP_MAX_LEDS;
        led_strip_lock();
        g_led_strip2.count = n;
        g_total_leds = g_led_strip.count + g_led_strip2.count;
        led_strip_unlock();
    }
    cJSON_Delete(root);
    dmx_recompute_lookups();
    led_strip_refresh2();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/test — настройка DMX-тестера (цвет, адрес, режим, количество каналов) */
static esp_err_t api_test_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *jline  = cJSON_GetObjectItem(root, "line");
    cJSON *jch    = cJSON_GetObjectItem(root, "channel");
    cJSON *jr     = cJSON_GetObjectItem(root, "r");
    cJSON *jg     = cJSON_GetObjectItem(root, "g");
    cJSON *jb     = cJSON_GetObjectItem(root, "b");
    cJSON *jmode  = cJSON_GetObjectItem(root, "mode");
    cJSON *jcount = cJSON_GetObjectItem(root, "count");

    if (cJSON_IsNumber(jline) && cJSON_IsNumber(jch)) {
        int line = jline->valueint;
        int ch = jch->valueint;
        if (line >= 0 && line <= DMX_PORT_COUNT && ch >= 1 && ch <= DMX_CHANNELS) {
            dmx_lock();
            g_dmx.tx_port = line;
            g_dmx.tx_channel = ch;
            dmx_unlock();
        }
    }
    dmx_lock();
    if (cJSON_IsNumber(jr)) { int v = jr->valueint; if (v<0)v=0; if(v>255)v=255; g_dmx.tx_r=v; }
    if (cJSON_IsNumber(jg)) { int v = jg->valueint; if (v<0)v=0; if(v>255)v=255; g_dmx.tx_g=v; }
    if (cJSON_IsNumber(jb)) { int v = jb->valueint; if (v<0)v=0; if(v>255)v=255; g_dmx.tx_b=v; }
    if (cJSON_IsString(jmode)) {
        g_dmx.tx_mode = (strcmp(jmode->valuestring, "fill") == 0) ? TX_MODE_FILL : TX_MODE_POINT;
    }
    if (cJSON_IsNumber(jcount)) {
        int v = jcount->valueint; if (v<1)v=1; if(v>DMX_CHANNELS)v=DMX_CHANNELS;
        g_dmx.tx_count = v;
    }
    dmx_unlock();
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** GET /api/patch — получить таблицу патчей в формате JSON */
static esp_err_t api_patch_get_handler(httpd_req_t *req) {
    char *json = patch_to_json_string();
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_sendstr(req, "[]");
    }
    return ESP_OK;
}

/** GET /api/patch.csv — получить таблицу патчей в формате CSV */
static esp_err_t api_patch_csv_handler(httpd_req_t *req) {
    char *csv = patch_to_csv_string();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (csv) {
        httpd_resp_sendstr(req, csv);
        free(csv);
    } else {
        httpd_resp_sendstr(req, "universe,skip\n");
    }
    return ESP_OK;
}

/** POST /api/patch — сохранить таблицу патчей (принимает JSON или CSV) */
static esp_err_t api_patch_post_handler(httpd_req_t *req) {
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 32768) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* Читаем тело запроса (может приходить частями) */
    int total = 0;
    int read;
    while (total < content_len && (read = httpd_req_recv(req, buf + total, content_len - total)) > 0) {
        total += read;
    }
    buf[total] = '\0';

    /* Определяем формат: JSON (начинается с '[') или CSV */
    if (buf[0] == '[') {
        cJSON *root = cJSON_Parse(buf);
        if (root && cJSON_IsArray(root)) {
            dmx_lock();
            g_patch.count = 0;
            int n = cJSON_GetArraySize(root);
            if (n > PATCH_MAX_ENTRIES) n = PATCH_MAX_ENTRIES;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(root, i);
                cJSON *jaddr = cJSON_GetObjectItem(item, "dmx_addr");
                cJSON *juni  = cJSON_GetObjectItem(item, "universe");
                uint16_t idx = g_patch.count;
                int dmx_addr = 1;
                if (cJSON_IsNumber(jaddr)) {
                    dmx_addr = jaddr->valueint;
                    if (dmx_addr < 1) dmx_addr = 1;
                    if (dmx_addr > 512) dmx_addr = 512;
                }
                g_patch.entries[idx].dmx_addr = dmx_addr;
                g_patch.entries[idx].universe = (cJSON_IsNumber(juni) && juni->valueint == 2) ? 2 : 1;
                g_patch.entries[idx].skip = false;
                g_patch.count++;
            }
            dmx_unlock();
        }
        if (root) cJSON_Delete(root);
    } else {
        patch_apply_from_csv(buf);
    }
    free(buf);
    patch_save();
    dmx_recompute_lookups();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/patch/nav — навигация по таблице патчей (курсор, добавление/пропуск) */
static esp_err_t api_patch_nav_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *jcursor = cJSON_GetObjectItem(root, "cursor");
    cJSON *juni = cJSON_GetObjectItem(root, "universe");
    cJSON *jadd = cJSON_GetObjectItem(root, "add");
    cJSON *jskip = cJSON_GetObjectItem(root, "skip");
    cJSON *jprev = cJSON_GetObjectItem(root, "prev");
    cJSON *jnext = cJSON_GetObjectItem(root, "next");

    dmx_lock();
    if (cJSON_IsNumber(jcursor)) {
        int v = jcursor->valueint;
        if (v >= 0 && v < PATCH_MAX_ENTRIES) g_dmx.patch_cursor = v;
    }
    if (cJSON_IsNumber(juni)) {
        int v = juni->valueint;
        if (v == 1 || v == 2) g_dmx.patch_universe = v;
    }
    if (cJSON_IsTrue(jadd)) {
        int c = g_dmx.patch_cursor;
        if (c >= 0 && c < PATCH_MAX_ENTRIES && c < g_patch.count) {
            g_patch.entries[c].skip = false;
            g_patch.entries[c].universe = g_dmx.patch_universe;
        } else if (c >= g_patch.count && c < PATCH_MAX_ENTRIES) {
            g_patch.entries[c].universe = g_dmx.patch_universe;
            g_patch.entries[c].skip = false;
            g_patch.count = c + 1;
        }
        if (c + 1 < PATCH_MAX_ENTRIES) g_dmx.patch_cursor = c + 1;
    }
    if (cJSON_IsTrue(jskip)) {
        int c = g_dmx.patch_cursor;
        if (c >= 0 && c < g_patch.count) {
            g_patch.entries[c].skip = true;
        } else if (c >= g_patch.count && c < PATCH_MAX_ENTRIES) {
            g_patch.entries[c].universe = g_dmx.patch_universe;
            g_patch.entries[c].skip = true;
            g_patch.count = c + 1;
        }
        if (c + 1 < PATCH_MAX_ENTRIES) g_dmx.patch_cursor = c + 1;
    }
    if (cJSON_IsTrue(jprev)) {
        int c = g_dmx.patch_cursor;
        if (c > 0) g_dmx.patch_cursor = c - 1;
    }
    if (cJSON_IsTrue(jnext)) {
        int c = g_dmx.patch_cursor;
        if (c + 1 < PATCH_MAX_ENTRIES) g_dmx.patch_cursor = c + 1;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"cursor\":%d,\"universe\":%d,\"count\":%d}",
        g_dmx.patch_cursor, g_dmx.patch_universe, g_patch.count);
    dmx_unlock();

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

/** POST /api/patch/range — массовое добавление/удаление диапазона патчей */
static esp_err_t api_patch_range_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *jaction = cJSON_GetObjectItem(root, "action");
    cJSON *jstart  = cJSON_GetObjectItem(root, "start");
    cJSON *jcount  = cJSON_GetObjectItem(root, "count");
    cJSON *juni    = cJSON_GetObjectItem(root, "universe");

    bool ok = false;
    if (cJSON_IsString(jaction) && cJSON_IsNumber(jstart) && cJSON_IsNumber(jcount)) {
        int start = jstart->valueint;
        int count = jcount->valueint;
        if (start >= 0 && count >= 1 && start + count <= PATCH_MAX_ENTRIES) {
            if (strcmp(jaction->valuestring, "add") == 0) {
                int uni = cJSON_IsNumber(juni) ? juni->valueint : 1;
                ok = patch_add_range((uni == 2) ? 2 : 1, start, count);
            } else if (strcmp(jaction->valuestring, "del") == 0) {
                ok = patch_del_range(start, count);
            }
        }
    }

    if (ok) {
        patch_save();
        dmx_recompute_lookups();
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

/** POST /api/led_test — тест LED-ленты (точка/заполнение/радуга/очистка) */
static esp_err_t api_led_test_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *jpx = cJSON_GetObjectItem(root, "pixel");
    cJSON *jr = cJSON_GetObjectItem(root, "r");
    cJSON *jg = cJSON_GetObjectItem(root, "g");
    cJSON *jb = cJSON_GetObjectItem(root, "b");
    cJSON *jmode = cJSON_GetObjectItem(root, "mode");
    cJSON *jcount = cJSON_GetObjectItem(root, "count");

    int px = cJSON_IsNumber(jpx) ? jpx->valueint : 0;
    int r = cJSON_IsNumber(jr) ? jr->valueint : 0;
    int g = cJSON_IsNumber(jg) ? jg->valueint : 0;
    int b = cJSON_IsNumber(jb) ? jb->valueint : 0;
    int count = cJSON_IsNumber(jcount) ? jcount->valueint : 1;
    const char *mode = cJSON_IsString(jmode) ? jmode->valuestring : "point";

    if (r < 0) { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0) { b = 0; }
    if (b > 255) { b = 255; }
    if (count < 1) { count = 1; }
    if (count > LED_STRIP_MAX_LEDS) { count = LED_STRIP_MAX_LEDS; }

    if (strcmp(mode, "clear") == 0) {
        /* Очистка: выключаем тест и возвращаем fallback-цвет */
        g_led_test_mode = 0;
        dmx_apply_fallback();
    } else if (strcmp(mode, "rainbow") == 0) {
        /* Радуга: запускаем фоновую задачу генерации эффекта */
        g_led_test_mode = 2;
        g_led_test_px = 0;
        g_led_test_count = count;
    } else {
        /* Статический цвет (point или fill) */
        g_led_test_mode = 1;
        /* Применяем глобальный порядок каналов к тестовому цвету */
        dmx_lock();
        uint8_t co = g_dmx.channel_order;
        dmx_unlock();
        uint8_t cr = r, cg = g, cb = b;
        dmx_apply_color_order(co, &cr, &cg, &cb);

        led_strip_lock();
        memset(g_led_strip.colors, 0, g_led_strip.count * sizeof(led_color_t));
        led_strip_unlock();

        if (strcmp(mode, "fill") == 0) {
            /* Заполнение: count пикселей начиная с px */
            for (int i = 0; i < count && px + i < g_led_strip.count; i++)
                led_strip_set_pixel(px + i, cr, cg, cb);
        } else {
            /* Точка: один пиксель */
            if (px >= 0 && px < g_led_strip.count)
                led_strip_set_pixel(px, cr, cg, cb);
        }
        led_strip_refresh();
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/led_reverse — включить/выключить реверс порядка LED */
static esp_err_t api_led_reverse_handler(httpd_req_t *req) {
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *jrev = cJSON_GetObjectItem(root, "reverse");
    if (cJSON_IsBool(jrev)) {
        dmx_lock();
        g_dmx.led_reverse = cJSON_IsTrue(jrev);
        dmx_unlock();
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/led_mode — переключить режим LED-лент (parallel/sequential)
 *  {"mode":"parallel"}    — порт0→strip1, порт1→strip2 (независимо)
 *  {"mode":"sequential"}  — оба порта → strip1 (2000 LED через 2 RMT) */
static esp_err_t api_led_mode_handler(httpd_req_t *req) {
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *jmode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(jmode)) {
        const char *m = jmode->valuestring;
        if (strcmp(m, "parallel") == 0) g_led_mode = LED_MODE_PARALLEL;
        else if (strcmp(m, "sequential") == 0) g_led_mode = LED_MODE_SEQUENTIAL;
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/led_shift — установить сдвиг LED-ленты (кольцевой буфер) */
static esp_err_t api_led_shift_handler(httpd_req_t *req) {
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *jshift = cJSON_GetObjectItem(root, "shift");
    if (cJSON_IsNumber(jshift)) {
        int v = jshift->valueint;
        if (v < 0) v = 0;
        if (v >= LED_STRIP_MAX_LEDS) v = LED_STRIP_MAX_LEDS - 1;
        dmx_lock();
        g_dmx.led_shift = (uint16_t)v;
        dmx_unlock();
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/dmx_addr_test — быстрый тест DMX-адреса (переключает в режим TESTER) */
static esp_err_t api_dmx_addr_test_handler(httpd_req_t *req) {
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *jline = cJSON_GetObjectItem(root, "line");
    cJSON *jaddr = cJSON_GetObjectItem(root, "addr");
    cJSON *jr    = cJSON_GetObjectItem(root, "r");
    cJSON *jg    = cJSON_GetObjectItem(root, "g");
    cJSON *jb    = cJSON_GetObjectItem(root, "b");

    int line = cJSON_IsNumber(jline) ? jline->valueint : 0;
    int addr = cJSON_IsNumber(jaddr) ? jaddr->valueint : 1;
    int r = cJSON_IsNumber(jr) ? jr->valueint : 255;
    int g = cJSON_IsNumber(jg) ? jg->valueint : 255;
    int b = cJSON_IsNumber(jb) ? jb->valueint : 255;

    if (line < 0 || line > 1) line = 0;
    if (addr < 1 || addr > 512) addr = 1;
    if (r < 0) { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0) { b = 0; }
    if (b > 255) { b = 255; }

    dmx_lock();
    g_dmx.tx_port = line;
    g_dmx.tx_channel = addr;
    g_dmx.tx_r = r;
    g_dmx.tx_g = g;
    g_dmx.tx_b = b;
    g_dmx.tx_mode = TX_MODE_POINT;
    g_dmx.tx_count = 3;
    dmx_unlock();

    dmx_set_mode(DMX_MODE_TESTER);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/fixture_settings — настройки прибора (порядок каналов, fallback, таймаут) */
static esp_err_t api_fixture_settings_handler(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *jco  = cJSON_GetObjectItem(root, "channel_order");
    cJSON *jfr  = cJSON_GetObjectItem(root, "fallback_r");
    cJSON *jfg  = cJSON_GetObjectItem(root, "fallback_g");
    cJSON *jfb  = cJSON_GetObjectItem(root, "fallback_b");
    cJSON *jft  = cJSON_GetObjectItem(root, "fallback_timeout_ms");

    dmx_lock();
    if (cJSON_IsNumber(jco)) {
        int v = jco->valueint;
        if (v >= 0 && v < CH_ORDER_COUNT) g_dmx.channel_order = (channel_order_t)v;
    }
    if (cJSON_IsNumber(jfr)) { int v = jfr->valueint; if (v<0)v=0; if(v>255)v=255; g_dmx.fallback_r=v; }
    if (cJSON_IsNumber(jfg)) { int v = jfg->valueint; if (v<0)v=0; if(v>255)v=255; g_dmx.fallback_g=v; }
    if (cJSON_IsNumber(jfb)) { int v = jfb->valueint; if (v<0)v=0; if(v>255)v=255; g_dmx.fallback_b=v; }
    if (cJSON_IsNumber(jft)) { int v = jft->valueint; if (v>=10 && v<=5000) g_dmx.fallback_timeout_ms=v; }
    bool fb_changed = cJSON_IsNumber(jfr) || cJSON_IsNumber(jfg) || cJSON_IsNumber(jfb);
    dmx_unlock();

    if (fb_changed) dmx_apply_fallback();

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/speed — заглушка (совместимость со старым UI) */
static esp_err_t api_speed_handler(httpd_req_t *req) {
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) buf[len] = '\0';
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** GET /api/channels — сырые DMX-данные обоих портов (1024 байта: 512 на порт) */
static esp_err_t api_channels_handler(httpd_req_t *req) {
    uint8_t buf[DMX_CHANNELS * 2];
    for (int p = 0; p < 2; p++)
        dmx_get_channel_data(p, buf + p * DMX_CHANNELS, DMX_CHANNELS);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)buf, sizeof(buf));
}

/** GET /api/dmx_channels — alias для /api/channels */
static esp_err_t api_dmx_channels_handler(httpd_req_t *req) {
    return api_channels_handler(req);
}

/** GET /api/led_preview — текущие цвета LED-ленты (N×3 байта: R,G,B на пиксель) */
static esp_err_t api_led_preview_handler(httpd_req_t *req) {
    static uint8_t preview_buf[LED_STRIP_MAX_LEDS * 3];

    led_strip_lock();
    uint16_t n = g_led_strip.count;
    if (n > LED_STRIP_MAX_LEDS) n = LED_STRIP_MAX_LEDS;
    for (uint16_t i = 0; i < n; i++) {
        preview_buf[i * 3 + 0] = g_led_strip.colors[i].r;
        preview_buf[i * 3 + 1] = g_led_strip.colors[i].g;
        preview_buf[i * 3 + 2] = g_led_strip.colors[i].b;
    }
    led_strip_unlock();

    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)preview_buf, n * 3);
}

/**
 * GET /api/blob — все данные одним блобом
 * Формат: [DMX port0: 512B][DMX port1: 512B][fixture_colors: count×3B]
 * Всего: 1024 + count*3 байт (макс 2044)
 */
static esp_err_t api_blob_handler(httpd_req_t *req) {
    static uint8_t blob[2048];

    uint8_t dmx[DMX_CHANNELS * 2];
    for (int p = 0; p < 2; p++)
        dmx_get_channel_data(p, dmx + p * DMX_CHANNELS, DMX_CHANNELS);

    /* Цвета приборов, вычисленные C-кодом (stream_fixture_colors) */
    const uint8_t (*fc)[3] = dmx_get_fixture_colors();
    uint16_t n_fix = 0;
    dmx_lock();
    n_fix = g_patch.count;
    if (n_fix > PATCH_MAX_ENTRIES) n_fix = PATCH_MAX_ENTRIES;
    dmx_unlock();

    size_t total = sizeof(dmx) + n_fix * 3;
    memcpy(blob, dmx, sizeof(dmx));
    memcpy(blob + sizeof(dmx), fc, n_fix * 3);

    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)blob, total);
}

/** POST /api/interpolate — включить/выключить интерполяцию цветов */
static esp_err_t api_interpolate_handler(httpd_req_t *req) {
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *jval = cJSON_GetObjectItem(root, "interpolate");
    if (cJSON_IsBool(jval)) {
        dmx_lock();
        g_dmx.interpolate = cJSON_IsTrue(jval);
        dmx_unlock();
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/save — ручное сохранение настроек в NVS */
static esp_err_t api_save_handler(httpd_req_t *req) {
    settings_save();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** POST /api/ota — обновление прошивки по воздуху */
static esp_err_t api_ota_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    esp_ota_handle_t ota_handle;
    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    if (!ota_part) {
        ESP_LOGE(TAG, "OTA: no partition");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no ota partition\"}");
    }

    int ota_size = (req->content_len > 0) ? req->content_len : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(ota_part, ota_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: begin failed size=%d %s", ota_size, esp_err_to_name(err));
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_begin failed\"}");
    }

    char buf[2048];
    int remaining = req->content_len;
    int written = 0;
    int retries = 0;

    if (remaining > 0) {
        while (remaining > 0) {
            int to_read = (remaining > (int)sizeof(buf)) ? (int)sizeof(buf) : remaining;
            int read_len = httpd_req_recv(req, buf, to_read);
            if (read_len == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > 500) {
                    ESP_LOGE(TAG, "OTA: timeout waiting for data");
                    esp_ota_abort(ota_handle);
                    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"timeout\"}");
                }
                continue;
            }
            retries = 0;
            if (read_len < 0) {
                esp_ota_abort(ota_handle);
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read error\"}");
            }
            err = esp_ota_write(ota_handle, (const void *)buf, read_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA: write failed at offset %d: %s", written, esp_err_to_name(err));
                esp_ota_abort(ota_handle);
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write error\"}");
            }
            remaining -= read_len;
            written += read_len;
        }
    } else {
        while (1) {
            int read_len = httpd_req_recv(req, buf, sizeof(buf));
            if (read_len == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > 500) {
                    ESP_LOGE(TAG, "OTA: timeout waiting for data");
                    esp_ota_abort(ota_handle);
                    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"timeout\"}");
                }
                continue;
            }
            retries = 0;
            if (read_len < 0) {
                if (read_len == -1 && written > 0) break;
                esp_ota_abort(ota_handle);
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read error\"}");
            }
            err = esp_ota_write(ota_handle, (const void *)buf, read_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA: write failed at offset %d: %s", written, esp_err_to_name(err));
                esp_ota_abort(ota_handle);
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write error\"}");
            }
            written += read_len;
            if (read_len < (int)sizeof(buf)) break;
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: end failed %s", esp_err_to_name(err));
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_end failed\"}");
    }

    err = esp_ota_set_boot_partition(ota_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set_boot failed %s", esp_err_to_name(err));
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"set_boot failed\"}");
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/** POST /api/reboot — перезагрузка ESP32 */
static esp_err_t api_reboot_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true}");
    esp_restart();
    return err;
}

/* ======================================================================
 * ИНИЦИАЛИЗАЦИЯ HTTP-СЕРВЕРА
 * ====================================================================== */

/**
 * @brief Запустить HTTP-сервер и зарегистрировать все эндпоинты
 *
 * Конфигурация:
 *   - max_uri_handlers = 24 (максимум зарегистрированных URI)
 *   - stack_size = 32768 (стек для HTTP-задачи)
 *   - lru_purge_enable = true (автоочистка неактивных соединений)
 *   - send_wait_timeout = 30 (таймаут отправки ответа)
 *
 * @return ESP_OK при успехе
 */
esp_err_t web_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 30;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 30;

    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t err = httpd_start(&s_server, &config);
        if (err == ESP_OK) goto registered;
        ESP_LOGE(TAG, "httpd_start attempt %d failed: %s (errno may indicate port busy)", attempt + 1, esp_err_to_name(err));
        if (s_server) { httpd_stop(s_server); s_server = NULL; }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGE(TAG, "Failed to start HTTP server after 3 attempts");
    return ESP_FAIL;
registered:

    /* Регистрация всех URI-обработчиков */
    const httpd_uri_t uris[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = api_status_handler },
        { .uri = "/api/settings",        .method = HTTP_GET,  .handler = api_settings_handler },
        { .uri = "/api/speed",           .method = HTTP_POST, .handler = api_speed_handler },
        { .uri = "/api/channels",        .method = HTTP_GET,  .handler = api_channels_handler },
        { .uri = "/api/dmx_channels",    .method = HTTP_GET,  .handler = api_dmx_channels_handler },
        { .uri = "/api/mode",            .method = HTTP_POST, .handler = api_mode_handler },
        { .uri = "/api/leds",            .method = HTTP_POST, .handler = api_leds_handler },
        { .uri = "/api/leds2",           .method = HTTP_POST, .handler = api_leds2_handler },
        { .uri = "/api/test",            .method = HTTP_POST, .handler = api_test_handler },
        { .uri = "/api/led_test",        .method = HTTP_POST, .handler = api_led_test_handler },
        { .uri = "/api/led_reverse",     .method = HTTP_POST, .handler = api_led_reverse_handler },
        { .uri = "/api/led_mode",        .method = HTTP_POST, .handler = api_led_mode_handler },
        { .uri = "/api/led_shift",       .method = HTTP_POST, .handler = api_led_shift_handler },
        { .uri = "/api/dmx_addr_test",   .method = HTTP_POST, .handler = api_dmx_addr_test_handler },
        { .uri = "/api/fixture_settings",.method = HTTP_POST, .handler = api_fixture_settings_handler },
        { .uri = "/api/led_preview",     .method = HTTP_GET,  .handler = api_led_preview_handler },
        { .uri = "/api/blob",            .method = HTTP_GET,  .handler = api_blob_handler },
        { .uri = "/api/interpolate",     .method = HTTP_POST, .handler = api_interpolate_handler },
        { .uri = "/api/patch",           .method = HTTP_GET,  .handler = api_patch_get_handler },
        { .uri = "/api/patch",           .method = HTTP_POST, .handler = api_patch_post_handler },
        { .uri = "/api/patch.csv",       .method = HTTP_GET,  .handler = api_patch_csv_handler },
        { .uri = "/api/patch/nav",       .method = HTTP_POST, .handler = api_patch_nav_handler },
        { .uri = "/api/patch/range",     .method = HTTP_POST, .handler = api_patch_range_handler },
        { .uri = "/api/ota",             .method = HTTP_POST, .handler = api_ota_handler },
        { .uri = "/api/reboot",          .method = HTTP_POST, .handler = api_reboot_handler },
        { .uri = "/api/save",            .method = HTTP_POST, .handler = api_save_handler },
    };

    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t e = httpd_register_uri_handler(s_server, &uris[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "Failed to register URI %s", uris[i].uri);
    }

    return ESP_OK;
}

/**
 * @brief Остановить HTTP-сервер (освободить ресурсы)
 */
void web_server_stop(void) {
    if (s_server) {
        httpd_handle_t srv = s_server;
        s_server = NULL;
        httpd_stop(srv);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
