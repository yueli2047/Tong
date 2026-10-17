#include "oled.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#define OLED_I2C_PORT       0
#define OLED_I2C_ADDR       0x3C
#define OLED_I2C_CLOCK_HZ   (400 * 1000)
#define OLED_H_RES          128
#define OLED_V_RES          64

static const char *TAG = "oled";
static lv_display_t *s_disp = NULL;
static lv_obj_t *s_label = NULL;

#define CHECK(x) do {                                               \
        esp_err_t err_ = (x);                                       \
        if (err_ != ESP_OK) {                                       \
            ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(err_)); \
            return err_;                                            \
        }                                                           \
    } while (0)

esp_err_t oled_init(int sda_gpio, int scl_gpio)
{
    if (s_disp != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .flags.enable_internal_pullup = true,
    };
    CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = OLED_I2C_CLOCK_HZ,
        .transaction_timeout_ms = 1000,
        .control_phase_bytes = 1,   // per SSD1306 datasheet
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    CHECK(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = {
        .height = OLED_V_RES,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_cfg,
    };
    CHECK(esp_lcd_new_panel_ssd1306(io, &panel_cfg, &panel));
    CHECK(esp_lcd_panel_reset(panel));
    CHECK(esp_lcd_panel_init(panel));
    CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = OLED_H_RES * OLED_V_RES,   // monochrome needs a full-frame buffer
        .double_buffer = true,
        .hres = OLED_H_RES,
        .vres = OLED_V_RES,
        .monochrome = true,
        .color_format = LV_COLOR_FORMAT_I1,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    // Module is mounted pins-up: rotate 180 degrees in the controller.
    // Done after add_disp so the port's own rotation setup cannot override it.
    CHECK(esp_lcd_panel_mirror(panel, true, true));

    // Use this display's own screen, not lv_screen_active() (that belongs to the ST7789).
    lvgl_port_lock(0);
    s_label = lv_label_create(lv_display_get_screen_active(s_disp));
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, OLED_H_RES);
    lv_obj_align(s_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_label, "");
    lvgl_port_unlock();

    ESP_LOGI(TAG, "init ok (%dx%d)", OLED_H_RES, OLED_V_RES);
    return ESP_OK;
}

void oled_clear(void)
{
    oled_show_text("");
}

void oled_show_text(const char *text)
{
    if (s_label == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(s_label, text);
    lvgl_port_unlock();
}