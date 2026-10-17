/*
 * Frame renderer: one face_frame_t -> pixels in the framebuffer, redrawing
 * only what changed since the previous frame.
 *
 *   nothing changed          -> nothing drawn (holds without blink/ambient)
 *   head moved or recolored  -> the whole head (old area + new area)
 *   only the eyes changed    -> each eye's old + new area (blinks)
 *
 * When the head has not moved, the previously drawn ellipse is reused, so
 * the pixels around an eye redraw are identical to the ones already there.
 * No LVGL here; also built on the host for tests.
 */
#include <string.h>
#include "face_priv.h"
#include "face_data.h"

static struct {
    bool valid;                 /* false: next frame repaints the whole screen */
    face_frame_t frame;         /* last frame rendered */
    face_shapes_t shapes;       /* as drawn */
    uint32_t body_rgb, eyes_rgb;
} R;

static face_shapes_t s_new;     /* static: ~3 KB, off the caller's stack */

void face_render_reset(void)
{
    R.valid = false;
}

/* Exact: even a sub-pixel move changes edge coverage near the top and bottom
 * of the outline, so any change means a head redraw. A held head is
 * bit-identical between frames and never triggers this. */
static bool head_moved(const face_ellipse_t *a, const face_ellipse_t *b)
{
    if (a->valid != b->valid) {
        return true;
    }
    return a->valid && (a->cx != b->cx || a->cy != b->cy || a->a != b->a || a->b != b->b ||
                        a->rot != b->rot);
}

static bool eye_changed(const face_poly_t *a, const face_poly_t *b)
{
    return a->visible != b->visible || a->n != b->n ||
           memcmp(a->xy, b->xy, (size_t)a->n * 2 * sizeof(float)) != 0;
}

static bool rects_touch(face_rect_t a, face_rect_t b)
{
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

void face_render(uint16_t *fb, int stride, const face_frame_t *fr, uint16_t bg, face_dirty_t *out)
{
    out->n = 0;
    if (R.valid && memcmp(fr, &R.frame, sizeof(*fr)) == 0) {
        return;                 /* nothing moved */
    }

    face_geom_build_rot(&fr->pose, fr->rot, &FACE_BODY, true, &s_new);
    face_rect_t rects[2];
    int n = 0;

    if (!R.valid) {
        rects[n++] = (face_rect_t){ 0, 0, FACE_SCREEN_W, FACE_SCREEN_H };
    } else if (fr->body_rgb != R.body_rgb || head_moved(&s_new.head, &R.shapes.head)) {
        rects[n++] = face_rect_union(face_shapes_bounds(&R.shapes), face_shapes_bounds(&s_new));
    } else {
        s_new.head = R.shapes.head;         /* keep the head pixel-identical */
        for (int i = 0; i < 2; i++) {
            if (fr->eyes_rgb != R.eyes_rgb || eye_changed(&s_new.eye[i], &R.shapes.eye[i])) {
                const face_rect_t r = face_rect_union(face_poly_bounds(&R.shapes.eye[i]),
                                                      face_poly_bounds(&s_new.eye[i]));
                if (!face_rect_empty(r)) {
                    rects[n++] = r;
                }
            }
        }
        if (n == 2 && rects_touch(rects[0], rects[1])) {
            rects[0] = face_rect_union(rects[0], rects[1]);
            n = 1;
        }
    }

    const uint16_t body = face_rgb565(fr->body_rgb), eyes = face_rgb565(fr->eyes_rgb);
    for (int i = 0; i < n; i++) {
        const face_rect_t r = face_rect_clip_screen(rects[i]);
        if (face_rect_empty(r)) {
            continue;
        }
        face_raster_shapes(fb, stride, r, &s_new, bg, body, eyes);
        out->r[out->n++] = r;
    }

    R.valid = true;
    R.frame = *fr;
    R.shapes = s_new;
    R.body_rgb = fr->body_rgb;
    R.eyes_rgb = fr->eyes_rgb;
}
