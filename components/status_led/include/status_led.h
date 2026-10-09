#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_BOOTING,  // blue
    STATUS_LED_OK,       // off
    STATUS_LED_ERROR,    // red
} status_led_state_t;

esp_err_t status_led_init(int gpio_num);
esp_err_t status_led_set(status_led_state_t state);

#ifdef __cplusplus
}
#endif