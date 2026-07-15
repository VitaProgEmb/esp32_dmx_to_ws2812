#include "debug_server.h"
#include "dmx.h"
#include "settings.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "uart_bypass.h"
#include <string.h>

static const char *TAG = "DBG";

#define DEBUG_PORT 5555

static int s_server_fd = -1;
static volatile bool s_running = false;

void debug_server_start(void) {
    s_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_server_fd < 0) { ESP_LOGE(TAG, "socket create failed"); return; }

    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DEBUG_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed"); close(s_server_fd); s_server_fd = -1; return;
    }
    if (listen(s_server_fd, 1) < 0) {
        ESP_LOGE(TAG, "listen failed"); close(s_server_fd); s_server_fd = -1; return;
    }

    ESP_LOGI(TAG, "TCP:%d", DEBUG_PORT);
    s_running = true;

    static uint32_t last_fc[2] = {0, 0};

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) break;

        char buf[8];
        int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            uint32_t c0, c1;
            for (int w = 0; w < 50; w++) {
                c0 = s_rx_frame_count[0];
                c1 = s_rx_frame_count[1];
                if (c0 != last_fc[0] || c1 != last_fc[1]) break;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            last_fc[0] = c0;
            last_fc[1] = c1;

            uint8_t resp[24 + DMX_CHANNELS * 2];
            memset(resp, 0, sizeof(resp));
            resp[0] = c0 & 0xFF;
            resp[1] = (c0 >> 8) & 0xFF;
            resp[2] = (c0 >> 16) & 0xFF;
            resp[3] = (c0 >> 24) & 0xFF;
            resp[4] = c1 & 0xFF;
            resp[5] = (c1 >> 8) & 0xFF;
            resp[6] = (c1 >> 16) & 0xFF;
            resp[7] = (c1 >> 24) & 0xFF;
            uint32_t isr0 = uart_bypass_get_isr_count(0);
            uint32_t isr1 = uart_bypass_get_isr_count(1);
            resp[8]  = isr0 & 0xFF;
            resp[9]  = (isr0 >> 8) & 0xFF;
            resp[10] = (isr0 >> 16) & 0xFF;
            resp[11] = (isr0 >> 24) & 0xFF;
            resp[12] = isr1 & 0xFF;
            resp[13] = (isr1 >> 8) & 0xFF;
            resp[14] = (isr1 >> 16) & 0xFF;
            resp[15] = (isr1 >> 24) & 0xFF;
            uint32_t brk0 = uart_bypass_get_break_count(0);
            uint32_t brk1 = uart_bypass_get_break_count(1);
            resp[16] = brk0 & 0xFF;
            resp[17] = (brk0 >> 8) & 0xFF;
            resp[18] = (brk0 >> 16) & 0xFF;
            resp[19] = (brk0 >> 24) & 0xFF;
            resp[20] = brk1 & 0xFF;
            resp[21] = (brk1 >> 8) & 0xFF;
            resp[22] = (brk1 >> 16) & 0xFF;
            resp[23] = (brk1 >> 24) & 0xFF;
            uint8_t *dmx = resp + 24;
            for (int p = 0; p < 2; p++)
                dmx_get_channel_data(p, dmx + p * DMX_CHANNELS, DMX_CHANNELS);
            send(client_fd, resp, sizeof(resp), 0);
        }

        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
    }

    s_running = false;
}

void debug_server_stop(void) {
    if (!s_running) return;
    s_running = false;
    if (s_server_fd >= 0) {
        shutdown(s_server_fd, SHUT_RDWR);
        close(s_server_fd);
        s_server_fd = -1;
    }
}

bool debug_server_is_running(void) {
    return s_running;
}
