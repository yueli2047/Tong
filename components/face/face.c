/*
 * Face component: LVGL glue and public API.
 *
 * LVGL only moves pixels here. The face is rasterized by face_raster.c
 * straight into a full-screen RGB565 canvas buffer in PSRAM; afterwards only
 * the changed rectangle is handed to LVGL, which copies it into its own
 * DMA buffers and flushes it over SPI.
 */
#include <string.h>
#include "face.h"
#include "face_priv.h"
#include "face_data.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "face";

#define BG_RGB  0x000000

static struct {
    lv_obj_t *canvas;
    uint16_t *fb;
    int stride;                 /* pixels per framebuffer row */
    face_rect_t drawn;          /* screen area covered by the last frame */
    int expr;
    int64_t refr_t0;            /* timing of the LVGL refresh after a render */
    bool refr_measure;
} s = { .expr = -1 };

/* Static: ~3 KB, kept off the LVGL task's stack */
static face_shapes_t s_shapes;

/* Runs in the LVGL task. REFR_READY comes after the last chunk was handed to
 * the flush callback; that chunk's DMA (~3 ms) may still be on the wire. */
static void refr_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s.refr_t0 = esp_timer_get_time();
    } else if (s.refr_measure) {
        s.refr_measure = false;
        ESP_LOGI(TAG, "lvgl refresh %d us", (int)(esp_timer_get_time() - s.refr_t0));
    }
}

/* Caller holds the display lock. */
static void render(const face_pose_t *pose, uint32_t body_rgb, uint32_t eyes_rgb)
{
    const int64_t t0 = esp_timer_get_time();

    face_geom_build(pose, &FACE_BODY, &s_shapes);
    const face_rect_t now = face_rect_clip_screen(face_shapes_bounds(&s_shapes));
    /* redraw what the old frame covered as well, so nothing is left behind */
    const face_rect_t dirty = face_rect_clip_screen(face_rect_union(s.drawn, now));

    const int64_t t1 = esp_timer_get_time();

    face_raster_shapes(s.fb, s.stride, dirty, &s_shapes,
                       face_rgb565(BG_RGB), face_rgb565(body_rgb), face_rgb565(eyes_rgb));
    s.drawn = now;

    const int64_t t2 = esp_timer_get_time();

    if (!face_rect_empty(dirty)) {
        const lv_area_t area = { dirty.x0, dirty.y0, dirty.x1 - 1, dirty.y1 - 1 };  /* inclusive */
        lv_obj_invalidate_area(s.canvas, &area);
        s.refr_measure = true;
    }
    ESP_LOGI(TAG, "render: geometry %d us + raster %d us, dirty %dx%d",
             (int)(t1 - t0), (int)(t2 - t1), dirty.x1 - dirty.x0, dirty.y1 - dirty.y0);
}

static void show_locked(int index)
{
    const face_expr_def_t *e = &FACE_EXPRS[index];
    face_pose_t pose;
    face_pose_from_expr(e, &pose);
    render(&pose, e->body_rgb, e->eyes_rgb);
    s.expr = index;
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
    const face_rect_t screen = { 0, 0, FACE_SCREEN_W, FACE_SCREEN_H };
    face_raster_fill(s.fb, s.stride, screen, face_rgb565(BG_RGB));

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

    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);

    s.drawn = screen;           /* first frame repaints everything */
    show_locked(FACE_EXPR_NEUTRAL);

    display_unlock();
    ESP_LOGI(TAG, "init ok: avatar \"%s\", %d expressions, %d states",
             FACE_AVATAR_NAME, FACE_EXPR_COUNT, FACE_ANIM_COUNT);
    return ESP_OK;
}

int face_expression_count(void)
{
    return FACE_EXPR_COUNT;
}

const char *face_expression_name(int index)
{
    return (index >= 0 && index < FACE_EXPR_COUNT) ? FACE_EXPR_NAMES[index] : NULL;
}

esp_err_t face_show_expression(int index)
{
    if (index < 0 || index >= FACE_EXPR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.canvas == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    show_locked(index);
    display_unlock();
    return ESP_OK;
}
