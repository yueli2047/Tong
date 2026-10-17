idf_component_register(SRCS "servo.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_mcpwm)