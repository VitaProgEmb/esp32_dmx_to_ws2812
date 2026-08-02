/**
 * @file settings_manager.h
 * @brief Функции для сохранения и загрузки настроек в NVS Flash
 */

#pragma once

#include "dmx/dmx.h"
#include "led_strip.h"

/**
 * @brief Инициализация NVS Flash + загрузка настроек
 */
void settings_init(void);

/**
 * @brief Сохранение текущих настроек в NVS с защитой от быстрого износа (debounce)
 */
void settings_save(void);
