#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"

#define STATUS_LED_BRIGHTNESS 20   // 0-255, kept low on purpose

static const char *TAG = "status_led";
static led_strip_handle_t s_strip = NULL;

static esp_err_t set_color(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t err = led_strip_set_pixel(s_strip, 0, r, g, b);
    if (err != ESP_OK) {
        return err;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t status_led_init(int gpio_num)
{
    if (s_strip != NULL) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }
    return led_strip_clear(s_strip);
}

esp_err_t status_led_set(status_led_state_t state)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (state) {
    case STATUS_LED_BOOTING:
        return set_color(0, 0, STATUS_LED_BRIGHTNESS);
    case STATUS_LED_ERROR:
        return set_color(STATUS_LED_BRIGHTNESS, 0, 0);
    case STATUS_LED_OK:
    default:
        return led_strip_clear(s_strip);
    }
}