/*
 * Playback layer: state (animation) -> one pose per frame.
 *
 * Follows the playback of Bible Strong Avatar Lab's exported runtime
 * (https://github.com/smontlouis/bible-strong-avatar-lab, AGPL-3.0):
 *   - a state is a list of steps; each step eases to an expression over
 *     transitionMs, then holds it for holdMs
 *   - loop / once / pingPong playback
 *   - blinks on the state's own schedule
 *   - ambient motion (microSaccades / shake / slowDrift) per expression,
 *     faded in during transitions
 *   - switching state eases from wherever the face currently is
 *
 * Timers are scheduled on exact due times rather than on the frame that
 * notices them, so the timeline does not drift with the frame rate. The
 * module has no ESP-IDF dependencies; it is also built on the host for tests.
 */
#include <math.h>
#include <string.h>
#include "face_priv.h"
#include "face_data.h"

/* ---------- the animatable values of an expression ---------- */

enum {
    V_HEAD_X, V_HEAD_Y, V_HEAD_Z,
    V_L_W, V_L_H, V_L_X, V_L_Y, V_L_ANGLE,
    V_R_W, V_R_H, V_R_X, V_R_Y, V_R_ANGLE,
    V_SPACING, V_PERSPECTIVE,
    V_COUNT,
};

static const int s_angle_fields[] = { V_HEAD_X, V_HEAD_Y, V_HEAD_Z, V_L_ANGLE, V_R_ANGLE };

typedef struct {
    float v[V_COUNT];
    uint8_t eye_motion, body_motion;
    uint32_t body_rgb, eyes_rgb;
} look_t;

static void look_from_expr(const face_expr_def_t *e, look_t *l)
{
    l->v[V_HEAD_X] = e->head[0];
    l->v[V_HEAD_Y] = e->head[1];
    l->v[V_HEAD_Z] = e->head[2];
    for (int i = 0; i < 2; i++) {
        float *d = &l->v[i ? V_R_W : V_L_W];
        d[0] = e->eye[i].w;
        d[1] = e->eye[i].h;
        d[2] = e->eye[i].x;
        d[3] = e->eye[i].y;
        d[4] = e->eye[i].angle;
    }
    l->v[V_SPACING] = e->spacing;
    l->v[V_PERSPECTIVE] = e->perspective;
    l->eye_motion = e->eye_motion;
    l->body_motion = e->body_motion;
    l->body_rgb = e->body_rgb;
    l->eyes_rgb = e->eyes_rgb;
}

/* ---------- state ---------- */

static struct {
    int anim, step, dir;
    bool playing;               /* false once a 'once' state has finished */
    int64_t step_due;

    look_t cur;                 /* where the face is, as of the last update */

    bool tr;                    /* transition in progress */
    look_t from, to;
    int64_t tr_start;
    int tr_ms;
    uint8_t tr_kind;
    float strength;             /* ambient motion fades in with the transition */

    uint8_t eye_sig, body_sig;  /* ambient clocks restart when the motion kind changes */
    int64_t eye_t0, body_t0;

    bool blink_scheduled, blinking;
    int64_t blink_due, blink_start;
    int blink_ms;
    float openness;

    float (*rand01)(void);
} A;

static uint32_t s_rng = 0x9e3779b9u;

static float default_rand01(void)
{
    s_rng ^= s_rng << 13;       /* xorshift32 */
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng >> 8) * (1.0f / 16777216.0f);
}

/* ---------- helpers ---------- */

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float ease(float p, uint8_t kind)
{
    switch (kind) {
    case FACE_TRANSITION_SNAPPY: {
        const float q = 1.0f - p;
        return 1.0f - q * q * q;
    }
    case FACE_TRANSITION_SPRING:            /* overshoots slightly past 1 */
        return 1.0f - expf(-6.0f * p) * cosf(8.0f * p);
    default:
        return p * p * (3.0f - 2.0f * p);   /* smooth */
    }
}

static float nearest_angle(float target, float current)
{
    while (target - current > 180.0f) target -= 360.0f;
    while (target - current < -180.0f) target += 360.0f;
    return target;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        const float ca = (float)((a >> sh) & 0xff), cb = (float)((b >> sh) & 0xff);
        const uint32_t c = (uint32_t)floorf(ca + (cb - ca) * t + 0.5f);
        out |= (c > 255 ? 255 : c) << sh;
    }
    return out;
}

/* ---------- ambient motion (lab formulas, double precision like the lab) ---------- */

#define EYE_SEED 17.29

static double smoothstep(double e)
{
    return e * e * (3.0 - 2.0 * e);
}

/* deterministic pseudo-random value in [-1, 1) */
static double hash_noise(double e)
{
    const double t = sin(e * 127.1 + 311.7) * 43758.5453;
    return (t - floor(t)) * 2.0 - 1.0;
}

/* smooth random wander: a new target every `period` ms */
static double value_noise(double t, double ch, double seed, double period)
{
    const double o = t / period, s = floor(o), c = smoothstep(o - s);
    const double l = hash_noise(s * 3.0 + ch + seed);
    return l + (hash_noise((s + 1.0) * 3.0 + ch + seed) - l) * c;
}

/* quick jump to a new target every 1100 ms, taking 140 ms */
static double saccade(double t, double ch, double seed)
{
    const double a = 1100.0;
    if (t <= 0.0) {
        return 0.0;
    }
    const double o = floor(t / a), s = (t - o * a) / 140.0, c = smoothstep(s < 1.0 ? s : 1.0);
    const double l = o == 0.0 ? 0.0 : hash_noise((o - 1.0) * 2.0 + ch + seed);
    return l + (hash_noise(o * 2.0 + ch + seed) - l) * c;
}

static double motion_seed(const look_t *l)
{
    return (double)l->v[V_HEAD_X] * 0.71 + (double)l->v[V_HEAD_Y] * 1.13 + (double)l->v[V_HEAD_Z] * 1.37;
}

/* ---------- timeline ---------- */

static void apply_motion(const look_t *target, int64_t now)
{
    if (target->eye_motion != A.eye_sig) {
        A.eye_sig = target->eye_motion;
        A.eye_t0 = now;
    }
    if (target->body_motion != A.body_sig) {
        A.body_sig = target->body_motion;
        A.body_t0 = now;
    }
}

static void animate_to(int expr, int ms, uint8_t kind, int64_t now)
{
    look_t target;
    look_from_expr(&FACE_EXPRS[expr], &target);
    apply_motion(&target, now);
    for (size_t i = 0; i < sizeof(s_angle_fields) / sizeof(s_angle_fields[0]); i++) {
        const int f = s_angle_fields[i];
        target.v[f] = nearest_angle(target.v[f], A.cur.v[f]);
    }
    if (ms <= 0) {
        A.tr = false;
        A.strength = 1.0f;
        A.cur = target;
        return;
    }
    A.tr = true;
    A.from = A.cur;
    A.to = target;
    A.tr_start = now;
    A.tr_ms = ms;
    A.tr_kind = kind;
    A.strength = 0.0f;
}

static void run_step(int64_t now)
{
    const face_step_def_t *st = &FACE_ANIMS[A.anim].steps[A.step];
    animate_to(st->expr, st->transition_ms, st->transition, now);
    A.step_due = now + st->transition_ms + st->hold_ms;
}

static void advance(int64_t now)
{
    const face_anim_def_t *an = &FACE_ANIMS[A.anim];
    const int last = an->step_count - 1;
    if (an->playback == FACE_PLAYBACK_ONCE && A.step >= last) {
        A.playing = false;      /* stays on the last expression */
        return;
    }
    if (an->playback == FACE_PLAYBACK_PING_PONG && last > 0) {
        if (A.step >= last) {
            A.dir = -1;
        } else if (A.step <= 0) {
            A.dir = 1;
        }
        A.step += A.dir;
    } else {
        A.step = (A.step + 1) % (last + 1);
    }
    run_step(now);
}

static void schedule_blink(int delay_ms, int64_t now)
{
    const face_blink_def_t *b = &FACE_ANIMS[A.anim].blink;
    A.blink_scheduled = b->enabled;
    A.blink_due = now + delay_ms;
}

static void fire_blink(void)
{
    const face_blink_def_t *b = &FACE_ANIMS[A.anim].blink;
    const int64_t t = A.blink_due;
    A.blinking = true;
    A.blink_start = t;
    A.blink_ms = b->duration_ms > 0 ? b->duration_ms : 1;
    const int range = (int)b->max_interval_ms - (int)b->min_interval_ms;
    schedule_blink(b->duration_ms + b->min_interval_ms + (int)(A.rand01() * (float)range), t);
}

/* Step and blink timers due up to `now`, in time order, each at its own due time. */
static void process_timers(int64_t now)
{
    for (;;) {
        const bool step = A.playing && A.step_due <= now;
        const bool blink = A.blink_scheduled && A.blink_due <= now;
        if (!step && !blink) {
            return;
        }
        if (step && (!blink || A.step_due <= A.blink_due)) {
            advance(A.step_due);
        } else {
            A.blink_scheduled = false;
            fire_blink();
        }
    }
}

/* ---------- public (face_priv.h) ---------- */

void face_anim_init(int anim, int64_t now_ms, float (*rand01)(void))
{
    memset(&A, 0, sizeof(A));
    A.rand01 = rand01 ? rand01 : default_rand01;
    A.openness = 1.0f;
    A.strength = 1.0f;
    A.anim = anim;
    look_from_expr(&FACE_EXPRS[FACE_ANIMS[anim].steps[0].expr], &A.cur);
    A.eye_sig = A.cur.eye_motion;
    A.body_sig = A.cur.body_motion;
    A.eye_t0 = A.body_t0 = now_ms;
    face_anim_play(anim, now_ms);
}

void face_anim_play(int anim, int64_t now_ms)
{
    /* whatever was due before the switch happens first (e.g. a blink already started) */
    process_timers(now_ms);
    A.anim = anim;
    A.step = 0;
    A.dir = 1;
    A.playing = true;
    run_step(now_ms);
    schedule_blink(FACE_ANIMS[anim].blink.initial_delay_ms, now_ms);
    /* a blink already under way finishes on its own */
}

int face_anim_current(void)
{
    return A.anim;
}

void face_anim_update(int64_t now, face_frame_t *out)
{
    process_timers(now);

    if (A.tr) {
        const float linear = clamp01((float)(now - A.tr_start) / (float)A.tr_ms);
        const float e = ease(linear, A.tr_kind);
        A.strength = clamp01(e);
        for (int i = 0; i < V_COUNT; i++) {
            A.cur.v[i] = A.from.v[i] + (A.to.v[i] - A.from.v[i]) * e;
        }
        A.cur.eye_motion = A.to.eye_motion;
        A.cur.body_motion = A.to.body_motion;
        A.cur.body_rgb = lerp_rgb(A.from.body_rgb, A.to.body_rgb, A.strength);
        A.cur.eyes_rgb = lerp_rgb(A.from.eyes_rgb, A.to.eyes_rgb, A.strength);
        if (linear >= 1.0f) {
            A.cur = A.to;
            A.tr = false;
            A.strength = 1.0f;
        }
    }

    if (A.blinking) {
        const float p = clamp01((float)(now - A.blink_start) / (float)A.blink_ms);
        if (p <= 0.42f) {
            const float c = p / 0.42f;
            A.openness = 1.0f - c * c;
        } else {
            const float o = 1.0f - (p - 0.42f) / 0.58f;
            A.openness = 1.0f - o * o;
        }
        if (p >= 1.0f) {
            A.openness = 1.0f;
            A.blinking = false;
        }
    }

    /* compose the frame: current look + blink + ambient motion */
    memset(out, 0, sizeof(*out));
    float v[V_COUNT];
    memcpy(v, A.cur.v, sizeof(v));
    const double k = A.strength;
    const double seed = motion_seed(&A.cur);
    const double eb = (double)(now - A.body_t0);
    const double ee = (double)(now - A.eye_t0);

    switch (A.cur.body_motion) {
    case FACE_MOTION_SLOW_DRIFT:
        v[V_HEAD_X] += (float)(value_noise(eb, 0, seed, 2600) * 0.8 * k);
        v[V_HEAD_Y] += (float)(value_noise(eb, 1, seed, 3300) * 1.15 * k);
        v[V_HEAD_Z] += (float)(value_noise(eb, 2, seed, 4100) * 0.45 * k);
        out->pose.body_dx = (float)(value_noise(eb, 3, seed, 2900) * 1.45 * k);
        out->pose.body_dy = (float)(value_noise(eb, 4, seed, 3700) * 1.1 * k);
        break;
    case FACE_MOTION_SHAKE: {
        const double s = eb / 1000.0;
        v[V_HEAD_X] += (float)((sin(s * 31) + sin(s * 53) * 0.45) * 1.15 * k);
        v[V_HEAD_Y] += (float)((sin(s * 37) + sin(s * 61) * 0.4) * 1.35 * k);
        v[V_HEAD_Z] += (float)(sin(s * 43) * 0.7 * k);
        out->pose.body_dx = (float)((sin(s * 31) + sin(s * 53) * 0.45) * 1.35 * k);
        out->pose.body_dy = (float)((sin(s * 37) + sin(s * 61) * 0.4) * 1.1 * k);
        break;
    }
    default:
        break;
    }

    switch (A.cur.eye_motion) {
    case FACE_MOTION_MICRO_SACCADES:
        out->pose.eye_dx = (float)(saccade(ee, 0, EYE_SEED) * 1.5 * k);
        out->pose.eye_dy = (float)(saccade(ee, 1, EYE_SEED) * 0.9 * k);
        break;
    case FACE_MOTION_SHAKE: {
        const double s = ee / 1000.0;
        out->pose.eye_dx = (float)((sin(s * 47) + sin(s * 71) * 0.45) * 1.2 * k);
        out->pose.eye_dy = (float)((sin(s * 59) + sin(s * 83) * 0.4) * 0.8 * k);
        break;
    }
    default:
        break;
    }

    face_pose_t *p = &out->pose;
    p->head[0] = v[V_HEAD_X];
    p->head[1] = v[V_HEAD_Y];
    p->head[2] = v[V_HEAD_Z];
    for (int i = 0; i < 2; i++) {
        const float *s = &v[i ? V_R_W : V_L_W];
        p->eye[i] = (face_eye_param_t){ s[0], s[1], s[2], s[3], s[4] };
    }
    p->spacing = v[V_SPACING];
    p->perspective = v[V_PERSPECTIVE];
    p->openness = A.openness;
    out->base_head[0] = A.cur.v[V_HEAD_X];
    out->base_head[1] = A.cur.v[V_HEAD_Y];
    out->base_head[2] = A.cur.v[V_HEAD_Z];
    face_geom_head_rotation(p->head, out->rot);
    out->body_rgb = A.cur.body_rgb;
    out->eyes_rgb = A.cur.eyes_rgb;
}
