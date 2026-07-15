#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PATCH_MAX_ENTRIES  340

typedef struct {
    uint8_t  universe;
    uint16_t dmx_addr;  // 1-512, DMX start address for this fixture
    bool     skip;
} patch_entry_t;

typedef struct {
    patch_entry_t entries[PATCH_MAX_ENTRIES];
    uint16_t      count;
} patch_map_t;

extern patch_map_t g_patch;
extern uint16_t    g_total_leds;

void     patch_init(void);
void     patch_load(void);
void     patch_save(void);
void     patch_apply_from_csv(const char *csv_text);
char*    patch_to_csv_string(void);
char*    patch_to_json_string(void);
bool     patch_add_range(uint8_t universe, uint16_t start_fixture, uint16_t count);
bool     patch_del_range(uint16_t start_fixture, uint16_t count);
