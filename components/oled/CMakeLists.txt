idf_component_register(SRCS "oled.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_lcd esp_driver_i2c)