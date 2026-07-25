#pragma once

/**
 * @file dmx_rx.h
 * @brief DMX512 приёмник — HW UART ISR + double-buffer + event notification
 */

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Конфигурация одного DMX приёмника
 */
typedef struct {
    int rx_pin;       /**< GPIO приёма данных */
    int tx_pin;       /**< GPIO передачи (направление RS485) */
    int dir_pin;      /**< GPIO DE/RE, -1 = нет управления */
    int uart_num;     /**< UART_NUM_1 или UART_NUM_2 */
} dmx_rx_cfg_t;

/**
 * @brief Инициализация одного DMX порта (без запуска UART)
 * @param port  0 или 1
 * @param cfg   конфигурация пинов
 */
void dmx_rx_init(int port, const dmx_rx_cfg_t *cfg);

/**
 * @brief Запуск UART, ISR и задач приёма обоих портов
 *
 * Вызывать после init всех портов. Создаёт event group,
 * настраивает UART, устанавливает ISR, запускает задачи.
 */
void dmx_rx_start(void);

/**
 * @brief Включение/отключение приёмника
 * @param port    0 или 1
 * @param enable  true=включить, false=отключить
 *
 * При отключении ISR выходит без обработки.
 * Нужно в режиме TX (тестер/патчер) чтобы не ловить шум.
 */
void dmx_rx_enable(int port, bool enable);
