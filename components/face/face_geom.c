/*
 * Geometry layer: pose -> head ellipse + two eye polygons.
 *
 * Ported from the procedural engine of Bible Strong Avatar Lab
 * (https://github.com/smontlouis/bible-strong-avatar-lab, AGPL-3.0),
 * sphere body only. Pipeline per eye point:
 *   capsule outline in the face plane
 *   -> rotated by the eye angle, placed at its eye position
 *   -> wrapped onto a radius-120 sphere, dropped onto the ellipsoid surface
 *   -> rotated with the head
 *   -> perspective projection (focal 620 / perspective)
 * The head outline is the exact perspective projection of the rotated
 * ellipsoid, which is always an ellipse.
 *
 * Head angles are composed exactly as in the lab (roll outside pitch outside
 * yaw), so every expression looks the way it does in the lab preview.
 * Servo following (step 3) does not change this: it decomposes the head
 * rotation into the gimbal's yaw/pitch and hands face_geom_build_rot() the
 * part the servos have not taken over yet.
 */
#include <math.h>
#include <string.h>
#include "face_priv.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LAB_FOCAL        620.0f   /* perspective focal length, lab units */
#define LAB_FACE_RADIUS  120.0f   /* face-plane coordinates are arc lengths on this radius */
#define LAB_EDGE_STEP    1.5f     /* outline sampling along straight edges */
#define LAB_CORNER_PTS   14       /* outline samples per quarter-circle corner */
#define LAB_MIN_EYE_H    5.0f     /* eye height when fully closed */

#define DEG2RAD(d) ((d) * 0.017453292519943295f)

typedef struct {
    float m[9];             /* row-major rotation */
    float persp;
    const face_body_t *body;
} frame_t;

void face_pose_from_expr(const face_expr_def_t *e, face_pose_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->head, e->head, sizeof(out->head));
    memcpy(out->eye, e->eye, sizeof(out->eye));
    out->spacing = e->spacing;
    out->perspective = e->perspective;
    out->openness = 1.0f;
}

/* ---------- rotation ---------- */

static void mat_mul(const float *a, const float *b, float *out)
{
    float r[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j]
                         + a[i * 3 + 1] * b[1 * 3 + j]
                         + a[i * 3 + 2] * b[2 * 3 + j];
        }
    }
    memcpy(out, r, sizeof(r));
}

void face_geom_head_rotation(const float head_deg[3], float m[9])
{
    const float x = DEG2RAD(head_deg[0]), y = DEG2RAD(head_deg[1]), z = DEG2RAD(head_deg[2]);
    const float cx = cosf(x), sx = sinf(x);
    const float cy = cosf(y), sy = sinf(y);
    const float cz = cosf(z), sz = sinf(z);
    const float rx[9] = { 1, 0, 0,   0, cx, -sx,   0, sx, cx };
    const float ry[9] = { cy, 0, sy,   0, 1, 0,   -sy, 0, cy };
    const float rz[9] = { cz, -sz, 0,   sz, cz, 0,   0, 0, 1 };
    float t[9];
    mat_mul(rz, rx, t);         /* lab order: Rz * Rx * Ry */
    mat_mul(t, ry, m);
}

static inline void rotate(const float *m, const float v[3], float out[3])
{
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

/* ---------- eyes ---------- */

/* Face-plane point (lab units) -> projected point; accumulates the normal's z. */
static void surface_point(const frame_t *f, float fx, float fy, float *px, float *py, float *nz_sum)
{
    /* face-plane coordinates are arc lengths on the radius-120 sphere */
    const float lon = fx / LAB_FACE_RADIUS, lat = fy / LAB_FACE_RADIUS;
    const float X = LAB_FACE_RADIUS * cosf(lat) * sinf(lon);
    const float Y = LAB_FACE_RADIUS * sinf(lat);

    /* drop onto the ellipsoid surface (front half) */
    const float a = f->body->w * 0.5f, b = f->body->h * 0.5f, c = f->body->d * 0.5f;
    float s = 1.0f - (X / a) * (X / a) - (Y / b) * (Y / b);
    if (s < 0.0f) {
        s = 0.0f;
    }
    const float Z = c * sqrtf(s);
    const float p[3] = { X, Y, Z };
    float n[3] = { X / (a * a), Y / (b * b), Z / (c * c) };
    const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.0f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    }

    float pr[3], nr[3];
    rotate(f->m, p, pr);
    rotate(f->m, n, nr);

    const float den = LAB_FOCAL - pr[2] * f->persp;
    const float k = fabsf(den) < 1e-4f ? LAB_FOCAL / 1e-4f : LAB_FOCAL / den;
    *px = pr[0] * k;
    *py = pr[1] * k;
    *nz_sum += nr[2];
}

typedef struct {
    float cx, cy, ca, sa;       /* eye center in the face plane, angle */
    const frame_t *f;
    face_poly_t *poly;
    float nz_sum;
} eye_ctx_t;

static void eye_emit(eye_ctx_t *c, float lx, float ly)
{
    if (c->poly->n >= FACE_EYE_MAX_PTS) {
        return;
    }
    const float fx = c->cx + lx * c->ca - ly * c->sa;
    const float fy = c->cy + lx * c->sa + ly * c->ca;
    float *dst = &c->poly->xy[c->poly->n * 2];
    surface_point(c->f, fx, fy, &dst[0], &dst[1], &c->nz_sum);
    c->poly->n++;
}

static void eye_edge(eye_ctx_t *c, float x0, float y0, float x1, float y1)
{
    int count = (int)ceilf(hypotf(x1 - x0, y1 - y0) / LAB_EDGE_STEP);
    if (count < 2) {
        count = 2;
    }
    for (int k = 0; k < count; k++) {
        const float t = (float)k / (float)count;
        eye_emit(c, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
    }
}

static void eye_corner(eye_ctx_t *c, float ox, float oy, float start, float r)
{
    for (int k = 0; k < LAB_CORNER_PTS; k++) {
        const float a = start + (float)k / (float)LAB_CORNER_PTS * (float)(M_PI / 2.0);
        eye_emit(c, ox + cosf(a) * r, oy + sinf(a) * r);
    }
}

static void build_eye(const frame_t *f, const face_pose_t *pose, int side, face_poly_t *out)
{
    const face_eye_param_t *ep = &pose->eye[side];
    const float sign = side == 0 ? -1.0f : 1.0f;
    const float h = LAB_MIN_EYE_H + (ep->h - LAB_MIN_EYE_H) * pose->openness;
    const float ang = DEG2RAD(ep->angle);

    eye_ctx_t c = {
        .cx = sign * pose->spacing * 0.5f + ep->x + pose->eye_dx,
        .cy = ep->y + pose->eye_dy,
        .ca = cosf(ang),
        .sa = sinf(ang),
        .f = f,
        .poly = out,
        .nz_sum = 0.0f,
    };
    out->n = 0;

    /* capsule outline: corner radius = half the shorter side */
    const float n = ep->w * 0.5f, r = h * 0.5f;
    const float i = r < n ? r : n;
    const float hp = (float)(M_PI / 2.0);
    eye_edge(&c, -n + i, -r, n - i, -r);
    eye_corner(&c, n - i, -r + i, -hp, i);
    eye_edge(&c, n, -r + i, n, r - i);
    eye_corner(&c, n - i, r - i, 0.0f, i);
    eye_edge(&c, n - i, r, -n + i, r);
    eye_corner(&c, -n + i, r - i, hp, i);
    eye_edge(&c, -n, r - i, -n, -r + i);
    eye_corner(&c, -n + i, -r + i, 2.0f * hp, i);

    out->visible = c.nz_sum > 0.0f;
}

/* ---------- head outline ---------- */

/* Exact perspective projection of the rotated ellipsoid (center at the
 * origin) as an ellipse. Double precision: the conic terms cancel badly
 * in float, and this runs once per frame. */
static bool head_ellipse(const frame_t *f, face_ellipse_t *e)
{
    const double rad[3] = { (double)f->body->w * 0.5, (double)f->body->h * 0.5, (double)f->body->d * 0.5 };
    double axis[3][3];          /* rotated unit axes = columns of m */
    for (int i = 0; i < 3; i++) {
        axis[i][0] = f->m[0 * 3 + i];
        axis[i][1] = f->m[1 * 3 + i];
        axis[i][2] = f->m[2 * 3 + i];
    }

    double cxx, cxy, cyy;       /* covariance of the projected ellipse */
    double ex, ey;              /* its center */

    if (fabs((double)f->persp) < 1e-4) {
        cxx = cxy = cyy = 0.0;
        for (int i = 0; i < 3; i++) {
            const double r2 = rad[i] * rad[i];
            cxx += axis[i][0] * axis[i][0] * r2;
            cxy += axis[i][0] * axis[i][1] * r2;
            cyy += axis[i][1] * axis[i][1] * r2;
        }
        ex = ey = 0.0;
    } else {
        double inv[3], o[3][3];
        for (int i = 0; i < 3; i++) {
            inv[i] = 1.0 / (rad[i] * rad[i]);
        }
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                o[r][c] = axis[0][r] * inv[0] * axis[0][c]
                        + axis[1][r] * inv[1] * axis[1][c]
                        + axis[2][r] * inv[2] * axis[2][c];
            }
        }
        const double s = (double)LAB_FOCAL / (double)f->persp;    /* camera distance */
        const double cam[3] = { 0.0, 0.0, s };
        double l[3];
        for (int r = 0; r < 3; r++) {
            l[r] = o[r][0] * cam[0] + o[r][1] * cam[1] + o[r][2] * cam[2];
        }
        const double u = cam[0] * l[0] + cam[1] * l[1] + cam[2] * l[2] - 1.0;
        const double d[3] = { l[0], l[1], -s * l[2] };
        const double fm[3][3] = {
            { o[0][0], o[0][1], -s * o[0][2] },
            { o[1][0], o[1][1], -s * o[1][2] },
            { -s * o[2][0], -s * o[2][1], s * s * o[2][2] },
        };
        double p[3][3];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                p[r][c] = d[r] * d[c] - u * fm[r][c];
            }
        }
        const double m = p[0][0] * p[1][1] - p[0][1] * p[0][1];
        if (fabs(m) < 1e-12) {
            return false;
        }
        ex = -(p[1][1] * p[0][2] - p[0][1] * p[1][2]) / m;
        ey = (p[0][1] * p[0][2] - p[0][0] * p[1][2]) / m;
        const double k = -(p[2][2] + p[0][2] * ex + p[1][2] * ey);
        if (fabs(k) < 1e-12) {
            return false;
        }
        const double v = p[0][0] / k, y = p[0][1] / k, b = p[1][1] / k;
        const double det = v * b - y * y;
        if (det <= 0.0) {
            return false;
        }
        cxx = b / det;
        cxy = -y / det;
        cyy = v / det;
    }

    const double sum = cxx + cyy;
    const double dif = hypot(cxx - cyy, 2.0 * cxy);
    const double l1 = (sum + dif) * 0.5, l2 = (sum - dif) * 0.5;
    if (l1 <= 0.0 || l2 <= 0.0) {
        return false;
    }
    e->cx = (float)ex;
    e->cy = (float)ey;
    e->a = (float)sqrt(l1);
    e->b = (float)sqrt(l2);
    e->rot = (float)(atan2(2.0 * cxy, cxx - cyy) * 0.5);
    if (f->body->w == f->body->h && f->body->h == f->body->d) {
        const float r = (e->a + e->b) * 0.5f;   /* the lab draws a perfect sphere as a circle */
        e->a = e->b = r;
        e->rot = 0.0f;
    }
    e->valid = true;
    return true;
}

/* ---------- entry points ---------- */

void face_geom_build_rot(const face_pose_t *pose, const float rot[9], const face_body_t *body,
                         bool screen_space, face_shapes_t *out)
{
    frame_t f = { .persp = pose->perspective, .body = body };
    memcpy(f.m, rot, sizeof(f.m));

    memset(&out->head, 0, sizeof(out->head));
    head_ellipse(&f, &out->head);
    build_eye(&f, pose, 0, &out->eye[0]);
    build_eye(&f, pose, 1, &out->eye[1]);

    /* ambient body offset moves the whole picture, as in the lab */
    const float ox = pose->body_dx, oy = pose->body_dy;
    const float k = screen_space ? FACE_SCALE : 1.0f;
    const float tx = screen_space ? FACE_ORIGIN_X : 0.0f;
    const float ty = screen_space ? FACE_ORIGIN_Y : 0.0f;

    if (out->head.valid) {
        out->head.cx = tx + (out->head.cx + ox) * k;
        out->head.cy = ty + (out->head.cy + oy) * k;
        out->head.a *= k;
        out->head.b *= k;
    }
    for (int s = 0; s < 2; s++) {
        face_poly_t *p = &out->eye[s];
        for (int i = 0; i < p->n; i++) {
            p->xy[i * 2 + 0] = tx + (p->xy[i * 2 + 0] + ox) * k;
            p->xy[i * 2 + 1] = ty + (p->xy[i * 2 + 1] + oy) * k;
        }
    }
}

void face_geom_build(const face_pose_t *pose, const face_body_t *body, face_shapes_t *out)
{
    float m[9];
    face_geom_head_rotation(pose->head, m);
    face_geom_build_rot(pose, m, body, true, out);
}

/* ---------- rectangles ---------- */

bool face_rect_empty(face_rect_t r)
{
    return r.x1 <= r.x0 || r.y1 <= r.y0;
}

face_rect_t face_rect_union(face_rect_t a, face_rect_t b)
{
    if (face_rect_empty(a)) {
        return b;
    }
    if (face_rect_empty(b)) {
        return a;
    }
    return (face_rect_t){
        a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
        a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1,
    };
}

face_rect_t face_rect_clip_screen(face_rect_t r)
{
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > FACE_SCREEN_W) r.x1 = FACE_SCREEN_W;
    if (r.y1 > FACE_SCREEN_H) r.y1 = FACE_SCREEN_H;
    return r;
}

face_rect_t face_ellipse_bounds(const face_ellipse_t *e)
{
    if (!e->valid) {
        return (face_rect_t){ 0, 0, 0, 0 };
    }
    const float c = cosf(e->rot), s = sinf(e->rot);
    const float hw = sqrtf(e->a * e->a * c * c + e->b * e->b * s * s);
    const float hh = sqrtf(e->a * e->a * s * s + e->b * e->b * c * c);
    /* one extra pixel for the anti-aliased edge */
    return (face_rect_t){
        (int)floorf(e->cx - hw) - 1, (int)floorf(e->cy - hh) - 1,
        (int)ceilf(e->cx + hw) + 1, (int)ceilf(e->cy + hh) + 1,
    };
}

face_rect_t face_poly_bounds(const face_poly_t *p)
{
    if (!p->visible || p->n < 3) {
        return (face_rect_t){ 0, 0, 0, 0 };
    }
    float x0 = p->xy[0], x1 = p->xy[0], y0 = p->xy[1], y1 = p->xy[1];
    for (int i = 1; i < p->n; i++) {
        const float x = p->xy[i * 2], y = p->xy[i * 2 + 1];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    return (face_rect_t){
        (int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1) + 1, (int)ceilf(y1) + 1,
    };
}

face_rect_t face_shapes_bounds(const face_shapes_t *s)
{
    face_rect_t r = face_ellipse_bounds(&s->head);
    r = face_rect_union(r, face_poly_bounds(&s->eye[0]));
    r = face_rect_union(r, face_poly_bounds(&s->eye[1]));
    return r;
}
