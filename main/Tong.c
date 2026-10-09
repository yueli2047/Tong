#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board_pins.h"
#include "status_led.h"
#include "buttons.h"

static const char *TAG = "tong";

void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init(PIN_STATUS_LED));
    status_led_set(STATUS_LED_BOOTING);

    if (buttons_init(PIN_BTN_YES, PIN_BTN_NO) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    status_led_set(STATUS_LED_OK);
    ESP_LOGI(TAG, "boot complete, press a button");

    while (1) {
        button_t b = buttons_wait(portMAX_DELAY);
        ESP_LOGI(TAG, "%s", (b == BUTTON_YES) ? "YES" : "NO");
    }
}