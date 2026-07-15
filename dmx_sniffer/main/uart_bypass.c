#include "uart_bypass.h"
#include "settings.h"
#include "esp_attr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "dmx_hal.h"
#include "dmx.h"

static const char *TAG = "UART_BYPASS";

#define UART_BAUD_RATE  250000
#define UART_RX_BUF     4096
#define UART_TX_BUF     1024

typedef struct {
    uart_port_t uart_num;
    int gpio_rx;
    int gpio_tx;

    uint8_t rx_active[BYPASS_DMX_SIZE];
    volatile uint32_t rx_head;
    bool in_frame;

    uint8_t rx_done[BYPASS_DMX_SIZE];
    volatile bool frame_ready;

    bool seen_break;

    volatile uint32_t frame_count;
    volatile uint32_t last_frame_len;

    volatile uint32_t isr_count;
    volatile uint32_t break_count;
    volatile uint32_t ovf_count;

    int64_t last_break_us;

    TaskHandle_t notify_task;
    TaskHandle_t event_task;

    QueueHandle_t uart_queue;
} hw_uart_ctx_t;

static hw_uart_ctx_t s_ctx[2];

static void uart_event_task(void *arg) {
    int port = (int)arg;
    hw_uart_ctx_t *ctx = &s_ctx[port];
    uart_event_t event;
    uint8_t tmp[256];

    while (1) {
        if (xQueueReceive(ctx->uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA: {
                int len = uart_read_bytes(ctx->uart_num, tmp, event.size, pdMS_TO_TICKS(10));
                ctx->isr_count++;
                for (int i = 0; i < len; i++) {
                    uint8_t b = tmp[i];
                    if (!ctx->in_frame) {
                        if (b == 0 && ctx->seen_break) {
                            ctx->in_frame = true;
                            ctx->rx_head = 0;
                            ctx->seen_break = false;
                        }
                        continue;
                    }
                    if (ctx->rx_head < DMX_CHANNELS) {
                        ctx->rx_active[ctx->rx_head] = b;
                        ctx->rx_head++;
                    }
                }
                break;
            }
            case UART_BREAK: {
                int64_t now = esp_timer_get_time();
                if (now - ctx->last_break_us < 1000) {
                    uart_flush_input(ctx->uart_num);
                    xQueueReset(ctx->uart_queue);
                    break;
                }
                ctx->last_break_us = now;
                ctx->break_count++;
                if (ctx->in_frame && ctx->rx_head > 0) {
                    dmx_store_frame(port, ctx->rx_active, ctx->rx_head);
                    if (ctx->notify_task)
                        xTaskNotifyGive(ctx->notify_task);
                }
                ctx->rx_head = 0;
                ctx->in_frame = false;
                ctx->seen_break = true;
                uart_flush_input(ctx->uart_num);
                xQueueReset(ctx->uart_queue);
                break;
            }
            case UART_FRAME_ERR:
                ctx->ovf_count++;
                break;
            case UART_FIFO_OVF:
                ctx->ovf_count++;
                uart_flush_input(ctx->uart_num);
                xQueueReset(ctx->uart_queue);
                ctx->rx_head = 0;
                ctx->in_frame = false;
                break;
            default:
                break;
            }
        }
    }
}

static void configure_uart(int port) {
    hw_uart_ctx_t *ctx = &s_ctx[port];

    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(ctx->uart_num, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(ctx->uart_num, ctx->gpio_tx, ctx->gpio_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(ctx->uart_num, UART_RX_BUF, UART_TX_BUF, 64,
                                         &ctx->uart_queue, 0));

    /* pull-up/pull-down настраиваем ПОСЛЕ установки драйвера,
     * иначе uart_driver_install может перезаписать конфигурацию GPIO */
    gpio_set_pull_mode(ctx->gpio_rx, GPIO_FLOATING);
}

void uart_bypass_init(int port) {
    hw_uart_ctx_t *ctx = &s_ctx[port];

    if (port == 0) {
        ctx->uart_num = UART_NUM_1;
        ctx->gpio_rx = DMX_GPIO_RX1;
        ctx->gpio_tx = DMX_GPIO_TX1;
    } else {
        ctx->uart_num = UART_NUM_2;
        ctx->gpio_rx = DMX_GPIO_RX2;  // UART2 on GPIO16 (COM6)
        ctx->gpio_tx = DMX_GPIO_TX2;
    }

    ctx->rx_head = 0;
    ctx->in_frame = false;
    ctx->frame_ready = false;
    ctx->frame_count = 0;
    ctx->last_frame_len = 0;
    ctx->isr_count = 0;
    ctx->break_count = 0;
    ctx->ovf_count = 0;
    ctx->last_break_us = 0;
    ctx->seen_break = false;
}

void uart_bypass_init_all(void) {
    uart_bypass_init(0);
    uart_bypass_init(1);

    gpio_config_t dir_conf = {
        .pin_bit_mask = (1ULL << DMX_GPIO_DIR2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dir_conf);
    gpio_set_level(DMX_GPIO_DIR2, 0);
}

void uart_bypass_start_timer(void) {
    configure_uart(0);
    configure_uart(1);

    TaskHandle_t evt0 = NULL, evt1 = NULL;
    xTaskCreatePinnedToCore(uart_event_task, "uart0_evt", 8192, (void*)0, 5, &evt0, 1);
    xTaskCreatePinnedToCore(uart_event_task, "uart1_evt", 8192, (void*)1, 5, &evt1, 1);
    s_ctx[0].event_task = evt0;
    s_ctx[1].event_task = evt1;
}

void uart_bypass_set_notify_task(int port, TaskHandle_t handle) {
    s_ctx[port].notify_task = handle;
}

void uart_bypass_set_dir(int port, int level) {
    gpio_set_level(DMX_GPIO_DIR2, level);
    ESP_LOGI(TAG, "DIR2(GPIO%d)=%d", DMX_GPIO_DIR2, gpio_get_level(DMX_GPIO_DIR2));
}

void uart_bypass_set_tx_mode(int port, bool tx_mode) {
    hw_uart_ctx_t *ctx = &s_ctx[port];
    if (tx_mode) {
        if (ctx->event_task) vTaskSuspend(ctx->event_task);
        uart_flush_input(ctx->uart_num);
        xQueueReset(ctx->uart_queue);
    } else {
        uart_flush_input(ctx->uart_num);
        xQueueReset(ctx->uart_queue);
        ctx->rx_head = 0;
        ctx->in_frame = false;
        ctx->seen_break = false;
        if (ctx->event_task) vTaskResume(ctx->event_task);
    }
}

bool uart_bypass_get_frame(int port, uint8_t *out, int max_len, uint32_t *frame_count) {
    hw_uart_ctx_t *ctx = &s_ctx[port];
    if (!ctx->frame_ready) return false;
    __asm__ __volatile__("memw" ::: "memory");
    int len = (max_len < (int)ctx->last_frame_len) ? max_len : (int)ctx->last_frame_len;
    for (int i = 0; i < len; i++) {
        out[i] = ctx->rx_done[i];
    }
    *frame_count = ctx->frame_count;
    ctx->frame_ready = false;
    return true;
}

uint32_t uart_bypass_get_isr_count(int port) {
    return s_ctx[port].isr_count;
}

uint32_t uart_bypass_get_break_count(int port) {
    return s_ctx[port].break_count;
}

uint32_t uart_bypass_get_ovf_count(int port) {
    return s_ctx[port].ovf_count;
}
