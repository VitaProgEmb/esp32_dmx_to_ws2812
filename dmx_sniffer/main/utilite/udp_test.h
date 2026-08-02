/**
 * @file udp_test.h
 * @brief UDP-сервер: DMX readback + checksum + OTA
 *
 * Протокол:
 *   0x01 — get blob:     ESP → P0[512] + P1[512] = 1024 байта
 *   0x02 — OTA begin:    [total_size:4] → ACK 0xAA
 *   0x03 — OTA chunk:    [seq:2][data:N] → ACK [seq:2]
 *   0x04 — OTA end:      → перезагрузка
 *   0x05 — get checksum: ESP → [p0_xor:2][p0_sum:2][p1_xor:2][p1_sum:2] = 8 байт
 *   0x06 — ck report:    ESP → [p0_count:4][p1_count:4][p0_ck:N×6][p1_ck:M×6]
 */

#pragma once

/**
 * @brief Инициализировать UDP-сервер
 *
 * Создаёт задачу FreeRTOS на ядре 1 (приоритет 5).
 * Задача слушает UDP-порт 5124 и обрабатывает команды 0x01..0x06.
 */
void udp_test_init(void);
