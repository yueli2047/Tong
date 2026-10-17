/*
 * Internal types shared by the face component.
 *
 *   face_data.c (generated)  ->  tables of expressions / animations
 *   face_geom.c              ->  pose -> head ellipse + eye polygons (screen px)
 *   face_raster.c            ->  shapes -> RGB565 pixels, anti-aliased
 *   face.c                   ->  LVGL canvas, public API
 *
 * Coordinates follow the avatar lab: x to the viewer's right, y down,
 * z toward the viewer. Head angles are in degrees and composed as in the
 * lab, R = Rz(head[2]) * Rx(head[0]) * Ry(head[1]):
 *   head[0] (x)  > 0 : looks up
 *   head[1] (y)  > 0 : turns to the viewer's right (the device's own left)
 *   head[2] (z)  > 0 : top of the head leans to the viewer's right
 * Mapping to the gimbal (ROS REP-103) happens in face_follow.c (step 3) by
 * decomposing R into the gimbal's yaw/pitch, not by reading the angles.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- screen and framing ---------- */

#define FACE_SCREEN_W   320
#define FACE_SCREEN_H   240

/* Same framing as the lab preview: its 300-unit view (-150..150)
 * maps onto the 240 px screen height. */
#define FACE_SCALE      0.8f
#define FACE_ORIGIN_X   160.0f
#define FACE_ORIGIN_Y   120.0f

/* ---------- tables generated from anchor.avatar.json ---------- */

typedef enum {
    FACE_MOTION_NONE = 0,
    FACE_MOTION_MICRO_SACCADES,
    FACE_MOTION_SHAKE,
    FACE_MOTION_SLOW_DRIFT,
} face_motion_t;

typedef enum {
    FACE_TRANSITION_SMOOTH = 0,
    FACE_TRANSITION_SNAPPY,
    FACE_TRANSITION_SPRING,
} face_transition_t;

typedef enum {
    FACE_PLAYBACK_LOOP = 0,
    FACE_PLAYBACK_ONCE,
    FACE_PLAYBACK_PING_PONG,
} face_playback_t;

typedef struct {
    float w, h, d;              /* ellipsoid size, lab units */
} face_body_t;

typedef struct {
    float w, h;                 /* eye size */
    float x, y;                 /* offset from the default eye position */
    float angle;                /* degrees */
} face_eye_param_t;

typedef struct {
    float head[3];              /* degrees, see the convention above */
    face_eye_param_t eye[2];    /* [0] screen-left, [1] screen-right */
    float spacing;              /* distance between the eye centers */
    float perspective;
    uint8_t eye_motion;         /* face_motion_t, ambient */
    uint8_t body_motion;        /* face_motion_t, ambient */
    uint32_t body_rgb;          /* 0xRRGGBB, already resolved against the avatar default */
    uint32_t eyes_rgb;
} face_expr_def_t;

typedef struct {
    uint8_t  expr;              /* index into FACE_EXPRS */
    uint8_t  transition;        /* face_transition_t */
    uint16_t transition_ms;
    uint16_t hold_ms;
} face_step_def_t;

typedef struct {
    bool     enabled;
    uint16_t initial_delay_ms;
    uint16_t min_interval_ms;
    uint16_t max_interval_ms;
    uint16_t duration_ms;
} face_blink_def_t;

typedef struct {
    const char *name;
    uint8_t playback;           /* face_playback_t */
    uint8_t step_count;
    const face_step_def_t *steps;
    face_blink_def_t blink;
} face_anim_def_t;

/* ---------- pose: everything the geometry layer needs for one frame ---------- */

typedef struct {
    float head[3];
    face_eye_param_t eye[2];
    float spacing;
    float perspective;
    float openness;             /* 1 = open, 0 = closed (blink) */
    float eye_dx, eye_dy;       /* ambient eye offset, lab units */
    float body_dx, body_dy;     /* ambient offset of the whole picture, lab units */
} face_pose_t;

void face_pose_from_expr(const face_expr_def_t *e, face_pose_t *out);

/* ---------- geometry output ---------- */

#define FACE_EYE_MAX_PTS 192

typedef struct {
    float cx, cy;               /* center */
    float a, b;                 /* semi-axes, a along `rot` */
    float rot;                  /* radians, from +x toward +y */
    bool valid;
} face_ellipse_t;

typedef struct {
    float xy[FACE_EYE_MAX_PTS * 2];
    int n;
    bool visible;               /* false when the eye faces away */
} face_poly_t;

typedef struct {
    face_ellipse_t head;
    face_poly_t eye[2];
} face_shapes_t;

/* [x0, x1) x [y0, y1); empty when x1 <= x0 or y1 <= y0 */
typedef struct {
    int x0, y0, x1, y1;
} face_rect_t;

/* Head rotation from pose->head, lab order, row-major 3x3. */
void face_geom_head_rotation(const float head_deg[3], float m[9]);

/* Usual path: rotation from pose->head, output in screen pixels. */
void face_geom_build(const face_pose_t *pose, const face_body_t *body, face_shapes_t *out);

/* Explicit rotation (servo following draws only the part the servos have not
 * taken over). screen_space = false returns lab units, used for verification. */
void face_geom_build_rot(const face_pose_t *pose, const float rot[9], const face_body_t *body,
                         bool screen_space, face_shapes_t *out);

face_rect_t face_ellipse_bounds(const face_ellipse_t *e);
face_rect_t face_poly_bounds(const face_poly_t *p);
face_rect_t face_shapes_bounds(const face_shapes_t *s);
face_rect_t face_rect_union(face_rect_t a, face_rect_t b);
face_rect_t face_rect_clip_screen(face_rect_t r);
bool        face_rect_empty(face_rect_t r);

/* ---------- raster: RGB565, native byte order, stride in pixels ---------- */

uint16_t face_rgb565(uint32_t rgb);
void face_raster_fill(uint16_t *fb, int stride, face_rect_t clip, uint16_t color);

/* Draw a full pose inside `clip`: background, then head, then visible eyes.
 * Every pixel in `clip` is written exactly once. */
void face_raster_shapes(uint16_t *fb, int stride, face_rect_t clip, const face_shapes_t *s,
                        uint16_t bg, uint16_t body, uint16_t eyes);

/* ---------- playback (face_anim.c) ---------- */

typedef struct {
    face_pose_t pose;           /* full pose, ambient motion included */
    float base_head[3];         /* head angles without ambient motion: what the servos follow */
    float rot[9];               /* head rotation drawn on screen (row-major) */
    uint32_t body_rgb, eyes_rgb;
} face_frame_t;

/* Start state `anim` at now_ms, already showing its first expression.
 * rand01 returns [0, 1) for blink intervals; NULL uses a built-in generator. */
void face_anim_init(int anim, int64_t now_ms, float (*rand01)(void));
/* Switch state; the face eases over from where it is. */
void face_anim_play(int anim, int64_t now_ms);
int  face_anim_current(void);
/* Advance to now_ms (non-decreasing) and produce that frame.
 * out->rot is the full head rotation; servo following replaces it. */
void face_anim_update(int64_t now_ms, face_frame_t *out);

/* ---------- servo following (face_follow.c) ---------- */

/* Read the servo limits; following starts faded out and fades toward `on`. */
void face_follow_init(bool on, int64_t now_ms);
/* Fade following in or out over FOLLOW_FADE_MS. */
void face_follow_set(bool on, int64_t now_ms);
bool face_follow_enabled(void);
/* Gimbal angles (ROS REP-103, degrees: +yaw = device turns to its own left,
 * +pitch = nods down) that point the face along rotation m. */
void face_follow_gimbal_angles(const float m[9], float *yaw, float *pitch);
/* Command the servos for this frame and set fr->rot to the part of the head
 * rotation the gimbal has not taken over (what the screen should draw). */
void face_follow_apply(face_frame_t *fr, int64_t now_ms);

/* ---------- frame renderer (face_render.c) ---------- */

typedef struct {
    face_rect_t r[2];
    int n;
} face_dirty_t;

/* The next face_render() repaints the whole screen. */
void face_render_reset(void);
/* Draw `fr` into the framebuffer, touching only what changed since the last
 * call; `out` lists the areas written (0 to 2). */
void face_render(uint16_t *fb, int stride, const face_frame_t *fr, uint16_t bg, face_dirty_t *out);

#ifdef __cplusplus
}
#endif
