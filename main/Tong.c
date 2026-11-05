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

/* ---------- demo playlist: actions, expressions, states ----------
 * Offline, Tong plays every item in this order by itself, then starts over.
 * A button press steps by hand instead (YES next, NO previous); after
 * AUTO_RESUME_MS without a press the automatic run carries on from there.
 * Custom sequences are not part of it: those are made online, by the model. */

#define AUTO_BOOT_MS        2000    /* resting face after boot before the run starts */
#define AUTO_RESUME_MS      30000   /* no press for this long: the automatic run resumes */
#define AUTO_GAP_MS         800     /* after an action, before the next item */
#define AUTO_EXPRESSION_MS  2200    /* per expression, transition included */
#define AUTO_STATE_MS       4500    /* per state */

static int item_count(void)
{
    return face_action_count() + face_expression_count() + face_state_count();
}

/* How long the automatic run stays on item i. */
static int item_ms(int i)
{
    const int na = face_action_count(), ne = face_expression_count();
    if (i < na) {
        const int d = face_action_duration_ms(i);
        return (d > 0 ? d : 1500) + AUTO_GAP_MS;
    }
    return (i < na + ne) ? AUTO_EXPRESSION_MS : AUTO_STATE_MS;
}

static void play_item(int i)
{
    const int na = face_action_count(), ne = face_expression_count(), ns = face_state_count();
    char text[48];
    esp_err_t err;
    if (i < na) {
        if (i == 0) {       /* the actions are shown on the resting face */
            face_show_expression(FACE_REST_EXPRESSION, FACE_TRANSITION_DEFAULT_MS);
        }
        err = face_do_action(face_action_name(i));
        snprintf(text, sizeof(text), "action %d/%d\n%s", i + 1, na, face_action_name(i));
    } else if (i < na + ne) {
        const int k = i - na;
        err = face_show_expression(face_expression_name(k), FACE_TRANSITION_DEFAULT_MS);
        snprintf(text, sizeof(text), "expression %d/%d\n%s", k + 1, ne, face_expression_name(k));
    } else {
        const int k = i - na - ne;
        err = face_play_state(face_state_name(k));
        snprintf(text, sizeof(text), "state %d/%d\n%s", k + 1, ns, face_state_name(k));
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
                                       : "offline: demo playlist (YES next, NO previous)");
            oled_show_text(online ? "online" : "offline");
            if (online) {   /* the model starts from the resting face, not a playlist item */
                face_stop_action();
                face_show_expression(FACE_REST_EXPRESSION, FACE_TRANSITION_DEFAULT_MS);
            }
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

    // Offline (no host): the demo playlist runs by itself; YES/NO step through
    // it by hand. Online (the bridge is attached): the model owns the face,
    // YES pats Tong on the head, NO turns the neck on or off.
    const int n = item_count();
    int i = -1;
    TickType_t wait = pdMS_TO_TICKS(AUTO_BOOT_MS);
    oled_show_text("Tong");
    ESP_LOGI(TAG, "boot complete: %d items (%d actions, %d expressions, %d states)",
             n, face_action_count(), face_expression_count(), face_state_count());

    while (1) {
        const button_t b = buttons_wait(wait);
        if (mcp_host_online()) {
            if (b == BUTTON_YES) {
                ESP_LOGI(TAG, "pat");
                mcp_pat();
            } else if (b == BUTTON_NO) {
                const bool on = !face_get_servo_follow();
                face_set_servo_follow(on);
                oled_show_text(on ? "neck on" : "neck off");
            }
            wait = pdMS_TO_TICKS(AUTO_RESUME_MS);
            continue;
        }
        if (b != BUTTON_NONE) {         // by hand
            i = (b == BUTTON_YES) ? (i + 1) % n : (i <= 0 ? n - 1 : i - 1);
            wait = pdMS_TO_TICKS(AUTO_RESUME_MS);
        } else {                        // the automatic run
            i = (i + 1) % n;
            wait = pdMS_TO_TICKS(item_ms(i));
        }
        mcp_cancel_auto_return();       // a playlist face stays until the next item
        play_item(i);
    }
}
