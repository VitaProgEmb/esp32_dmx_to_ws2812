#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BYPASS_DMX_SIZE 513

void uart_bypass_init(int port);
void uart_bypass_init_all(void);
void uart_bypass_start_timer(void);
void uart_bypass_set_notify_task(int port, TaskHandle_t handle);
void uart_bypass_set_dir(int port, int level);
void uart_bypass_set_tx_mode(int port, bool tx_mode);
bool uart_bypass_get_frame(int port, uint8_t *out, int max_len, uint32_t *frame_len);
