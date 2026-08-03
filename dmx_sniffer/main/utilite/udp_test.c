/**
 * @file udp_test.c
 * @brief Реализация UDP-сервера для DMX-сниффера
 *
 * Модуль реализует UDP-сервер на порту 5124, который обрабатывает
 * шесть типов команд от удалённого клиента:
 *
 *   0x01 — get blob:     ESP → P0[512] + P1[512] = 1024 байта (сырые DMX-данные)
 *   0x02 — OTA begin:    [total_size:4] → ACK 0xAA (начало OTA-прошивки)
 *   0x03 — OTA chunk:    [seq:2][data:N] → ACK [seq:2] (чанк прошивки)
 *   0x04 — OTA end:      → перезагрузка (завершение OTA и ребут)
 *   0x05 — get checksum: ESP → [p0_xor:2][p0_sum:2][p1_xor:2][p1_sum:2] = 8 байт
 *   0x06 — ck report:    ESP → [p0_count:4][p1_count:4][p0_ck:N×6][p1_ck:M×6]
 *
 * Протокол использует UDP (без установки соединения), поэтому клиент
 * должен сам отслеживать таймауты и повторять запросы при необходимости.
 *
 * @note Все ответы отправляются через sendto() в адрес отправителя.
 * @note OTA-прошивка выполняется через esp_ota API с проверкой целостности.
 */

#include "udp_test.h"
#include "dmx/dmx.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <string.h>

/** @brief UDP-порт сервера */
#define UDP_PORT    5124

/** @brief Максимальный размер чанка данных (байт) */
#define CHUNK_SIZE  1024

/** @brief Тег для логирования ESP-IDF */
#define TAG         "UDP"

/** @brief Дескриптор текущей OTA-сессии */
static esp_ota_handle_t s_ota_handle = 0;

/** @brief Указатель на целевой раздел для OTA-прошивки */
static const esp_partition_t *s_ota_partition = NULL;

/** @brief Общий размер прошивки в байтах (из команды 0x02) */
static uint32_t s_ota_total = 0;

/** @brief Количество записанных байт на данный момент */
static uint32_t s_ota_written = 0;

/** @brief Номер последнего обработанного чанка (для порядка следования) */
static uint16_t s_ota_last_seq = 0;

/** @brief Флаг активности OTA-прошивки (true = OTA в процессе) */
static bool s_ota_active = false;

/**
 * @brief Основная задача UDP-сервера
 *
 * Создаёт UDP-сокет, привязывает его к порту 5124 и в бесконечном цикле
 * принимает UDP-дейтаграммы. Первый байт дейтаграммы определяет тип команды:
 *   - 0x01 → get blob (сырые DMX-данные)
 *   - 0x02 → OTA begin (начало прошивки)
 *   - 0x03 → OTA chunk (запись чанка)
 *   - 0x04 → OTA end (завершение и ребут)
 *   - 0x05 → get checksum (контрольные суммы)
 *   - 0x06 → ck report (отчёт кольцевого буфера)
 *
 * @param[in] arg Аргумент задачи (не используется)
 */
static void udp_test_task(void *arg) {
    /* Создание UDP-сокета */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Привязка сокета к UDP-порту 5124 */
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP listening on port %d (DMX + OTA)", UDP_PORT);

    /* Буферы для приёма данных и формирования ответа */
    uint8_t buf[CHUNK_SIZE + 16];
    static uint8_t resp[16384];

    /* Основной цикл обработки UDP-дейтаграмм */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        /* Приём UDP-дейтаграммы */
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&client_addr, &addr_len);
        if (len <= 0) continue;

        /* Извлечение кода команды из первого байта */
        uint8_t cmd = buf[0];

        /* --- 0x01: DMX blob readback ---
         * Возвращает сырые DMX-данные обоих портов (P0 и P1).
         * Формат ответа: [P0:512][P1:512] = 1024 байта.
         * Данные копируются из глобального массива g_raw_frames. */
        if (cmd == 0x01) {
            memcpy(resp, g_raw_frames[0].data, DMX_CHANNELS);
            memcpy(resp + DMX_CHANNELS, g_raw_frames[1].data, DMX_CHANNELS);
            sendto(sock, resp, DMX_CHANNELS * 2, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }

        /* --- 0x05: DMX checksum readback ---
         * Вычисляет XOR и сумму для каждого DMX-порта.
         * Формат ответа: [p0_xor:2][p0_sum:2][p1_xor:2][p1_sum:2] = 8 байт.
         * Все значения в big-endian формате.
         * Используется для быстрой проверки целостности данных. */
        else if (cmd == 0x05) {
            uint16_t p0_xor = 0, p0_sum = 0;
            uint16_t p1_xor = 0, p1_sum = 0;
            const uint8_t *d0 = g_raw_frames[0].data;
            const uint8_t *d1 = g_raw_frames[1].data;
            uint16_t len0 = g_raw_frames[0].len;
            uint16_t len1 = g_raw_frames[1].len;
            for (int i = 0; i < len0; i++) { p0_xor ^= d0[i]; p0_sum += d0[i]; }
            for (int i = 0; i < len1; i++) { p1_xor ^= d1[i]; p1_sum += d1[i]; }
            uint8_t ck[8] = {
                p0_xor >> 8, p0_xor & 0xFF,
                p0_sum >> 8, p0_sum & 0xFF,
                p1_xor >> 8, p1_xor & 0xFF,
                p1_sum >> 8, p1_sum & 0xFF,
            };
            sendto(sock, ck, sizeof(ck), 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }

        /* --- 0x06: checksum ring buffer report ---
         * Возвращает содержимое кольцевого буфера контрольных сумм.
         * Формат ответа:
         *   [p0_count:4][p1_count:4] — заголовок (big-endian)
         *   [p0_ck:N×6] — записи для P0: [xor:2][sum:2][frame_len:2]
         *   [p1_ck:M×6] — записи для P1: [xor:2][sum:2][frame_len:2]
         * Количество записей ограничено CK_RING_SIZE. */
        else if (cmd == 0x06) {
            uint32_t n0 = g_ck_rings[0].count;
            uint32_t n1 = g_ck_rings[1].count;
            uint32_t c0 = n0 > CK_RING_SIZE ? CK_RING_SIZE : n0;
            uint32_t c1 = n1 > CK_RING_SIZE ? CK_RING_SIZE : n1;

            /* Заголовок: big-endian [p0_count:4][p1_count:4] */
            resp[0] = c0 >> 24; resp[1] = c0 >> 16; resp[2] = c0 >> 8; resp[3] = c0;
            resp[4] = c1 >> 24; resp[5] = c1 >> 16; resp[6] = c1 >> 8; resp[7] = c1;

            uint8_t *p = resp + 8;

            /* Записи контрольных сумм для порта P0 */
            for (uint32_t i = 0; i < c0; i++) {
                uint32_t idx = (n0 - c0 + i) & (CK_RING_SIZE - 1);
                uint16_t x = g_ck_rings[0].buf[idx].xor_val;
                uint16_t s = g_ck_rings[0].buf[idx].sum_val;
                uint16_t fl = g_ck_rings[0].buf[idx].frame_len;
                *p++ = x >> 8;  *p++ = x & 0xFF;
                *p++ = s >> 8;  *p++ = s & 0xFF;
                *p++ = fl >> 8; *p++ = fl & 0xFF;
            }

            /* Записи контрольных сумм для порта P1 */
            for (uint32_t i = 0; i < c1; i++) {
                uint32_t idx = (n1 - c1 + i) & (CK_RING_SIZE - 1);
                uint16_t x = g_ck_rings[1].buf[idx].xor_val;
                uint16_t s = g_ck_rings[1].buf[idx].sum_val;
                uint16_t fl = g_ck_rings[1].buf[idx].frame_len;
                *p++ = x >> 8;  *p++ = x & 0xFF;
                *p++ = s >> 8;  *p++ = s & 0xFF;
                *p++ = fl >> 8; *p++ = fl & 0xFF;
            }

            int total = 8 + (c0 + c1) * 6;
            sendto(sock, resp, total, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }

        /* --- 0x02: OTA begin ---
         * Инициализирует процесс OTA-обновления прошивки.
         * Принимает общий размер прошивки в buf[1..4] (4 байта, big-endian).
         * Алгоритм:
         *   1. Парсинг размера прошивки
         *   2. Поиск следующего доступного раздела (esp_ota_get_next_update_partition)
         *   3. Вызов esp_ota_begin() для начала записи
         *   4. Отправка ACK (0xAA) клиенту при успехе
         *   5. Отправка NACK (0xFF) при ошибке
         * При ошибке раздел не используется, OTA не активируется. */
        else if (cmd == 0x02 && len >= 5) {
            s_ota_total = (buf[1] << 24) | (buf[2] << 16) | (buf[3] << 8) | buf[4];
            s_ota_written = 0;
            s_ota_last_seq = 0;
            s_ota_active = false;

            s_ota_partition = esp_ota_get_next_update_partition(NULL);
            if (!s_ota_partition) {
                ESP_LOGE(TAG, "OTA: no partition found");
                uint8_t nack = 0xFF;
                sendto(sock, &nack, 1, 0, (struct sockaddr *)&client_addr, addr_len);
                continue;
            }

            esp_err_t err = esp_ota_begin(s_ota_partition, s_ota_total, &s_ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
                uint8_t nack = 0xFF;
                sendto(sock, &nack, 1, 0, (struct sockaddr *)&client_addr, addr_len);
                continue;
            }

            s_ota_active = true;
            ESP_LOGI(TAG, "OTA begin: %lu bytes, partition: %s",
                     (unsigned long)s_ota_total, s_ota_partition->label);

            uint8_t ack = 0xAA;
            sendto(sock, &ack, 1, 0, (struct sockaddr *)&client_addr, addr_len);
        }

        /* --- 0x03: OTA chunk ---
         * Записывает чанк данных прошивки.
         * Формат: [seq:2][data:N], где seq — порядковый номер (big-endian).
         * Проверяет порядок: seq должен быть ровно на 1 больше предыдущего.
         * При совпадении — запись через esp_ota_write().
         * При несовпадении — чанк игнорируется (но ACK всё равно отправляется).
         * ACK формат: [0x06][seq_hi][seq_lo] — возврат номера чанка.
         * Прогресс логируется каждые ~10 КБ. */
        else if (cmd == 0x03 && s_ota_active && len >= 3) {
            uint16_t seq = (buf[1] << 8) | buf[2];
            uint8_t *data = buf + 3;
            int data_len = len - 3;

            if (seq == s_ota_last_seq + 1) {
                esp_err_t err = esp_ota_write(s_ota_handle, data, data_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "OTA write failed at seq %u: %s", seq, esp_err_to_name(err));
                    s_ota_active = false;
                    esp_ota_abort(s_ota_handle);
                    continue;
                }
                s_ota_written += data_len;
                s_ota_last_seq = seq;

                if (s_ota_written % (10 * CHUNK_SIZE) == 0 || s_ota_written >= s_ota_total) {
                    ESP_LOGI(TAG, "OTA progress: %lu / %lu bytes (%d%%)",
                             (unsigned long)s_ota_written, (unsigned long)s_ota_total,
                             (int)(s_ota_written * 100 / s_ota_total));
                }
            }

            /* ACK: возврат номера чанка (всегда, даже при пропуске) */
            uint8_t ack[3] = {0x06, buf[1], buf[2]};
            sendto(sock, ack, 3, 0, (struct sockaddr *)&client_addr, addr_len);
        }

        /* --- 0x04: OTA end ---
         * Завершает процесс OTA-обновления.
         * Алгоритм:
         *   1. esp_ota_end() — проверка целостности прошивки (CRC)
         *   2. esp_ota_set_boot_partition() — установка нового загрузочного раздела
         *   3. Отправка ACK (0xAA) клиенту
         *   4. Задержка 100 мс (для завершения UDP-ответа)
         *   5. esp_restart() — перезагрузка устройства
         * При ошибке любого этапа отправляется NACK (0xFF), OTA деактивируется. */
        else if (cmd == 0x04 && s_ota_active) {
            esp_err_t err = esp_ota_end(s_ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
                uint8_t nack = 0xFF;
                sendto(sock, &nack, 1, 0, (struct sockaddr *)&client_addr, addr_len);
                s_ota_active = false;
                continue;
            }

            err = esp_ota_set_boot_partition(s_ota_partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
                uint8_t nack = 0xFF;
                sendto(sock, &nack, 1, 0, (struct sockaddr *)&client_addr, addr_len);
                s_ota_active = false;
                continue;
            }

            ESP_LOGI(TAG, "OTA complete! %lu bytes written. Rebooting...",
                     (unsigned long)s_ota_written);

            uint8_t ack = 0xAA;
            sendto(sock, &ack, 1, 0, (struct sockaddr *)&client_addr, addr_len);

            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

/**
 * @brief Инициализация UDP-сервера
 *
 * Создаёт задачу FreeRTOS на ядре 1 (CPU1) с приоритетом 5
 * и стеком 8192 байт. Задача слушает UDP-порт 5124 и обрабатывает
 * команды DMX readback, checksum и OTA-прошивки.
 *
 * @note Функция неблокирующая — задача создаётся асинхронно.
 * @note Сервер запускается на ядре 1 для снижения нагрузки на основное ядро.
 */
void udp_test_init(void) {
    xTaskCreatePinnedToCore(udp_test_task, "udp_test", 8192, NULL, 5, NULL, 1);
}
