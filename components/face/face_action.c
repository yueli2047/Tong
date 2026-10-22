/*
 * Action layer: one-shot head motions played on top of the face.
 *
 * An action is a keyframed offset (REP-103 yaw, pitch, roll) relative to
 * wherever the face is pointing, so a nod is a nod whether the face looks
 * straight ahead or off to the side. The offset is handed to face_follow,
 * which adds it to the head: the servos take what they can, the screen
 * draws the rest, exactly as for the face's own head turns.
 *
 *   - each segment eases from one key to the next (smooth / snappy / spring)
 *   - a new action starts from the current offset instead of key 0, so
 *     cutting one action short with another (or with stop) never jumps
 *   - an action may borrow an expression's eyes for part of its run; the
 *     face's own program keeps its timing underneath and comes back after
 *
 * No ESP-IDF dependencies; also built on the host for tests.
 */
#include <string.h>
#include "face_priv.h"
#include "face_action_data.h"

#define STOP_MS         350     /* face_stop_action(): ease back to rest */
#define GIVE_BACK_MS    300     /* borrowed eyes given back when cut short */

static const face_key_def_t s_stop_keys[] = {
    { 0,       FACE_TRANSITION_SMOOTH, { 0.0f, 0.0f, 0.0f } },
    { STOP_MS, FACE_TRANSITION_SMOOTH, { 0.0f, 0.0f, 0.0f } },
};
static const face_action_def_t s_stop = {
    .name = "stop", .description = "", .expr = -1, .key_count = 2, .keys = s_stop_keys,
};

static struct {
    const face_action_def_t *def;   /* NULL: at rest */
    int index;                      /* FACE_ACTIONS index; -1 at rest or easing back */
    int64_t t0;
    float from[3];                  /* offset when it started; replaces key 0 */
    bool expr_on;                   /* this action holds the face's eyes */
} X;

void face_action_init(void)
{
    memset(&X, 0, sizeof(X));
    X.index = -1;
}

/* Offset of the current action at `now`; X.def must be set. */
static void eval(int64_t now, float out[3])
{
    const face_action_def_t *d = X.def;
    const int64_t rel = now - X.t0;
    const face_key_def_t *last = &d->keys[d->key_count - 1];
    if (rel >= last->t_ms) {
        memcpy(out, last->off, sizeof(last->off));
        return;
    }
    int i = 1;
    while (d->keys[i].t_ms <= rel) {
        i++;
    }
    const face_key_def_t *k0 = &d->keys[i - 1], *k1 = &d->keys[i];
    const float *a = (i == 1) ? X.from : k0->off;
    const float p = (float)(rel - k0->t_ms) / (float)(k1->t_ms - k0->t_ms);
    const float e = face_ease(p, k1->ease);
    for (int ax = 0; ax < 3; ax++) {
        out[ax] = a[ax] + (k1->off[ax] - a[ax]) * e;
    }
}

void face_action_play(int index, int64_t now_ms)
{
    float cur[3] = { 0.0f, 0.0f, 0.0f };
    if (X.def != NULL) {
        eval(now_ms, cur);
    }
    const face_action_def_t *d = (index >= 0 && index < FACE_ACTION_COUNT) ? &FACE_ACTIONS[index] : &s_stop;
    if (d->expr >= 0) {
        face_anim_override(d->expr, d->expr_in_ms, now_ms);
        X.expr_on = true;
    } else if (X.expr_on) {
        face_anim_override(-1, GIVE_BACK_MS, now_ms);
        X.expr_on = false;
    }
    X.def = d;
    X.index = (d == &s_stop) ? -1 : index;
    X.t0 = now_ms;
    memcpy(X.from, cur, sizeof(cur));
}

int face_action_current(void)
{
    return X.index;
}

void face_action_update(int64_t now_ms, float out[3])
{
    if (X.def == NULL) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const int64_t rel = now_ms - X.t0;
    if (X.expr_on && rel >= X.def->expr_until_ms) {
        face_anim_override(-1, X.def->expr_out_ms, now_ms);
        X.expr_on = false;
    }
    eval(now_ms, out);
    if (rel >= X.def->keys[X.def->key_count - 1].t_ms) {
        X.def = NULL;
        X.index = -1;
    }
}
