#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Animated face on the ST7789, drawn from anchor.avatar.json.
 *
 * A state is one of the avatar's animations ("idle", "thinking", "happy", ...):
 * a loop of expressions with its own pace and blinking. Switching states eases
 * over from wherever the face currently is.
 */

/* Call after display_init() and servo_init(). Draws on the ST7789 returned by
 * display_get(), not on LVGL's default display, starts the "idle" state and,
 * by default, servo following (faded in). */
esp_err_t face_init(void);

int         face_state_count(void);
const char *face_state_name(int index);              /* NULL if out of range */
int         face_get_state(void);                    /* -1 before face_init() */
esp_err_t   face_set_state(int index);
esp_err_t   face_set_state_by_name(const char *name); /* ESP_ERR_NOT_FOUND if unknown */

/* Servo following. On: the gimbal turns the device toward where the face looks
 * and the screen draws the rest. Off: the servos ease back to center and the
 * screen draws the whole head turn (use it when the servos have no power: there
 * is no position feedback). Switching fades over 0.6 s; the head never jumps. */
esp_err_t   face_set_servo_follow(bool on);
bool        face_get_servo_follow(void);

#ifdef __cplusplus
}
#endif
