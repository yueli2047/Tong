#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Face on the ST7789, drawn from anchor.avatar.json.
 *
 * Step 1: still pictures of each expression, no animation yet.
 * States, transitions and blinking come in step 2; servo following in step 3.
 */

/* Call after display_init(). Draws on the ST7789 returned by display_get(),
 * not on LVGL's default display. Shows "neutral". */
esp_err_t face_init(void);

int         face_expression_count(void);
const char *face_expression_name(int index);   /* e.g. "joyful-wide"; NULL if out of range */
esp_err_t   face_show_expression(int index);    /* switches immediately */

#ifdef __cplusplus
}
#endif
