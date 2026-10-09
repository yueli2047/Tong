#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sclk;
    int mosi;
    int cs;
    int dc;
    int rst;
    int bl;
} display_pins_t;

esp_err_t display_init(const display_pins_t *pins);

lv_display_t *display_get(void);

// LVGL is not thread-safe: wrap every LVGL call in lock/unlock.
// timeout_ms = 0 waits forever.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

#ifdef __cplusplus
}
#endif