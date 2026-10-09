#include "display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)
#define LCD_H_RES           320   // landscape, after rotation
#define LCD_V_RES           240
#define LCD_BUF_LINES       40    // partial render buffer height

static const char *TAG = "display";
static lv_display_t *s_disp = NULL;

#define CHECK(x) do {                                               \
        esp_err_t err_ = (x);                                       \
        if (err_ != ESP_OK) {                                       \
            ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(err_)); \
            return err_;                                            \
        }                                                           \
    } while (0)

esp_err_t display_init(const display_pins_t *pins)
{
    if (s_disp != NULL) {
        return ESP_OK;
    }

    // Backlight off while initializing
    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << pins->bl,
    };
    CHECK(gpio_config(&bl_cfg));
    gpio_set_level(pins->bl, 0);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = pins->sclk,
        .mosi_io_num = pins->mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t),
    };
    CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = pins->dc,
        .cs_gpio_num = pins->cs,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    // Panel quirks verified with the spi_lcd_touch example:
    // RGB element order, colors inverted
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = pins->rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    CHECK(esp_lcd_panel_reset(panel));
    CHECK(esp_lcd_panel_init(panel));
    CHECK(esp_lcd_panel_invert_color(panel, true));
    CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    CHECK(lvgl_port_init(&lvgl_cfg));

    // Hardware rotation: equivalent of LV_DISPLAY_ROTATION_270 in the example
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_BUF_LINES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,   // SPI LCD is big-endian
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    gpio_set_level(pins->bl, 1);
    ESP_LOGI(TAG, "init ok (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

lv_display_t *display_get(void)
{
    return s_disp;
}

bool display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}