/**
 * @file dmx_hal.c
 * @brief Аппаратный абстрактный слой (HAL) для DMX-портов
 */

#include "dmx_hal.h"
#include "settings.h"
#include "esp_log.h"
#include "uart_bypass.h"
#include "driver/uart.h"
#include "rom/ets_sys.h"

static const char *HAL_TAG = "DMX_HAL";

void dmx_hal_init(void) {
    uart_bypass_init_all();
}

void dmx_hal_start(void) {
    uart_bypass_start_timer();
}

bool dmx_hal_send(int port, const uint8_t *data, int len, int break_len) {
    uart_port_t uart_num = (port == 0) ? UART_NUM_1 : UART_NUM_2;

    uart_set_line_inverse(uart_num, UART_SIGNAL_TXD_INV);
    ets_delay_us(176);

    uart_set_line_inverse(uart_num, UART_SIGNAL_INV_DISABLE);
    ets_delay_us(16);

    uart_write_bytes(uart_num, data, len);
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100));

    return true;
}
