/*
 * Face component: LVGL glue and public API.
 *
 * Every FRAME_MS an LVGL timer (so it runs in the LVGL task, holding the
 * LVGL lock) picks up the latest request, advances the playback, drives the
 * servos, renders what changed straight into a full-screen RGB565 canvas
 * buffer in PSRAM, and hands only those areas to LVGL, which copies them into
 * its DMA buffers and flushes them over SPI. The face is the only caller of
 * the servo component.
 *
 * The public play/show/follow calls run in the caller's task: they resolve
 * names, validate, and leave a request in a small slot guarded by a spinlock.
 * Only the frame timer touches the playback, so callers never need the LVGL
 * lock and never wait for a frame.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "face.h"
#include "face_priv.h"
#include "face_data.h"
#include "face_action_data.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "face";

#define BG_RGB          0x000000
#define FRAME_MS        33          /* ~30 fps */
#define INIT_STATE      "idle"      /* the player starts on a state; the resting face replaces it before the first frame */
#define FOLLOW_AT_BOOT  true        /* servos follow the head from power-on */

/* Periodic timing summary in the log; set to 0 once the numbers are known. */
#define FACE_STATS          1
#define FACE_STATS_EVERY_MS 5000

_Static_assert(FACE_SEQ_MAX_STEPS == FACE_CUSTOM_MAX_STEPS, "sequence size mismatch");

static struct {
    lv_obj_t *canvas;
    lv_timer_t *timer;
    uint16_t *fb;
    int stride;                 /* pixels per framebuffer row */
} s;

/* ---------- requests: written by any task, taken by the frame timer ---------- */

typedef struct {
    face_showing_t kind;
    int anim;                   /* FACE_SHOWING_STATE: index into FACE_ANIMS */
    uint8_t playback;           /* otherwise: a custom program */
    uint8_t n;
    face_step_def_t steps[FACE_SEQ_MAX_STEPS];
} request_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {                 /* everything here is guarded by s_lock */
    bool ready;                 /* face_init() done */
    bool req_pending;
    request_t req;
    bool act_pending;
    int act;                    /* FACE_ACTIONS index, -1 = stop */
    bool follow_pending;
    bool follow_on;             /* last setting requested */
    face_status_t status;       /* includes a pending request */
} q;

static request_t s_taken;       /* frame timer only */

#if FACE_STATS
static struct {
    int64_t since;
    int frames, renders;
    int64_t render_sum;
    int render_max;
    int64_t pixels;
    bool refr_pending, refr_running;
    int64_t refr_t0, refr_sum;
    int refr_n, refr_max;
} st;

/* REFR_READY comes after the last chunk was handed to the flush callback;
 * that chunk's DMA (up to ~5 ms) may still be on the wire. */
static void refr_event_cb(lv_event_t *e)
{
    const int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        st.refr_running = st.refr_pending;
        st.refr_pending = false;
        st.refr_t0 = now;
    } else if (st.refr_running) {
        st.refr_running = false;
        const int us = (int)(now - st.refr_t0);
        st.refr_sum += us;
        st.refr_n++;
        if (us > st.refr_max) st.refr_max = us;
    }
}

static void stats_report(int64_t now)
{
    if (now - st.since < FACE_STATS_EVERY_MS * 1000LL) {
        return;
    }
    const int secs = (int)((now - st.since) / 1000000);
    ESP_LOGI(TAG, "%ds: %d frames, %d drawn | draw avg %d us max %d us, avg %d px | lvgl refresh avg %d us max %d us",
             secs, st.frames, st.renders,
             st.renders ? (int)(st.render_sum / st.renders) : 0, st.render_max,
             st.renders ? (int)(st.pixels / st.renders) : 0,
             st.refr_n ? (int)(st.refr_sum / st.refr_n) : 0, st.refr_max);
    const bool pending = st.refr_pending, running = st.refr_running;
    const int64_t t0 = st.refr_t0;
    memset(&st, 0, sizeof(st));
    st.since = now;
    st.refr_pending = pending;
    st.refr_running = running;
    st.refr_t0 = t0;
}
#endif

static float esp_rand01(void)
{
    return (float)(esp_random() >> 8) * (1.0f / 16777216.0f);
}

/* Take the pending requests and follow setting, if any, and start them.
 * The face goes first, so an action requested in the same frame can still
 * borrow its eyes. */
static void take_requests(int64_t now_ms)
{
    bool have_req, have_act, follow_set, follow_on;
    int act;
    portENTER_CRITICAL(&s_lock);
    have_req = q.req_pending;
    if (have_req) {
        s_taken = q.req;
        q.req_pending = false;
    }
    have_act = q.act_pending;
    act = q.act;
    q.act_pending = false;
    follow_set = q.follow_pending;
    follow_on = q.follow_on;
    q.follow_pending = false;
    portEXIT_CRITICAL(&s_lock);

    if (follow_set) {
        face_follow_set(follow_on, now_ms);
    }
    if (have_req) {
        if (s_taken.kind == FACE_SHOWING_STATE) {
            face_anim_play(s_taken.anim, now_ms);
        } else {
            face_anim_play_custom(s_taken.steps, s_taken.n, s_taken.playback, now_ms);
        }
    }
    if (have_act) {
        face_action_play(act, now_ms);
    }
}

/* Caller holds the display lock. */
static void draw_frame(void)
{
    const int64_t t0 = esp_timer_get_time();
    const int64_t now_ms = t0 / 1000;
    take_requests(now_ms);
    float act[3];
    face_action_update(now_ms, act);    /* before the face: it may give the eyes back */
    face_frame_t fr;
    face_anim_update(now_ms, &fr);
    memcpy(fr.act, act, sizeof(act));

    const bool finished = face_anim_finished();
    const int action = face_action_current();
    portENTER_CRITICAL(&s_lock);
    if (!q.req_pending) {       /* a newer request is not finished yet */
        q.status.finished = finished;
    }
    if (!q.act_pending) {
        q.status.action = action >= 0 ? FACE_ACTIONS[action].name : NULL;
    }
    portEXIT_CRITICAL(&s_lock);

    face_follow_apply(&fr, now_ms);
    face_dirty_t d;
    face_render(s.fb, s.stride, &fr, face_rgb565(BG_RGB), &d);

    for (int i = 0; i < d.n; i++) {
        const lv_area_t area = { d.r[i].x0, d.r[i].y0, d.r[i].x1 - 1, d.r[i].y1 - 1 };  /* inclusive */
        lv_obj_invalidate_area(s.canvas, &area);
    }

#if FACE_STATS
    const int64_t t1 = esp_timer_get_time();
    if (st.since == 0) {
        st.since = t0;
    }
    st.frames++;
    if (d.n > 0) {
        const int us = (int)(t1 - t0);
        st.renders++;
        st.render_sum += us;
        if (us > st.render_max) st.render_max = us;
        for (int i = 0; i < d.n; i++) {
            st.pixels += (int64_t)(d.r[i].x1 - d.r[i].x0) * (d.r[i].y1 - d.r[i].y0);
        }
        st.refr_pending = true;
    }
    stats_report(t1);
#endif
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    draw_frame();
}

static int find_state(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < FACE_ANIM_COUNT; i++) {
        if (strcmp(FACE_ANIMS[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_expression(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < FACE_EXPR_COUNT; i++) {
        if (strcmp(FACE_EXPR_NAMES[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t face_init(void)
{
    if (s.canvas != NULL) {
        return ESP_OK;
    }
    lv_display_t *disp = display_get();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (lv_display_get_horizontal_resolution(disp) != FACE_SCREEN_W ||
        lv_display_get_vertical_resolution(disp) != FACE_SCREEN_H) {
        ESP_LOGE(TAG, "expected a %dx%d display", FACE_SCREEN_W, FACE_SCREEN_H);
        return ESP_ERR_INVALID_SIZE;
    }

    /* the same stride LVGL computes for the canvas, so both sides agree */
    const uint32_t stride_bytes = lv_draw_buf_width_to_stride(FACE_SCREEN_W, LV_COLOR_FORMAT_RGB565);
    const size_t bytes = (size_t)stride_bytes * FACE_SCREEN_H;
    s.fb = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.fb == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the %u-byte framebuffer", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    s.stride = (int)(stride_bytes / sizeof(uint16_t));

    if (!display_lock(0)) {
        heap_caps_free(s.fb);
        s.fb = NULL;
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG_RGB), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s.canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s.canvas, s.fb, FACE_SCREEN_W, FACE_SCREEN_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s.canvas, 0, 0);

#if FACE_STATS
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);
#endif

    int start = find_state(INIT_STATE);
    if (start < 0) {
        start = 0;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    face_anim_init(start, now_ms, esp_rand01);
    face_action_init();
    face_follow_init(FOLLOW_AT_BOOT, now_ms);

    /* From power-on the face rests (FACE_REST_EXPRESSION), before the first frame. */
    const int rest = find_expression(FACE_REST_EXPRESSION);
    face_status_t first = { FACE_SHOWING_STATE, FACE_ANIMS[start].name, false, NULL };
    if (rest >= 0) {
        const face_step_def_t step = { (uint8_t)rest, FACE_TRANSITION_SMOOTH, 0, 0 };
        face_anim_play_custom(&step, 1, FACE_PLAYBACK_ONCE, now_ms);
        first = (face_status_t){ FACE_SHOWING_EXPRESSION, FACE_EXPR_NAMES[rest], false, NULL };
    } else {
        ESP_LOGW(TAG, "no expression \"%s\" to rest on; starting \"%s\"", FACE_REST_EXPRESSION, FACE_ANIMS[start].name);
    }

    portENTER_CRITICAL(&s_lock);
    q.status = first;
    q.follow_on = FOLLOW_AT_BOOT;
    q.ready = true;
    portEXIT_CRITICAL(&s_lock);

    face_render_reset();        /* first frame paints the whole screen */
    draw_frame();
    s.timer = lv_timer_create(frame_cb, FRAME_MS, NULL);

    display_unlock();
    ESP_LOGI(TAG, "init ok: avatar \"%s\", %d expressions, %d states, %d actions, starting \"%s\", servo follow %s",
             FACE_AVATAR_NAME, FACE_EXPR_COUNT, FACE_ANIM_COUNT, FACE_ACTION_COUNT, first.name,
             FOLLOW_AT_BOOT ? "on" : "off");
    return ESP_OK;
}

/* ---------- names ---------- */

int face_state_count(void)
{
    return FACE_ANIM_COUNT;
}

const char *face_state_name(int index)
{
    return (index >= 0 && index < FACE_ANIM_COUNT) ? FACE_ANIMS[index].name : NULL;
}

int face_expression_count(void)
{
    return FACE_EXPR_COUNT;
}

const char *face_expression_name(int index)
{
    return (index >= 0 && index < FACE_EXPR_COUNT) ? FACE_EXPR_NAMES[index] : NULL;
}

int face_action_count(void)
{
    return FACE_ACTION_COUNT;
}

const char *face_action_name(int index)
{
    return (index >= 0 && index < FACE_ACTION_COUNT) ? FACE_ACTIONS[index].name : NULL;
}

const char *face_action_description(int index)
{
    return (index >= 0 && index < FACE_ACTION_COUNT) ? FACE_ACTIONS[index].description : NULL;
}

int face_action_duration_ms(int index)
{
    if (index < 0 || index >= FACE_ACTION_COUNT) {
        return -1;
    }
    const face_action_def_t *a = &FACE_ACTIONS[index];
    return a->keys[a->key_count - 1].t_ms;
}

/* ---------- play / show ---------- */

/* Replace any pending request; the next frame starts it. */
static esp_err_t submit(const request_t *r, const char *name)
{
    portENTER_CRITICAL(&s_lock);
    const bool ready = q.ready;
    if (ready) {
        q.req = *r;
        q.req_pending = true;
        q.status.kind = r->kind;
        q.status.name = name;
        q.status.finished = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t face_play_state(const char *name)
{
    const int index = find_state(name);
    if (index < 0) {
        ESP_LOGW(TAG, "no state \"%s\"", name ? name : "(null)");
        return ESP_ERR_NOT_FOUND;
    }
    const request_t r = { .kind = FACE_SHOWING_STATE, .anim = index };
    const esp_err_t err = submit(&r, FACE_ANIMS[index].name);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "state %s", FACE_ANIMS[index].name);
    }
    return err;
}

esp_err_t face_show_expression(const char *name, int transition_ms)
{
    const int index = find_expression(name);
    if (index < 0) {
        ESP_LOGW(TAG, "no expression \"%s\"", name ? name : "(null)");
        return ESP_ERR_NOT_FOUND;
    }
    if (transition_ms < 0 || transition_ms > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    request_t r = { .kind = FACE_SHOWING_EXPRESSION, .playback = FACE_PLAYBACK_ONCE, .n = 1 };
    r.steps[0] = (face_step_def_t){ (uint8_t)index, FACE_TRANSITION_SMOOTH, (uint16_t)transition_ms, 0 };
    const esp_err_t err = submit(&r, FACE_EXPR_NAMES[index]);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "expression %s (%d ms)", FACE_EXPR_NAMES[index], transition_ms);
    }
    return err;
}

esp_err_t face_play_sequence(const face_step_t *steps, int n, face_loop_t loop)
{
    static const uint8_t playback[] = {
        [FACE_LOOP_ONCE] = FACE_PLAYBACK_ONCE,
        [FACE_LOOP_REPEAT] = FACE_PLAYBACK_LOOP,
        [FACE_LOOP_PING_PONG] = FACE_PLAYBACK_PING_PONG,
    };
    static const uint8_t transition[] = {
        [FACE_EASE_SMOOTH] = FACE_TRANSITION_SMOOTH,
        [FACE_EASE_SNAPPY] = FACE_TRANSITION_SNAPPY,
        [FACE_EASE_SPRING] = FACE_TRANSITION_SPRING,
    };
    if (steps == NULL || n < 1 || n > FACE_SEQ_MAX_STEPS ||
        (unsigned)loop >= sizeof(playback) / sizeof(playback[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    request_t r = { .kind = FACE_SHOWING_SEQUENCE, .playback = playback[loop], .n = (uint8_t)n };
    uint32_t cycle_ms = 0;
    for (int i = 0; i < n; i++) {
        const int index = find_expression(steps[i].expression);
        if (index < 0) {
            ESP_LOGW(TAG, "sequence step %d: no expression \"%s\"", i,
                     steps[i].expression ? steps[i].expression : "(null)");
            return ESP_ERR_NOT_FOUND;
        }
        if ((unsigned)steps[i].ease >= sizeof(transition) / sizeof(transition[0])) {
            return ESP_ERR_INVALID_ARG;
        }
        r.steps[i] = (face_step_def_t){ (uint8_t)index, transition[steps[i].ease],
                                        steps[i].transition_ms, steps[i].hold_ms };
        cycle_ms += (uint32_t)steps[i].transition_ms + steps[i].hold_ms;
    }
    if (loop != FACE_LOOP_ONCE && cycle_ms == 0) {
        ESP_LOGW(TAG, "a repeating sequence needs some time per cycle");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = submit(&r, NULL);
    if (err == ESP_OK) {
        static const char *const loop_name[] = { "once", "repeat", "ping-pong" };
        ESP_LOGI(TAG, "sequence: %d steps, %s, %s ... %s", n, loop_name[loop],
                 FACE_EXPR_NAMES[r.steps[0].expr], FACE_EXPR_NAMES[r.steps[n - 1].expr]);
    }
    return err;
}

/* ---------- actions ---------- */

static esp_err_t submit_action(int index)
{
    portENTER_CRITICAL(&s_lock);
    const bool ready = q.ready;
    if (ready) {
        q.act = index;
        q.act_pending = true;
        q.status.action = index >= 0 ? FACE_ACTIONS[index].name : NULL;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t face_do_action(const char *name)
{
    int index = -1;
    for (int i = 0; name != NULL && i < FACE_ACTION_COUNT; i++) {
        if (strcmp(FACE_ACTIONS[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        ESP_LOGW(TAG, "no action \"%s\"", name ? name : "(null)");
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = submit_action(index);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "action %s", FACE_ACTIONS[index].name);
    }
    return err;
}

esp_err_t face_stop_action(void)
{
    const esp_err_t err = submit_action(-1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "action stop");
    }
    return err;
}

esp_err_t face_get_status(face_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    const bool ready = q.ready;
    *out = q.status;
    portEXIT_CRITICAL(&s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ---------- servo following ---------- */

esp_err_t face_set_servo_follow(bool on)
{
    portENTER_CRITICAL(&s_lock);
    const bool ready = q.ready;
    if (ready) {
        q.follow_on = on;
        q.follow_pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "servo follow %s", on ? "on" : "off");
    return ESP_OK;
}

bool face_get_servo_follow(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool on = q.ready && q.follow_on;
    portEXIT_CRITICAL(&s_lock);
    return on;
}
