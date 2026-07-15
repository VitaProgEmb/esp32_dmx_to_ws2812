/**
 * @file settings_manager.h
 * @brief Функции для сохранения и загрузки настроек в NVS Flash
 */

#pragma once

#include "dmx.h"
#include "led_strip.h"

/**
 * @brief Загрузка сохраненных настроек из NVS в глобальные структуры данных
 */
void settings_load(void);

/**
 * @brief Сохранение текущих настроек в NVS с защитой от быстрого износа (debounce)
 */
void settings_save(void);
