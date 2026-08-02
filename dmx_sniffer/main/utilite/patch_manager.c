/**
 * @file patch_manager.c
 * @brief Менеджер таблицы патчей (соответствие DMX-адрес → прибор/пиксель)
 *
 * Таблица патчей хранит соответствие между DMX-адресами приборов
 * и их позицией на LED-ленте. Хранится в файле /spiffs/patch.csv
 * на энергонезависимой flash-памяти (SPIFFS).
 *
 * Формат CSV:
 *   fixture,universe
 *   1,1
 *   2,1
 *   3,2
 *   ...
 *
 * где fixture — номер прибора (1-based), universe — DMX-вселенная (1 или 2).
 * DMX-адрес прибора вычисляется как (fixture - 1) * 3 + 1 (3 канала на прибор).
 */

#include "patch_manager.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "PATCH";
static const char *PATCH_PATH = "/spiffs/patch.csv";

/** Рекурсивный мьютекс (т.к. patch_to_csv_string вызывается из patch_save) */
static SemaphoreHandle_t s_patch_mutex = NULL;

/** Глобальная таблица патчей */
patch_map_t g_patch = { .count = 0 };

/**
 * @brief Монтирование файловой системы SPIFFS
 *
 * Если раздел не отформатирован — автоматически форматирует.
 * Вызывается ОДИН раз при старте.
 */
static void ensure_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path       = "/spiffs",
        .partition_label = NULL,
        .max_files       = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info(NULL, &total, &used);
    }
}

/**
 * @brief Инициализация менеджера патчей
 *
 * Создаёт рекурсивный мьютекс, монтирует SPIFFS и загружает патч из файла.
 * Должен вызываться ДО dmx_init(), т.к. SPIFFS обращается к flash
 * и временно отключает кэш CPU, что может вызвать panic при обращении
 * к GPIO ISR (если DMX-драйвер уже установлен).
 */
void patch_init(void) {
    s_patch_mutex = xSemaphoreCreateRecursiveMutex();
    ensure_spiffs();
    patch_load();
}

/**
 * @brief Загрузить таблицу патчей из CSV-файла
 *
 * Читает файл /spiffs/patch.csv и парсит строки "fixture,universe".
 * Пропускает заголовок и пустые строки.
 */
void patch_load(void) {
    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    FILE *f = fopen(PATCH_PATH, "r");
    if (!f) {
        g_patch.count = 0;
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 32768) {
        fclose(f);
        g_patch.count = 0;
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return;
    }

    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        g_patch.count = 0;
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return;
    }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    patch_apply_from_csv(buf);
    free(buf);

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
}

/**
 * @brief Разобрать CSV-текст и заполнить таблицу патчей
 *
 * Формат строки: "fixture,universe" (fixture ≥ 1, universe = 1 или 2).
 * DMX-адрес вычисляется как: (fixture - 1) * 3 + 1.
 *
 * @param csv_text Текст в формате CSV (с заголовком или без)
 */
void patch_apply_from_csv(const char *csv_text) {
    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    memset(g_patch.entries, 0, sizeof(g_patch.entries));
    g_patch.count = 0;

    const char *p = csv_text;
    int line = 0;

    while (*p && g_patch.count < PATCH_MAX_ENTRIES) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;

        // Data lines start with a digit (fixture number); skip header & empty
        if (len > 0 && *p >= '0' && *p <= '9') {
            int fixture = 0, uni = 1;
            if (sscanf(p, "%d,%d", &fixture, &uni) >= 1 && fixture > 0) {
                uint16_t idx = g_patch.count;
                g_patch.entries[idx].universe = (uni == 2) ? 2 : 1;
                g_patch.entries[idx].dmx_addr = (fixture - 1) * 3 + 1;
                g_patch.count++;
            }
        }
        line++;
        if (!nl) break;
        p = nl + 1;
    }

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
}

/**
 * @brief Сериализовать таблицу патчей в CSV-строку
 *
 * @return malloc-строка (вызывающий обязан free()) или NULL при ошибке
 */
char* patch_to_csv_string(void) {
    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    size_t buf_size = PATCH_MAX_ENTRIES * 16 + 20;
    char *buf = malloc(buf_size);
    if (!buf) {
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return NULL;
    }

    int pos = sprintf(buf, "fixture,universe\n");
    for (int i = 0; i < g_patch.count; i++) {
        int fixture = g_patch.entries[i].dmx_addr / 3 + 1;
        pos += sprintf(buf + pos, "%d,%d\n",
                       fixture,
                       g_patch.entries[i].universe);
    }

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
    return buf;
}

/**
 * @brief Сериализовать таблицу патчей в JSON-строку
 *
 * Формат: [{"dmx_addr":1,"universe":1},{"dmx_addr":4,"universe":1},...]
 *
 * @return malloc-строка (вызывающий обязан free()) или NULL при ошибке
 */
char* patch_to_json_string(void) {
    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    size_t buf_size = PATCH_MAX_ENTRIES * 64 + 16;
    char *buf = malloc(buf_size);
    if (!buf) {
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return NULL;
    }

    int pos = 0;
    pos += sprintf(buf + pos, "[");
    int first = 1;
    for (int i = 0; i < g_patch.count; i++) {
        if (g_patch.entries[i].universe == 0) continue;
        if (g_patch.entries[i].skip) continue;
        int addr = g_patch.entries[i].dmx_addr;
        pos += sprintf(buf + pos, "%s{\"dmx_addr\":%d,\"universe\":%d}",
                       first ? "" : ",",
                       addr,
                       g_patch.entries[i].universe);
        first = 0;
    }
    pos += sprintf(buf + pos, "]");

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
    return buf;
}

/**
 * @brief Добавить диапазон приборов в таблицу патчей
 *
 * @param universe      DMX-вселенная (1 или 2)
 * @param start_fixture Начальный номер прибора (0-based индекс)
 * @param count         Количество приборов
 * @return true при успехе
 */
bool patch_add_range(uint8_t universe, uint16_t start_fixture, uint16_t count) {
    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    if (g_patch.count + count > PATCH_MAX_ENTRIES) {
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return false;
    }

    uint16_t end = start_fixture + count;
    for (uint16_t i = start_fixture; i < end; i++) {
        uint16_t idx = g_patch.count;
        g_patch.entries[idx].universe = universe;
        g_patch.entries[idx].dmx_addr = i * 3 + 1;
        g_patch.entries[idx].skip = false;
        g_patch.count++;
    }

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
    return true;
}

/**
 * @brief Удалить диапазон приборов из таблицы патчей
 *
 * Сдвигает оставшиеся записи влево через memmove.
 *
 * @param start_fixture Индекс первого удаляемого прибора (0-based)
 * @param count         Количество удаляемых приборов
 * @return true при успехе
 */
bool patch_del_range(uint16_t start_fixture, uint16_t count) {
    if (start_fixture >= g_patch.count || count == 0) return false;

    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    uint16_t end = start_fixture + count;
    if (end > g_patch.count) end = g_patch.count;

    uint16_t shift = end - start_fixture;
    uint16_t remaining = g_patch.count - end;

    if (remaining > 0) {
        memmove(&g_patch.entries[start_fixture],
                &g_patch.entries[end],
                remaining * sizeof(patch_entry_t));
    }
    g_patch.count -= shift;

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
    return true;
}

/**
 * @brief Сохранить таблицу патчей в CSV-файл на SPIFFS
 *
 * Сериализует текущую таблицу в CSV и записывает в /spiffs/patch.csv.
 * Вызывается при каждом изменении патча через веб-интерфейс.
 */
void patch_save(void) {
    if (s_patch_mutex) xSemaphoreTakeRecursive(s_patch_mutex, portMAX_DELAY);

    char *csv = patch_to_csv_string();
    if (!csv) {
        if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
        return;
    }

    FILE *f = fopen(PATCH_PATH, "w");
    if (f) {
        fwrite(csv, 1, strlen(csv), f);
        fclose(f);
    } else {
        ESP_LOGE(TAG, "Failed to write patch.csv");
    }
    free(csv);

    if (s_patch_mutex) xSemaphoreGiveRecursive(s_patch_mutex);
}
