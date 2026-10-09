#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board_pins.h"
#include "status_led.h"
#include "buttons.h"
#include "display.h"

static const char *TAG = "tong";

static lv_obj_t *make_eye(lv_obj_t *parent, int cx, int cy, int r)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, r * 2, r * 2);
    lv_obj_set_pos(eye, cx - r, cy - r);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(0x00A0FF), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    return eye;
}

static void draw_test_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    make_eye(scr, 90, 93, 20);    // Stack-chan layout, bigger radius
    make_eye(scr, 230, 96, 20);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Tong");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 4, 4);
}

void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init(PIN_STATUS_LED));
    status_led_set(STATUS_LED_BOOTING);

    if (buttons_init(PIN_BTN_YES, PIN_BTN_NO) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

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

    if (display_lock(0)) {
        draw_test_screen();
        display_unlock();
    }

    status_led_set(STATUS_LED_OK);
    ESP_LOGI(TAG, "boot complete, press a button");

    while (1) {
        button_t b = buttons_wait(portMAX_DELAY);
        ESP_LOGI(TAG, "%s", (b == BUTTON_YES) ? "YES" : "NO");
    }
}