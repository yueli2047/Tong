#pragma once

// Pin map: the single source of truth for every GPIO the firmware uses.

#define PIN_STATUS_LED   48   // onboard RGB (WS2812)

#define PIN_TFT_SCLK     12
#define PIN_TFT_MOSI     11
#define PIN_TFT_CS       10
#define PIN_TFT_DC        9
#define PIN_TFT_RST      13
#define PIN_TFT_BL       14

#define PIN_OLED_SDA      5
#define PIN_OLED_SCL      4

#define PIN_SERVO_PAN    47
#define PIN_SERVO_TILT   21

#define PIN_BTN_YES       6
#define PIN_BTN_NO        7
