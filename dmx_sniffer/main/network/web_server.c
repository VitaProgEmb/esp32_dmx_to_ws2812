/**
 * @file web_server.c
 * @brief HTTP-сервер — прямые вызовы handler'ов
 */

#include "web_server.h"
#include "web_page.h"
#include "utilite/handlers.h"
#include "dmx/dmx.h"
#include "dmx/dmx_led.h"
#include "led_strip.h"
#include "utilite/patch_manager.h"
#include "utilite/settings_manager.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WEB";
static httpd_handle_t s_server = NULL;

static esp_err_t init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs", .partition_label = "storage",
        .max_files = 5, .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) ESP_LOGW(TAG, "SPIFFS init failed: %s", esp_err_to_name(ret));
    return ret;
}

static int read_body(httpd_req_t *req, char *buf, int max) {
    int len = httpd_req_recv(req, buf, max - 1);
    if (len > 0) buf[len] = '\0';
    return len;
}

static esp_err_t resp_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo; if (v > hi) return hi; return v;
}

/* ===== GET ===== */

static esp_err_t handle_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    const char *ptr = (const char *)index_html_gz;
    size_t left = index_html_gz_len;
    while (left > 0) {
        size_t n = (left > 4096) ? 4096 : left;
        esp_err_t r = httpd_resp_send_chunk(req, ptr, n);
        if (r != ESP_OK) return r;
        ptr += n; left -= n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t handle_state(httpd_req_t *req) {
    char json[1024];
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    dmx_lock();
    int dmx0 = (g_dmx.last_rx_ms[0] != 0 && (now_ms - g_dmx.last_rx_ms[0]) < 2000) ? 1 : 0;
    int dmx1 = (g_dmx.last_rx_ms[1] != 0 && (now_ms - g_dmx.last_rx_ms[1]) < 2000) ? 1 : 0;
    int mode_int = g_dmx.mode;
    snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"dmx0\":%d,\"dmx1\":%d,"
        "\"leds\":%d,\"leds2\":%d,"
        "\"reverse\":%s,\"shift\":%d,\"interpolate\":%s,"
        "\"fb_r\":%d,\"fb_g\":%d,\"fb_b\":%d,\"fb_timeout\":%d,"
        "\"lr0\":%lu,\"lr1\":%lu}",
        mode_int == 1 ? "tester" : mode_int == 2 ? "patch" : "sniffer",
        dmx0, dmx1, (int)led_get_count(0), (int)led_get_count(1),
        g_led_settings.reverse ? "true" : "false", (int)g_led_settings.shift,
        g_led_settings.interpolate ? "true" : "false",
        g_led_settings.fallback_r, g_led_settings.fallback_g, g_led_settings.fallback_b,
        g_led_settings.fallback_timeout_ms,
        (unsigned long)g_dmx.last_rx_ms[0], (unsigned long)g_dmx.last_rx_ms[1]);
    dmx_unlock();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_channels(httpd_req_t *req) {
    static uint8_t buf[1024];
    for (int p = 0; p < 2; p++) dmx_read(p, buf + p * DMX_CHANNELS, DMX_CHANNELS);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)buf, sizeof(buf));
}

static esp_err_t handle_led_preview(httpd_req_t *req) {
    static uint8_t buf[2048];
    const uint8_t (*fc)[3] = dmx_led_get_fixture_colors();
    uint16_t n_fix = 0;
    dmx_lock(); n_fix = g_patch.count; if (n_fix > PATCH_MAX_ENTRIES) n_fix = PATCH_MAX_ENTRIES; dmx_unlock();
    int pos = 0;
    for (int i = 0; i < n_fix && pos + 3 < (int)sizeof(buf); i++) {
        buf[pos++] = fc[i][0]; buf[pos++] = fc[i][1]; buf[pos++] = fc[i][2];
    }
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)buf, pos);
}

static esp_err_t handle_blob(httpd_req_t *req) {
    uint8_t blob[1024];
    for (int p = 0; p < 2; p++) dmx_read(p, blob + p * DMX_CHANNELS, DMX_CHANNELS);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)blob, sizeof(blob));
}

static esp_err_t handle_patch_get(httpd_req_t *req) {
    char *csv = patch_to_csv_string();
    if (csv) { httpd_resp_set_type(req, "text/csv"); esp_err_t r = httpd_resp_sendstr(req, csv); free(csv); return r; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "[]");
}

/* ===== POST → handler_() ===== */

static esp_err_t handle_mode(httpd_req_t *req) {
    char buf[128]; if (read_body(req, buf, sizeof(buf)) <= 0) return resp_ok(req);
    cJSON *root = cJSON_Parse(buf); if (!root) return resp_ok(req);
    cJSON *j = cJSON_GetObjectItem(root, "mode");
    uint8_t mode = 0;
    if (cJSON_IsString(j)) {
        if (strcmp(j->valuestring, "tester") == 0) mode = 1;
        else if (strcmp(j->valuestring, "patch") == 0) mode = 2;
    }
    cJSON_Delete(root); handler_mode(mode); return resp_ok(req);
}

static esp_err_t handle_leds(httpd_req_t *req) {
    char buf[128]; if (read_body(req, buf, sizeof(buf)) <= 0) return resp_ok(req);
    cJSON *root = cJSON_Parse(buf); if (!root) return resp_ok(req);
    cJSON *jc = cJSON_GetObjectItem(root, "count");
    cJSON *js = cJSON_GetObjectItem(root, "strip");
    uint16_t count = cJSON_IsNumber(jc) ? clamp(jc->valueint, 1, LED_STRIP_MAX_LEDS) : 100;
    uint8_t strip = cJSON_IsNumber(js) ? js->valueint : 0;
    cJSON_Delete(root); handler_led_count(strip, count); return resp_ok(req);
}

static esp_err_t handle_led_config(httpd_req_t *req) {
    char buf[256]; if (read_body(req, buf, sizeof(buf)) <= 0) return resp_ok(req);
    cJSON *root = cJSON_Parse(buf); if (!root) return resp_ok(req);
    cJSON *j;
    j = cJSON_GetObjectItem(root, "reverse");
    if (cJSON_IsBool(j)) handler_led_reverse(cJSON_IsTrue(j));
    j = cJSON_GetObjectItem(root, "interpolate");
    if (cJSON_IsBool(j)) handler_led_interpolate(cJSON_IsTrue(j));
    j = cJSON_GetObjectItem(root, "shift");
    if (cJSON_IsNumber(j)) handler_led_shift(clamp(j->valueint, 0, 255));
    j = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(j)) handler_led_mode(strcmp(j->valuestring, "sequential") == 0);
    cJSON_Delete(root); return resp_ok(req);
}

static esp_err_t handle_led_test(httpd_req_t *req) {
    char buf[256]; if (read_body(req, buf, sizeof(buf)) <= 0) return resp_ok(req);
    cJSON *root = cJSON_Parse(buf); if (!root) return resp_ok(req);
    cJSON *j;
    uint16_t pixel = 0, count = 1;
    uint8_t r = 0, g = 0, b = 0, speed = 30, mode = 1;
    j = cJSON_GetObjectItem(root, "pixel"); pixel = cJSON_IsNumber(j) ? clamp(j->valueint, 0, LED_STRIP_MAX_LEDS) : 0;
    j = cJSON_GetObjectItem(root, "r"); r = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "g"); g = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "b"); b = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "speed"); speed = cJSON_IsNumber(j) ? clamp(j->valueint, 5, 200) : 30;
    j = cJSON_GetObjectItem(root, "count"); count = cJSON_IsNumber(j) ? clamp(j->valueint, 1, LED_STRIP_MAX_LEDS) : 1;
    j = cJSON_GetObjectItem(root, "mode");
    const char *m = cJSON_IsString(j) ? j->valuestring : "point";
    if      (strcmp(m, "rainbow") == 0) mode = 2;
    else if (strcmp(m, "running") == 0) mode = 3;
    else if (strcmp(m, "breathe") == 0) mode = 4;
    else if (strcmp(m, "wave")    == 0) mode = 5;
    else if (strcmp(m, "fill")    == 0) mode = 1;
    else if (strcmp(m, "clear")   == 0) mode = 0;
    else                                mode = 1;
    cJSON_Delete(root);
    if (mode == 0) handler_led_clear();
    else handler_led_test(mode, r, g, b, speed, pixel, count);
    return resp_ok(req);
}

static esp_err_t handle_dmx_test(httpd_req_t *req) {
    char buf[256]; if (read_body(req, buf, sizeof(buf)) <= 0) return resp_ok(req);
    cJSON *root = cJSON_Parse(buf); if (!root) return resp_ok(req);
    cJSON *j;
    uint8_t port = 0, channel = 1, r = 0, g = 0, b = 0, mode = 0;
    uint16_t count = 1;
    j = cJSON_GetObjectItem(root, "port"); port = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 1) : 0;
    j = cJSON_GetObjectItem(root, "channel"); channel = cJSON_IsNumber(j) ? clamp(j->valueint, 1, 512) : 1;
    j = cJSON_GetObjectItem(root, "r"); r = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "g"); g = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "b"); b = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "mode"); mode = (cJSON_IsString(j) && strcmp(j->valuestring, "fill") == 0) ? 1 : 0;
    j = cJSON_GetObjectItem(root, "count"); count = cJSON_IsNumber(j) ? clamp(j->valueint, 1, 512) : 1;
    cJSON_Delete(root); handler_dmx_test(port, channel, r, g, b, mode, count); return resp_ok(req);
}

static esp_err_t handle_fixture(httpd_req_t *req) {
    char buf[256]; if (read_body(req, buf, sizeof(buf)) <= 0) return resp_ok(req);
    cJSON *root = cJSON_Parse(buf); if (!root) return resp_ok(req);
    cJSON *j;
    uint8_t co = 0, fr = 0, fg = 0, fb = 255;
    uint16_t ft = 100;
    j = cJSON_GetObjectItem(root, "channel_order"); co = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 5) : 0;
    j = cJSON_GetObjectItem(root, "fallback_r"); fr = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "fallback_g"); fg = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 0;
    j = cJSON_GetObjectItem(root, "fallback_b"); fb = cJSON_IsNumber(j) ? clamp(j->valueint, 0, 255) : 255;
    j = cJSON_GetObjectItem(root, "timeout"); ft = cJSON_IsNumber(j) ? clamp(j->valueint, 10, 5000) : 100;
    cJSON_Delete(root); handler_fixture_settings(co, fr, fg, fb, ft); return resp_ok(req);
}

static esp_err_t handle_patch_post(httpd_req_t *req) {
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 32768) return resp_ok(req);
    char *buf = malloc(content_len + 1); if (!buf) return resp_ok(req);
    int total = 0;
    while (total < content_len) { int r = httpd_req_recv(req, buf + total, content_len - total); if (r <= 0) break; total += r; }
    buf[total] = '\0';
    if (buf[0] == '[') {
        cJSON *root = cJSON_Parse(buf);
        if (root && cJSON_IsArray(root)) {
            dmx_lock(); g_patch.count = 0;
            int n = cJSON_GetArraySize(root); if (n > PATCH_MAX_ENTRIES) n = PATCH_MAX_ENTRIES;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(root, i);
                cJSON *jport = cJSON_GetObjectItem(item, "port");
                cJSON *jaddr = cJSON_GetObjectItem(item, "addr");
                if (cJSON_IsNumber(jaddr)) {
                    g_patch.entries[i].universe = cJSON_IsNumber(jport) ? jport->valueint : 0;
                    g_patch.entries[i].dmx_addr = clamp(jaddr->valueint, 1, 512);
                    g_patch.entries[i].skip = false; g_patch.count++;
                }
            }
            dmx_unlock();
        }
        if (root) cJSON_Delete(root);
    } else {
        patch_apply_from_csv(buf);
    }
    free(buf); patch_save(); dmx_led_recompute(); return resp_ok(req);
}

static esp_err_t handle_save(httpd_req_t *req) { settings_save(); return resp_ok(req); }

static esp_err_t handle_reboot(httpd_req_t *req) {
    esp_err_t r = resp_ok(req); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); return r;
}

static esp_err_t handle_ota(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    esp_ota_handle_t ota;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no partition\"}");
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    if (err != ESP_OK) return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_begin\"}");
    char buf[2048]; int remaining = req->content_len; int retries = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, sizeof(buf));
        if (n == 0) { vTaskDelay(pdMS_TO_TICKS(10)); if (++retries > 500) { esp_ota_abort(ota); return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"timeout\"}"); } continue; }
        retries = 0;
        if (n < 0) { esp_ota_abort(ota); return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read\"}"); }
        err = esp_ota_write(ota, buf, n); if (err != ESP_OK) { esp_ota_abort(ota); return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write\"}"); }
        remaining -= n;
    }
    err = esp_ota_end(ota); if (err != ESP_OK) return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_end\"}");
    err = esp_ota_set_boot_partition(part); if (err != ESP_OK) return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"boot_part\"}");
    httpd_resp_sendstr(req, "{\"ok\":true}"); vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); return ESP_OK;
}

/* ===== INIT ===== */

esp_err_t web_server_init(void) {
    init_spiffs();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24; config.stack_size = 8192;
    config.lru_purge_enable = true; config.send_wait_timeout = 30;
    for (int i = 0; i < 3; i++) {
        if (httpd_start(&s_server, &config) == ESP_OK) goto reg;
        ESP_LOGE(TAG, "httpd_start attempt %d failed", i + 1);
        if (s_server) { httpd_stop(s_server); s_server = NULL; }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return ESP_FAIL;
reg:;
    const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = handle_index },
        { .uri = "/api/state",     .method = HTTP_GET,  .handler = handle_state },
        { .uri = "/api/channels",  .method = HTTP_GET,  .handler = handle_channels },
        { .uri = "/api/led_preview",.method = HTTP_GET, .handler = handle_led_preview },
        { .uri = "/api/blob",      .method = HTTP_GET,  .handler = handle_blob },
        { .uri = "/api/patch",     .method = HTTP_GET,  .handler = handle_patch_get },
        { .uri = "/api/mode",       .method = HTTP_POST, .handler = handle_mode },
        { .uri = "/api/leds",       .method = HTTP_POST, .handler = handle_leds },
        { .uri = "/api/led_config", .method = HTTP_POST, .handler = handle_led_config },
        { .uri = "/api/led_test",   .method = HTTP_POST, .handler = handle_led_test },
        { .uri = "/api/dmx_test",   .method = HTTP_POST, .handler = handle_dmx_test },
        { .uri = "/api/fixture",    .method = HTTP_POST, .handler = handle_fixture },
        { .uri = "/api/patch",      .method = HTTP_POST, .handler = handle_patch_post },
        { .uri = "/api/save",       .method = HTTP_POST, .handler = handle_save },
        { .uri = "/api/reboot",     .method = HTTP_POST, .handler = handle_reboot },
        { .uri = "/api/ota",        .method = HTTP_POST, .handler = handle_ota },
    };
    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++)
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK)
            ESP_LOGE(TAG, "Failed to register %s", uris[i].uri);
    ESP_LOGI(TAG, "Server started, %d endpoints", (int)(sizeof(uris)/sizeof(uris[0])));
    return ESP_OK;
}

void web_server_stop(void) {
    if (s_server) { httpd_handle_t srv = s_server; s_server = NULL; httpd_stop(srv); vTaskDelay(pdMS_TO_TICKS(500)); }
}
