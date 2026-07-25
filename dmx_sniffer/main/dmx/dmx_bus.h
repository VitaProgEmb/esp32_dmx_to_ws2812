#pragma once

/**
 * @file dmx_bus.h
 * @brief Общие типы и константы для DMX модуля
 */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define DMX_CHANNELS  512
#define DMX_FRAME_LEN 513

/** Event bits: ISR ставит, задача ждёт */
#define DMX_EVT_FRAME0  BIT0
#define DMX_EVT_FRAME1  BIT1

/** Raw DMX кадр — данные для визуализатора/отладки */
typedef struct {
    uint8_t  data[512];
    uint16_t len;
    uint32_t timestamp_ms;
} dmx_raw_frame_t;

extern EventGroupHandle_t g_dmx_events;
extern dmx_raw_frame_t    g_raw_frames[2];
