#pragma once
#include "esp_err.h"

// lvgl_port_init() must be called before oled_init().
esp_err_t oled_init(int sda_gpio, int scl_gpio);
void      oled_clear(void);
void      oled_show_text(const char *text);   // top-left, word-wrapped, replaces previous text