#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Animated face on the ST7789, drawn from tong.avatar.json.
 *
 * Three ways to say what the face does. They share one channel: each call
 * replaces whatever was playing, and the face eases over from where it is.
 *
 *   state       one of the avatar's animations ("idle", "thinking", ...):
 *               a loop of expressions with its own pace and blinking
 *   expression  one of the avatar's expressions ("joyful-wide", ...):
 *               eases there and holds it, blinking like "idle"
 *   sequence    expressions put together at run time ("surprised, then
 *               happy"), played once, repeated or ping-ponged, blinking
 *               like "idle"
 *
 * All of them go through the same playback, so transitions, blinking, ambient
 * motion and servo following behave the same way for each.
 *
 * Actions ("nod", "shake", "doze", ...) are a second channel on top: a
 * one-shot head motion added to wherever the face points, so "happy" plus
 * "nod" is two calls that play together. A new action cuts the current one
 * short and takes over smoothly. Some actions borrow an expression's eyes
 * for a moment (doze closes them) and give the face back afterwards; a
 * state/expression/sequence call made during the action takes the face
 * back at once. The servos follow actions like any head turn, and the
 * screen draws what they cannot (speed, limits, and roll, which they lack).
 *
 * Threading: the play/show/follow calls only validate their arguments and
 * leave a request that the face picks up on its next frame (within ~33 ms).
 * They never take the LVGL lock, so any task may call them (not ISRs). If
 * several requests arrive within one frame, the last one wins.
 */

#define FACE_SEQ_MAX_STEPS          16
#define FACE_TRANSITION_DEFAULT_MS  500     /* what the avatar's own states use */

/* The resting face: shown from power-on, and where a face set through MCP
 * returns after its hold time. Any expression name from the avatar. */
#define FACE_REST_EXPRESSION        "neutral"

typedef enum {
    FACE_EASE_SMOOTH = 0,       /* ease in and out */
    FACE_EASE_SNAPPY,           /* fast start, gentle stop */
    FACE_EASE_SPRING,           /* overshoots a little and settles */
} face_ease_t;

typedef enum {
    FACE_LOOP_ONCE = 0,         /* play through, then stay on the last step */
    FACE_LOOP_REPEAT,           /* 1, 2, 3, 1, 2, 3, ... */
    FACE_LOOP_PING_PONG,        /* 1, 2, 3, 2, 1, 2, ... */
} face_loop_t;

typedef struct {
    const char *expression;     /* an expression name; copied, need not outlive the call */
    uint16_t transition_ms;     /* ease to it over this long ... */
    uint16_t hold_ms;           /* ... then hold it this long */
    face_ease_t ease;           /* 0 = FACE_EASE_SMOOTH */
} face_step_t;

typedef enum {
    FACE_SHOWING_STATE = 0,
    FACE_SHOWING_EXPRESSION,
    FACE_SHOWING_SEQUENCE,
} face_showing_t;

typedef struct {
    face_showing_t kind;
    const char *name;           /* state or expression name; NULL for a sequence */
    bool finished;              /* an expression has been reached, or a FACE_LOOP_ONCE
                                 * sequence has played through; never for states or
                                 * repeating sequences */
    const char *action;         /* action in progress, NULL if none */
} face_status_t;

/* Call after display_init() and servo_init(). Draws on the ST7789 returned by
 * display_get(), not on LVGL's default display, starts the "idle" state and,
 * by default, servo following (faded in). */
esp_err_t face_init(void);

/* Names, for listing what can be played. NULL if out of range. */
int         face_state_count(void);
const char *face_state_name(int index);
int         face_expression_count(void);
const char *face_expression_name(int index);
int         face_action_count(void);
const char *face_action_name(int index);
const char *face_action_description(int index);    /* one line, English */
int         face_action_duration_ms(int index);    /* -1 if out of range */

/* ESP_ERR_NOT_FOUND: unknown name. ESP_ERR_INVALID_STATE: before face_init(). */
esp_err_t face_play_state(const char *name);

/* transition_ms: 0..65535, 0 jumps there at once (FACE_TRANSITION_DEFAULT_MS
 * is a good default). ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_ARG / _INVALID_STATE. */
esp_err_t face_show_expression(const char *name, int transition_ms);

/* 1..FACE_SEQ_MAX_STEPS steps, copied. A repeating sequence must take some
 * time per cycle (not every step 0 ms). ESP_ERR_NOT_FOUND names the unknown
 * expression in the log; ESP_ERR_INVALID_ARG / _INVALID_STATE otherwise. */
esp_err_t face_play_sequence(const face_step_t *steps, int n, face_loop_t loop);

/* Play an action from actions.json on top of the face. ESP_ERR_NOT_FOUND /
 * ESP_ERR_INVALID_STATE. */
esp_err_t face_do_action(const char *name);
/* Cut the current action short: the head eases back over ~0.35 s. */
esp_err_t face_stop_action(void);

/* What the face is doing, including requests not yet picked up. */
esp_err_t face_get_status(face_status_t *out);

/* Servo following. On: the gimbal turns the device toward where the face looks
 * and the screen draws the rest. Off: the servos ease back to center and the
 * screen draws the whole head turn (use it when the servos have no power: there
 * is no position feedback). Switching fades over 0.6 s; the head never jumps. */
esp_err_t face_set_servo_follow(bool on);
bool      face_get_servo_follow(void);      /* the last setting requested */

#ifdef __cplusplus
}
#endif
