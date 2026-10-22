#pragma once
#include "esp_err.h"

// lvgl_port_init() must be called before oled_init().
esp_err_t oled_init(int sda_gpio, int scl_gpio);
void      oled_clear(void);
// Top-left, word-wrapped, replaces the previous text. Text taller than the
// screen (4 lines) scrolls up a line at a time and starts over at the end.
void      oled_show_text(const char *text);