#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "board_pins.h"
#include "status_led.h"
#include "buttons.h"
#include "display.h"
#include "face.h"
#include "mcp.h"
#include "oled.h"
#include "servo.h"

static const char *TAG = "tong";
#define SERVO_STREAM_TEST 0      // set to 0 after the test
void servo_stream_test(void);

/* ---------- face test playlist: actions, states, expressions, sequences ----------
 * Actions play on top of whatever face was selected last, so the first YES
 * presses after boot play them over "idle". */

typedef struct {
    const char *label;
    const face_step_t *steps;
    int n;
    face_loop_t loop;
} demo_seq_t;

static const face_step_t SEQ_SURPRISE[] = {
    { "surprised-wide-left", 150, 800, FACE_EASE_SNAPPY },
    { "joyful-wide",         500, 0,   FACE_EASE_SMOOTH },
};
static const face_step_t SEQ_LOOK_AROUND[] = {
    { "far-right-glance",   400, 700, FACE_EASE_SMOOTH },
    { "attentive-left",     400, 700, FACE_EASE_SMOOTH },
    { "upward-side-glance", 400, 900, FACE_EASE_SPRING },
};
static const face_step_t SEQ_NOD_OFF[] = {
    { "sleepy-squint", 600, 900,  FACE_EASE_SMOOTH },
    { "drowsy-closed", 600, 900,  FACE_EASE_SMOOTH },
    { "eyes-closed",   600, 1500, FACE_EASE_SMOOTH },
};
static const demo_seq_t DEMOS[] = {
    { "surprise>happy", SEQ_SURPRISE,    2, FACE_LOOP_ONCE },       /* stays on happy */
    { "look around",    SEQ_LOOK_AROUND, 3, FACE_LOOP_REPEAT },
    { "nod off",        SEQ_NOD_OFF,     3, FACE_LOOP_PING_PONG },
};
#define DEMO_COUNT ((int)(sizeof(DEMOS) / sizeof(DEMOS[0])))

static int item_count(void)
{
    return face_action_count() + face_state_count() + face_expression_count() + DEMO_COUNT;
}

static void play_item(int i)
{
    const int na = face_action_count(), ns = face_state_count(), ne = face_expression_count();
    char text[48];
    esp_err_t err;
    if (i < na) {
        err = face_do_action(face_action_name(i));
        snprintf(text, sizeof(text), "action %d/%d\n%s", i + 1, na, face_action_name(i));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "action %d: %s", i, esp_err_to_name(err));
        }
        oled_show_text(text);
        return;
    }
    i -= na;
    if (i < ns) {
        err = face_play_state(face_state_name(i));
        snprintf(text, sizeof(text), "state %d/%d\n%s", i + 1, ns, face_state_name(i));
    } else if (i < ns + ne) {
        const int k = i - ns;
        err = face_show_expression(face_expression_name(k), FACE_TRANSITION_DEFAULT_MS);
        snprintf(text, sizeof(text), "expr %d/%d\n%s", k + 1, ne, face_expression_name(k));
    } else {
        const demo_seq_t *d = &DEMOS[i - ns - ne];
        err = face_play_sequence(d->steps, d->n, d->loop);
        snprintf(text, sizeof(text), "seq %d/%d\n%s", i - ns - ne + 1, DEMO_COUNT, d->label);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "item %d: %s", i, esp_err_to_name(err));
    }
    oled_show_text(text);
}

/* Bad calls must be refused and leave the face as it is. */
static void face_self_check(void)
{
    static const face_step_t zero[] = { { "neutral", 0, 0, FACE_EASE_SMOOTH } };
    const struct { const char *what; esp_err_t got, want; } c[] = {
        { "unknown state",      face_play_state("no-such-state"),              ESP_ERR_NOT_FOUND },
        { "unknown expression", face_show_expression("no-such-face", 0),       ESP_ERR_NOT_FOUND },
        { "zero-time repeat",   face_play_sequence(zero, 1, FACE_LOOP_REPEAT), ESP_ERR_INVALID_ARG },
        { "unknown action",     face_do_action("no-such-action"),              ESP_ERR_NOT_FOUND },
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        ESP_LOGI(TAG, "self-check %s: %s (%s)", c[i].what, esp_err_to_name(c[i].got),
                 c[i].got == c[i].want ? "ok" : "UNEXPECTED");
    }
}

/* Logs when an expression or a once-sequence has finished, when an action
 * ends, and when the device goes online/offline (a host attached or gone). */
static void face_watch_task(void *arg)
{
    (void)arg;
    bool was_finished = false, was_online = false;
    const char *was_action = NULL;
    int64_t action_t0 = 0;
    for (;;) {
        face_status_t st;
        if (face_get_status(&st) == ESP_OK) {
            if (st.finished && !was_finished) {
                ESP_LOGI(TAG, "finished: %s", st.name ? st.name : "sequence");
            }
            was_finished = st.finished;
            if (st.action != was_action) {
                const int64_t now = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (was_action != NULL) {
                    ESP_LOGI(TAG, "action %s ended after ~%d ms", was_action, (int)(now - action_t0));
                }
                action_t0 = now;
                was_action = st.action;
            }
        }
        const bool online = mcp_host_online();
        if (online != was_online) {
            was_online = online;
            ESP_LOGI(TAG, "%s", online ? "online: YES = pat, NO = neck on/off"
                                       : "offline: YES = next, NO = previous");
            oled_show_text(online ? "online" : "offline\nYES/NO: browse");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init(PIN_STATUS_LED));
    status_led_set(STATUS_LED_BOOTING);

    if (buttons_init(PIN_BTN_YES, PIN_BTN_NO) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    if (servo_init(PIN_SERVO_PAN, PIN_SERVO_TILT) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    // LVGL runs once for the whole program; both displays attach to it.
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    // ST7789 first: the first display added becomes LVGL's default, which face draws on.
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

    if (oled_init(PIN_OLED_SDA, PIN_OLED_SCL) != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    if (face_init() != ESP_OK) {
        status_led_set(STATUS_LED_ERROR);
        return;
    }

    // MCP over the console UART (see tools/tong_bridge.py). Without it the face still runs.
    if (mcp_start() != ESP_OK) {
        ESP_LOGW(TAG, "MCP server did not start; continuing without it");
    }

    status_led_set(STATUS_LED_OK);

    face_self_check();
    xTaskCreate(face_watch_task, "face_watch", 3072, NULL, 2, NULL);

    // Offline (no host): YES/NO browse the test playlist; the first YES goes to
    // the first action. Online (the bridge is attached): YES pats Tong on the
    // head, NO turns the neck on or off.
    const int n = item_count();
    int i = -1;
    oled_show_text("offline\nYES/NO: browse");
    ESP_LOGI(TAG, "boot complete: %d items (%d actions, %d states, %d expressions, %d sequences)",
             n, face_action_count(), face_state_count(), face_expression_count(), DEMO_COUNT);

    while (1) {
        const button_t b = buttons_wait(portMAX_DELAY);
        if (mcp_host_online()) {
            if (b == BUTTON_YES) {
                ESP_LOGI(TAG, "pat");
                mcp_pat();
            } else {
                const bool on = !face_get_servo_follow();
                face_set_servo_follow(on);
                oled_show_text(on ? "neck on" : "neck off");
            }
            continue;
        }
        i = (b == BUTTON_YES) ? (i + 1) % n : (i <= 0 ? n - 1 : i - 1);
        mcp_cancel_auto_return();       // a face picked by hand stays
        play_item(i);
    }
}
