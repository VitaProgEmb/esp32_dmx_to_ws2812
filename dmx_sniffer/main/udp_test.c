/**
 * @file udp_test.c
 * @brief UDP-сервер для стресс-тестирования DMX-приёмников
 *
 * ============================================================================
 * НАЗНАЧЕНИЕ
 * ============================================================================
 *
 * Этот модуль предоставляет высокоскоростной UDP-интерфейс для получения
 * данных обоих DMX-портов. Используется совместно с C++ тестовой программой
 * (stress_test.cpp) для длительного стресс-тестирования (15 минут, 30fps).
 *
 * ============================================================================
 * ЗАЧЕМ UDP, А НЕ HTTP
 * ============================================================================
 *
 * HTTP-запрос к /api/blob занимает ~60мс (TCP handshake + HTTP headers + send).
 * При 30fps каждый кадр = 33мс. HTTP не успевает — реальный FPS падает до 15.
 *
 * UDP-запрос: 1 байт отправка + 1024 байта ответ = ~0.1мс.
 * Это позволяет достичь реальных 30fps в стресс-тесте.
 *
 * ============================================================================
 * ПРОТОКОЛ
 * ============================================================================
 *
 * Формат запроса (PC -> ESP):
 *   [0x01]                    — команда "get blob" (1 байт)
 *
 * Формат ответа (ESP -> PC):
 *   [P0: 512 байт]           — данные порта 0 (каналы 1-512)
 *   [P1: 512 байт]           — данные порта 1 (каналы 1-512)
 *   Итого: 1024 байта
 *
 * PC-программа отправляет 0x01 и получает 1024 байта.
 * Сравнивает P0[i] с P1[i] для каждого канала.
 *
 * ============================================================================
 * АРХИТЕКТУРА
 * ============================================================================
 *
 * Задача FreeRTOS (udp_test_task):
 *   - Ядро 1 (приоритет 5 — выше HTTP, но ниже ISR)
 *   - Блокируется на recvfrom() — ждёт UDP-пакет
 *   - При получении 0x01: копирует данные обоих портов и отправляет ответ
 *   - Использует dmx_get_channel_data() — потокобезопасное чтение из s_rx_buf[]
 *
 * Инициализация:
 *   udp_test_init() вызывается из app_main() после dmx_start_rx_task().
 *   Создаёт задачу и начинает прослушивание UDP-порта 5124.
 *
 * ============================================================================
 * ПРОВОДНАЯ СХЕМА ДЛЯ ТЕСТА
 * ============================================================================
 *
 * COM9 TX (FT2232) ──┬── GPIO15 (RX1, порт 0)
 *                    └── GPIO16 (RX2, порт 1)
 *
 * Оба UART-приёмника слушают один и тот же источник данных.
 * Routing matrix = 0 (нормальный режим): порт0→GPIO15, порт1→GPIO16.
 * Сравнение P0 vs P1 проверяет корректность обоих приёмников.
 */

#include "udp_test.h"
#include "dmx.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include <string.h>

/** UDP-порт сервера (5124 — не пересекается с HTTP=80, mDNS=5353) */
#define UDP_PORT 5124

/** Тег для логов ESP-IDF */
#define TAG "UDP_TEST"

/**
 * @brief Задача UDP-сервера для стресс-теста
 *
 * Цикл:
 *   1. recvfrom() — блокирующее ожидание UDP-пакета от PC
 *   2. Если команда = 0x01 → копировать P0+P1 в ответный буфер
 *   3. sendto() — отправить 1024 байта обратно на PC
 *
 * @param arg Не используется (NULL)
 */
static void udp_test_task(void *arg) {
    /* Создание UDP-сокета */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Привязка к порту 5124 на всех интерфейсах (INADDR_ANY) */
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

    ESP_LOGI(TAG, "UDP listening on port %d", UDP_PORT);

    /**
     * Ответный буфер: P0[512] + P1[512] = 1024 байта.
     * Статический (не в стеке) чтобы не нагружать FreeRTOS heap.
     */
    uint8_t resp[1024];

    /* Основной цикл: обработка запросов */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        uint8_t cmd;

        /* Блокирующее ожидание UDP-пакета (бесконечный таймаут) */
        int len = recvfrom(sock, &cmd, 1, 0,
                          (struct sockaddr *)&client_addr, &addr_len);
        if (len <= 0) continue;

        /* Команда 0x01: "get blob" — вернуть данные обоих DMX-портов */
        if (cmd == 0x01) {
            /* dmx_get_channel_data() — потокобезопасное чтение из s_rx_buf[]
             * через spinlock. Не блокирует ISR, не вызывает дрифта. */
            dmx_get_channel_data(0, resp, DMX_CHANNELS);
            dmx_get_channel_data(1, resp + DMX_CHANNELS, DMX_CHANNELS);

            /* Отправка ответа: 1024 байта (P0 + P1) */
            sendto(sock, resp, sizeof(resp), 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
    }
}

/**
 * @brief Инициализация UDP-тест-сервера
 *
 * Вызывается из app_main() после запуска RX-задач.
 * Создаёт задачу udp_test_task на ядре 1 (приоритет 5).
 *
 * Ядро 1 выбрано потому что:
 *   - Ядро 0: HTTP-сервер, Wi-Fi, LED, boot-button
 *   - Ядро 1: DMX ISR, RX/TX задачи, UDP-тест
 *   - UDP-задача работает на том же ядре что и ISR — минимальная задержка
 *     при чтении s_rx_buf[] (нет миграции между ядрами)
 *
 * Стек 4096 байт достаточен для UDP-операций (lwip не требует много RAM).
 */
void udp_test_init(void) {
    xTaskCreatePinnedToCore(udp_test_task, "udp_test", 4096, NULL, 5, NULL, 1);
}
