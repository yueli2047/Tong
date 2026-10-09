idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio)