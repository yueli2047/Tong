#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_NONE = 0,  // timeout, nothing pressed
    BUTTON_YES,
    BUTTON_NO,
} button_t;

esp_err_t buttons_init(int yes_gpio, int no_gpio);

// Block until a button is pressed or timeout expires.
// Use portMAX_DELAY to wait forever.
button_t buttons_wait(TickType_t timeout);

// Discard any presses that happened before this call.
void buttons_flush(void);

#ifdef __cplusplus
}
#endif