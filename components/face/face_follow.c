/*
 * Servo following: the gimbal turns the device toward where the face looks,
 * and the screen draws only what the gimbal has not taken over.
 *
 *   servos : pointing direction of the base pose (no ambient motion)
 *            x FOLLOW_GAIN x fade, clamped to the servo limits
 *   screen : R_screen = R_gimbal^T * R_head, with R_gimbal from the position
 *            the servos have actually been commanded to (servo_get_position)
 *
 * So while the servos lag, are clamped, or are fading in/out, the screen
 * makes up the difference and the head as a whole always points where the
 * animation says. Roll (head tilt) and ambient motion stay on the screen.
 *
 * Frames: lab coordinates (x right, y down, z toward the viewer).
 * Gimbal: ROS REP-103 angles, +yaw = device turns to its own left (the
 * viewer's right), +pitch = nods down. In lab terms
 *   R_gimbal = Ry(yaw) * Rx(-pitch)    (pan outside tilt)
 */
#include <math.h>
#include <string.h>
#include "face_priv.h"
#include "servo.h"

/* How much of the head turn the servos take: 1 = all of it (the face on the
 * screen stays nearly frontal), 0.5 = half, the rest drawn on screen. */
#define FOLLOW_GAIN     1.0f
/* Fade in / out when following is switched (and at boot). */
#define FOLLOW_FADE_MS  600

#define DEG2RAD(d) ((d) * 0.017453292519943295f)
#define RAD2DEG(r) ((r) * 57.29577951308232f)

static struct {
    bool on;                    /* where the fade is heading */
    float fade_from;
    int64_t fade_start;
    bool driving;               /* still sending commands to the servos */
    float yaw_min, yaw_max, pitch_min, pitch_max;
} F;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float fade_at(int64_t now)
{
    float p = (float)(now - F.fade_start) / (float)FOLLOW_FADE_MS;
    p = clampf(p, 0.0f, 1.0f);
    const float s = p * p * (3.0f - 2.0f * p);
    const float to = F.on ? 1.0f : 0.0f;
    return F.fade_from + (to - F.fade_from) * s;
}

void face_follow_init(bool on, int64_t now_ms)
{
    memset(&F, 0, sizeof(F));
    servo_get_limits(&F.yaw_min, &F.yaw_max, &F.pitch_min, &F.pitch_max);
    F.fade_from = 0.0f;         /* servos start centered (servo_init) */
    F.fade_start = now_ms;
    F.on = on;
    F.driving = on;
}

void face_follow_set(bool on, int64_t now_ms)
{
    if (on == F.on) {
        return;
    }
    F.fade_from = fade_at(now_ms);   /* reverse smoothly from wherever the fade is */
    F.fade_start = now_ms;
    F.on = on;
    F.driving = true;
}

bool face_follow_enabled(void)
{
    return F.on;
}

void face_follow_gimbal_angles(const float m[9], float *yaw, float *pitch)
{
    /* where the face points: R * (0, 0, 1) */
    const float fx = m[2], fy = m[5], fz = m[8];
    *yaw = RAD2DEG(atan2f(fx, fz));
    *pitch = RAD2DEG(asinf(clampf(fy, -1.0f, 1.0f)));   /* y is down: looking down = +pitch */
}

static void gimbal_rotation(float yaw, float pitch, float m[9])
{
    const float y = DEG2RAD(yaw), x = DEG2RAD(-pitch);
    const float cy = cosf(y), sy = sinf(y), cx = cosf(x), sx = sinf(x);
    /* Ry(y) * Rx(x) */
    const float r[9] = {
        cy,  sy * sx,  sy * cx,
        0.0f, cx,      -sx,
        -sy, cy * sx,  cy * cx,
    };
    memcpy(m, r, sizeof(r));
}

void face_follow_apply(face_frame_t *fr, int64_t now_ms)
{
    const float f = fade_at(now_ms);

    if (F.driving) {
        float base[9], yaw, pitch;
        face_geom_head_rotation(fr->base_head, base);
        face_follow_gimbal_angles(base, &yaw, &pitch);
        const float k = FOLLOW_GAIN * f;
        servo_track(clampf(yaw * k, F.yaw_min, F.yaw_max),
                    clampf(pitch * k, F.pitch_min, F.pitch_max));
        if (!F.on && f <= 0.0f) {
            F.driving = false;  /* faded out: the servos settle at the last (centered) target */
        }
    }

    /* the screen draws what the gimbal has not done (yet) */
    float yaw_c, pitch_c, g[9], head[9];
    servo_get_position(&yaw_c, &pitch_c);
    gimbal_rotation(yaw_c, pitch_c, g);
    memcpy(head, fr->rot, sizeof(head));
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            fr->rot[i * 3 + j] = g[0 * 3 + i] * head[0 * 3 + j]
                               + g[1 * 3 + i] * head[1 * 3 + j]
                               + g[2 * 3 + i] * head[2 * 3 + j];
        }
    }
}
