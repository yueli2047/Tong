#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "board_pins.h"
#include "status_led.h"
#include "buttons.h"
#include "display.h"
#include "face.h"
#include "oled.h"
#include "servo.h"

static const char *TAG = "tong";
#define SERVO_STREAM_TEST 0      // set to 0 after the test
void servo_stream_test(void);
void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init(PIN_STATUS_LED));
    status_led_set(STATUS_LED_BOOTING);

    if (buttons_init(PIN_BTN_YES, PIN_BTN_NO) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    if (servo_init(PIN_SERVO_PAN, PIN_SERVO_TILT) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    // LVGL runs once for the whole program; both displays attach to it.
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    // ST7789 first: the first display added becomes LVGL's default, which face draws on.
    const display_pins_t disp_pins = {
        .sclk = PIN_TFT_SCLK,
        .mosi = PIN_TFT_MOSI,
        .cs   = PIN_TFT_CS,
        .dc   = PIN_TFT_DC,
        .rst  = PIN_TFT_RST,
        .bl   = PIN_TFT_BL,
    };
    if (display_init(&disp_pins) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    if (oled_init(PIN_OLED_SDA, PIN_OLED_SCL) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    if (face_init() != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    status_led_set(STATUS_LED_OK);

    oled_show_text("neutral");
    ESP_LOGI(TAG, "boot complete: YES = next expression, NO = previous");

    int expr = 0;
    const int count = face_expression_count();
    while (1) {
        button_t b = buttons_wait(portMAX_DELAY);
        expr = (b == BUTTON_YES) ? (expr + 1) % count : (expr + count - 1) % count;
        face_show_expression(expr);
        oled_show_text(face_expression_name(expr));
        ESP_LOGI(TAG, "expression %d: %s", expr, face_expression_name(expr));
    }
}