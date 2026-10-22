idf_component_register(SRCS "Tong.c" "servo_test.c"
                       PRIV_REQUIRES status_led buttons display face oled servo mcp esp_timer esp_psram
                       INCLUDE_DIRS ".")
