/**
 * @file dmx_hal.c
 * @brief Аппаратный абстрактный слой (HAL) для DMX-портов
 */

#include "dmx_hal.h"
#include "settings.h"
#include "esp_log.h"
#include "uart_bypass.h"
#include "driver/uart.h"
#include "soc/uart_struct.h"
#include "soc/uart_reg.h"
#include "hal/uart_ll.h"
#include "rom/ets_sys.h"

static const char *HAL_TAG = "DMX_HAL";

void dmx_hal_init(void) {
    uart_bypass_init_all();
}

void dmx_hal_start(void) {
    uart_bypass_start_timer();
}

bool dmx_hal_send(int port, const uint8_t *data, int len, int break_len) {
#if DMX_SW_UART_MODE == 2
    /* Mode 2: HW UART без ESP-IDF driver — прямой доступ к регистрам */
    uart_dev_t *hw = (port == 0) ? (&UART1) : (&UART2);

    /* BREAK: инвертировать TX → LOW на 176мкс */
    hw->conf0.txd_inv = 1;
    ets_delay_us(176);

    /* MAB: нормальный TX → HIGH на 16мкс */
    hw->conf0.txd_inv = 0;
    ets_delay_us(16);

    /* Отправить данные через TX FIFO */
    int sent = 0;
    while (sent < len) {
        uint32_t fifo_free = 128 - uart_ll_get_txfifo_len(hw);
        if (fifo_free > 0) {
            int chunk = (len - sent) < (int)fifo_free ? (len - sent) : (int)fifo_free;
            uart_ll_write_txfifo(hw, data + sent, chunk);
            sent += chunk;
        }
        if (sent < len) {
            ets_delay_us(10);  /* даём FIFO освободиться */
        }
    }

    /* Ждать завершения передачи */
    while (!(hw->int_st.val & UART_TX_DONE_INT_ST)) {}
    hw->int_clr.val = UART_TX_DONE_INT_CLR;

    return true;
#else
    uart_port_t uart_num = (port == 0) ? UART_NUM_1 : UART_NUM_2;

    uart_set_line_inverse(uart_num, UART_SIGNAL_TXD_INV);
    ets_delay_us(176);

    uart_set_line_inverse(uart_num, UART_SIGNAL_INV_DISABLE);
    ets_delay_us(16);

    uart_write_bytes(uart_num, data, len);
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100));

    return true;
#endif
}
