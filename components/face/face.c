/*
 * Face component: LVGL glue and public API.
 *
 * Every FRAME_MS an LVGL timer (so it runs in the LVGL task, holding the
 * LVGL lock) advances the playback, drives the servos, renders what changed
 * straight into a full-screen RGB565 canvas buffer in PSRAM, and hands only
 * those areas to LVGL, which copies them into its DMA buffers and flushes
 * them over SPI. The face is the only caller of the servo component.
 */
#include <string.h>
#include "face.h"
#include "face_priv.h"
#include "face_data.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "face";

#define BG_RGB          0x000000
#define FRAME_MS        33          /* ~30 fps */
#define DEFAULT_STATE   "idle"
#define FOLLOW_AT_BOOT  true        /* servos follow the head from power-on */

/* Periodic timing summary in the log; set to 0 once the numbers are known. */
#define FACE_STATS          1
#define FACE_STATS_EVERY_MS 5000

static struct {
    lv_obj_t *canvas;
    lv_timer_t *timer;
    uint16_t *fb;
    int stride;                 /* pixels per framebuffer row */
} s;

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

/* Caller holds the display lock. */
static void draw_frame(void)
{
    const int64_t t0 = esp_timer_get_time();
    const int64_t now_ms = t0 / 1000;
    face_frame_t fr;
    face_anim_update(now_ms, &fr);
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

    int start = find_state(DEFAULT_STATE);
    if (start < 0) {
        start = 0;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    face_anim_init(start, now_ms, esp_rand01);
    face_follow_init(FOLLOW_AT_BOOT, now_ms);
    face_render_reset();        /* first frame paints the whole screen */
    draw_frame();
    s.timer = lv_timer_create(frame_cb, FRAME_MS, NULL);

    display_unlock();
    ESP_LOGI(TAG, "init ok: avatar \"%s\", %d expressions, %d states, starting \"%s\", servo follow %s",
             FACE_AVATAR_NAME, FACE_EXPR_COUNT, FACE_ANIM_COUNT, FACE_ANIMS[start].name,
             FOLLOW_AT_BOOT ? "on" : "off");
    return ESP_OK;
}

int face_state_count(void)
{
    return FACE_ANIM_COUNT;
}

const char *face_state_name(int index)
{
    return (index >= 0 && index < FACE_ANIM_COUNT) ? FACE_ANIMS[index].name : NULL;
}

int face_get_state(void)
{
    return s.canvas != NULL ? face_anim_current() : -1;
}

esp_err_t face_set_state(int index)
{
    if (index < 0 || index >= FACE_ANIM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.canvas == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    face_anim_play(index, esp_timer_get_time() / 1000);
    display_unlock();
    return ESP_OK;
}

esp_err_t face_set_state_by_name(const char *name)
{
    const int index = find_state(name);
    return index < 0 ? ESP_ERR_NOT_FOUND : face_set_state(index);
}

esp_err_t face_set_servo_follow(bool on)
{
    if (s.canvas == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    face_follow_set(on, esp_timer_get_time() / 1000);
    display_unlock();
    ESP_LOGI(TAG, "servo follow %s", on ? "on" : "off");
    return ESP_OK;
}

bool face_get_servo_follow(void)
{
    return s.canvas != NULL && face_follow_enabled();
}
