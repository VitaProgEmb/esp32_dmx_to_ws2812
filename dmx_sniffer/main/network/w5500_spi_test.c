/**
 * @brief Тест SPI-коммуникации с W5500
 *
 * Использует правильный SPI-протокол W5500 как в ESP-IDF драйвере:
 *   command_bits = 16 (адрес регистра)
 *   address_bits = 8  (BSB + RWB + OM)
 *
 * Формат кадра:
 *   [16-bit register addr] [8-bit control: BSB|RWB|OM] [Data]
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "SPI_TEST";

/* W5500 Block Select Bits */
#define W5500_BSB_COM_REG  0x00

/* W5500 Access Modes */
#define W5500_ACCESS_MODE_READ   0
#define W5500_ACCESS_MODE_WRITE  1
#define W5500_SPI_OP_MODE_VDM    0

/* W5500 register addresses (16-bit offsets) */
#define W5500_REG_MR       0x0000
#define W5500_REG_SHAR     0x0009
#define W5500_REG_VERSIONR 0x0039

/* Build 32-bit address map: (offset << 16) | (bsb << 3) */
#define W5500_ADDR(offset, bsb)  (((offset) << 16) | ((bsb) << 3))

static spi_device_handle_t s_spi;

/**
 * @brief W5500 SPI read — правильный протокол
 *
 * Формат кадра на шине:
 *   cmd (16 bits) = register offset
 *   addr (8 bits) = BSB[7:3] | RWB[2] | OM[1:0]
 *   data (N bits) = читаемые данные
 */
static void w5500_read_regs(uint32_t address, uint8_t *data, uint32_t len) {
    uint16_t cmd = (uint16_t)(address >> 16);
    uint8_t ctrl = (uint8_t)((address & 0x07) | (W5500_ACCESS_MODE_READ << 2) | W5500_SPI_OP_MODE_VDM);

    spi_transaction_t t = {
        .flags = len <= 4 ? SPI_TRANS_USE_RXDATA : 0,
        .cmd = cmd,
        .addr = ctrl,
        .length = 8 * len,
        .rx_buffer = len <= 4 ? NULL : data,
    };
    spi_device_polling_transmit(s_spi, &t);
    if (len <= 4) {
        memcpy(data, t.rx_data, len);
    }
}

/**
 * @brief W5500 SPI write — правильный протокол
 */
static void w5500_write_regs(uint32_t address, const uint8_t *data, uint32_t len) {
    uint16_t cmd = (uint16_t)(address >> 16);
    uint8_t ctrl = (uint8_t)((address & 0x07) | (W5500_ACCESS_MODE_WRITE << 2) | W5500_SPI_OP_MODE_VDM);

    spi_transaction_t t = {
        .cmd = cmd,
        .addr = ctrl,
        .length = 8 * len,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

void w5500_spi_test(void) {
    ESP_LOGI(TAG, "=== W5500 SPI Test (correct protocol) ===");

    /* 1. Init SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = W5500_SPI_MOSI,
        .miso_io_num   = W5500_SPI_MISO,
        .sclk_io_num   = W5500_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Важно: command_bits=16, address_bits=8 — как в ESP-IDF W5500 драйвере */
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 1 * 1000 * 1000,
        .spics_io_num = W5500_SPI_CS,
        .command_bits = 16,
        .address_bits = 8,
        .queue_size = 5,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(err));
        return;
    }

    /* 2. Read VERSIONR */
    uint32_t addr_ver = W5500_ADDR(W5500_REG_VERSIONR, W5500_BSB_COM_REG);
    uint8_t ver = 0;
    w5500_read_regs(addr_ver, &ver, 1);
    ESP_LOGI(TAG, "W5500 VERSIONR [0x0039] = 0x%02X (expected 0x04)", ver);

    /* 3. Read MAC (SHAR, 0x0009-0x000E) */
    uint32_t addr_mac = W5500_ADDR(W5500_REG_SHAR, W5500_BSB_COM_REG);
    uint8_t mac[6] = {0};
    w5500_read_regs(addr_mac, mac, 6);
    ESP_LOGI(TAG, "W5500 MAC [0x0009] = %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 4. Read MODE register */
    uint32_t addr_mr = W5500_ADDR(W5500_REG_MR, W5500_BSB_COM_REG);
    uint8_t mr = 0;
    w5500_read_regs(addr_mr, &mr, 1);
    ESP_LOGI(TAG, "W5500 MR [0x0000] = 0x%02X", mr);

    /* 5. Write+Read test: write 0xAA to MR, read back */
    uint8_t val = 0xAA;
    w5500_write_regs(addr_mr, &val, 1);
    uint8_t mr2 = 0;
    w5500_read_regs(addr_mr, &mr2, 1);
    ESP_LOGI(TAG, "W5500 MR write 0xAA, read back = 0x%02X (expected 0xAA)", mr2);
    val = 0x00;
    w5500_write_regs(addr_mr, &val, 1);

    /* 6. Write MAC and read back */
    uint8_t test_mac[6] = {0x02, 0x00, 0x11, 0x22, 0x33, 0x44};
    w5500_write_regs(addr_mac, test_mac, 6);
    uint8_t mac2[6] = {0};
    w5500_read_regs(addr_mac, mac2, 6);
    ESP_LOGI(TAG, "W5500 MAC after write = %02X:%02X:%02X:%02X:%02X:%02X (expected 02:00:11:22:33:44)",
             mac2[0], mac2[1], mac2[2], mac2[3], mac2[4], mac2[5]);

    bool write_ok = (memcmp(test_mac, mac2, 6) == 0);
    bool read_ok = (ver == 0x04);
    ESP_LOGI(TAG, "SPI Read test:  %s (version=0x%02X)", read_ok ? "PASS" : "FAIL", ver);
    ESP_LOGI(TAG, "SPI Write test: %s", write_ok ? "PASS" : "FAIL");

    ESP_LOGI(TAG, "=== Test Complete ===");

    /* Cleanup */
    spi_bus_remove_device(s_spi);
    spi_bus_free(SPI2_HOST);
}
