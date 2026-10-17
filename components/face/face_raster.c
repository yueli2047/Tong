/*
 * Raster layer: shapes -> RGB565 pixels.
 *
 * Each screen row is composed in a line buffer in internal RAM and then
 * copied to the framebuffer (PSRAM) once, so PSRAM is written exactly once
 * per pixel, sequentially.
 *
 * Anti-aliasing: every pixel row is sampled on SUBROWS horizontal lines; on
 * each line the shape's spans are exact, and a pixel's coverage is its
 * horizontal overlap with those spans, averaged over the lines.
 *   - head: pixels covered on every line are filled directly; only the
 *     edge pixels compute coverage
 *   - eyes: even-odd polygon with an active edge list, so each line only
 *     looks at the two or three edges that actually cross it
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "face_priv.h"

#define SUBROWS     8
#define SUB_W       (1.0f / SUBROWS)
#define MAX_CROSS   16

static uint16_t s_line[FACE_SCREEN_W] __attribute__((aligned(4)));
/* coverage accumulation for the eyes; +2 so a span ending on the clip edge stays in range */
static float s_cov[FACE_SCREEN_W + 2];
static float s_run[FACE_SCREEN_W + 2];

uint16_t face_rgb565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t blend(uint16_t dst, uint16_t src, float alpha)
{
    const int a = (int)(alpha * 256.0f + 0.5f);
    const int dr = dst >> 11, dg = (dst >> 5) & 0x3f, db = dst & 0x1f;
    const int sr = src >> 11, sg = (src >> 5) & 0x3f, sb = src & 0x1f;
    const int r = dr + (((sr - dr) * a) >> 8);
    const int g = dg + (((sg - dg) * a) >> 8);
    const int b = db + (((sb - db) * a) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* src over dst with coverage a */
static inline uint16_t mix(uint16_t dst, uint16_t src, float a)
{
    if (a <= 0.002f) {
        return dst;
    }
    return a >= 0.998f ? src : blend(dst, src, a > 1.0f ? 1.0f : a);
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* line must be 4-byte aligned; writes two pixels per store */
typedef uint32_t __attribute__((may_alias)) px2_t;

static inline void fill_span(uint16_t *line, int x0, int x1, uint16_t color)
{
    if (x1 <= x0) {
        return;
    }
    int x = x0;
    if (x & 1) {
        line[x++] = color;
    }
    const uint32_t c2 = (uint32_t)color | ((uint32_t)color << 16);
    px2_t *w = (px2_t *)&line[x];
    for (; x + 1 < x1; x += 2) {
        *w++ = c2;
    }
    if (x < x1) {
        line[x] = color;
    }
}

void face_raster_fill(uint16_t *fb, int stride, face_rect_t clip, uint16_t color)
{
    clip = face_rect_clip_screen(clip);
    if (face_rect_empty(clip)) {
        return;
    }
    fill_span(s_line, clip.x0, clip.x1, color);
    const size_t bytes = (size_t)(clip.x1 - clip.x0) * sizeof(uint16_t);
    for (int y = clip.y0; y < clip.y1; y++) {
        memcpy(fb + (size_t)y * (size_t)stride + clip.x0, &s_line[clip.x0], bytes);
    }
}

/* ---------- head: ellipse ---------- */

typedef struct {
    bool valid;
    float cx, cy;
    float qa, qb, qc, inv2a;    /* (x-cx) on a line at dy: qa*x^2 + qb*dy*x + qc*dy^2 - 1 = 0 */
    int y0, y1;
} ellipse_rows_t;

static void ellipse_prepare(const face_ellipse_t *e, ellipse_rows_t *r)
{
    r->valid = e->valid && e->a > 0.0f && e->b > 0.0f;
    if (!r->valid) {
        return;
    }
    const float c = cosf(e->rot), s = sinf(e->rot);
    const float ia2 = 1.0f / (e->a * e->a), ib2 = 1.0f / (e->b * e->b);
    r->cx = e->cx;
    r->cy = e->cy;
    r->qa = c * c * ia2 + s * s * ib2;
    r->qb = 2.0f * c * s * (ia2 - ib2);
    r->qc = s * s * ia2 + c * c * ib2;
    r->inv2a = 0.5f / r->qa;
    const face_rect_t box = face_ellipse_bounds(e);
    r->y0 = box.y0;
    r->y1 = box.y1;
}

/* Coverage of pixel [x, x+1) by the row's sub-line spans. */
static inline float span_coverage(const float *l, const float *r, int n, int x)
{
    const float px0 = (float)x, px1 = (float)(x + 1);
    float cov = 0.0f;
    for (int k = 0; k < n; k++) {
        const float a = l[k] > px0 ? l[k] : px0;
        const float b = r[k] < px1 ? r[k] : px1;
        if (b > a) {
            cov += b - a;
        }
    }
    return cov * SUB_W;
}

/* Writes the whole row [x0, x1): background with the head on it. */
static void head_row(const ellipse_rows_t *e, int y, int x0, int x1, uint16_t *line,
                     uint16_t bg, uint16_t body)
{
    if (!e->valid || y < e->y0 || y >= e->y1) {
        fill_span(line, x0, x1, bg);
        return;
    }
    float l[SUBROWS], r[SUBROWS];
    int n = 0;
    float min_l = 1e9f, max_l = -1e9f, min_r = 1e9f, max_r = -1e9f;
    for (int k = 0; k < SUBROWS; k++) {
        const float dy = (float)y + ((float)k + 0.5f) * SUB_W - e->cy;
        const float b = e->qb * dy;
        const float disc = b * b - 4.0f * e->qa * (e->qc * dy * dy - 1.0f);
        if (disc <= 0.0f) {
            continue;
        }
        const float root = sqrtf(disc);
        l[n] = e->cx + (-b - root) * e->inv2a;
        r[n] = e->cx + (-b + root) * e->inv2a;
        if (l[n] < min_l) min_l = l[n];
        if (l[n] > max_l) max_l = l[n];
        if (r[n] < min_r) min_r = r[n];
        if (r[n] > max_r) max_r = r[n];
        n++;
    }
    if (n == 0) {
        fill_span(line, x0, x1, bg);
        return;
    }
    const int lo = clampi((int)floorf(min_l), x0, x1);
    const int hi = clampi((int)ceilf(max_r), x0, x1);
    /* pixels inside every sub-line's span are fully covered */
    int in0 = n == SUBROWS ? clampi((int)ceilf(max_l), lo, hi) : hi;
    int in1 = n == SUBROWS ? clampi((int)floorf(min_r), lo, hi) : hi;
    if (in1 <= in0) {
        in0 = in1 = hi;         /* no solid interior: everything is edge */
    }
    fill_span(line, x0, lo, bg);
    for (int x = lo; x < in0; x++) {
        line[x] = mix(bg, body, span_coverage(l, r, n, x));
    }
    fill_span(line, in0, in1, body);
    for (int x = in1 > in0 ? in1 : hi; x < hi; x++) {
        line[x] = mix(bg, body, span_coverage(l, r, n, x));
    }
    fill_span(line, hi, x1, bg);
}

/* ---------- eyes: even-odd polygon ---------- */

typedef struct {
    float ymin, ymax, x, dxdy;  /* x at ymin */
} edge_t;

typedef struct {
    edge_t e[FACE_EYE_MAX_PTS];
    int n;                      /* edges, sorted by ymin */
    int next;                   /* first edge not yet active */
    int act[MAX_CROSS];
    int n_act;
    int y0, y1;
    float xmin, xmax;
} poly_rows_t;

static int edge_cmp(const void *a, const void *b)
{
    const float ya = ((const edge_t *)a)->ymin, yb = ((const edge_t *)b)->ymin;
    return (ya > yb) - (ya < yb);
}

static void poly_prepare(const face_poly_t *p, poly_rows_t *r)
{
    r->n = r->next = r->n_act = 0;
    r->y0 = r->y1 = 0;
    if (!p->visible || p->n < 3) {
        return;
    }
    float ymin = 1e9f, ymax = -1e9f;
    r->xmin = 1e9f;
    r->xmax = -1e9f;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        const float xa = p->xy[j * 2], ya = p->xy[j * 2 + 1];
        const float xb = p->xy[i * 2], yb = p->xy[i * 2 + 1];
        if (xb < r->xmin) r->xmin = xb;
        if (xb > r->xmax) r->xmax = xb;
        if (yb < ymin) ymin = yb;
        if (yb > ymax) ymax = yb;
        if (ya == yb) {
            continue;           /* horizontal edges never cross a sample line */
        }
        edge_t *e = &r->e[r->n++];
        if (ya < yb) {
            *e = (edge_t){ ya, yb, xa, (xb - xa) / (yb - ya) };
        } else {
            *e = (edge_t){ yb, ya, xb, (xa - xb) / (ya - yb) };
        }
    }
    qsort(r->e, (size_t)r->n, sizeof(edge_t), edge_cmp);
    r->y0 = (int)floorf(ymin);
    r->y1 = (int)ceilf(ymax) + 1;
}

/* Spans of one sample line at sy, which must not decrease between calls. */
static int poly_line(poly_rows_t *r, float sy, float *xs)
{
    /* an edge crosses the line when ymin <= sy < ymax */
    while (r->next < r->n && r->e[r->next].ymin <= sy) {
        if (r->n_act < MAX_CROSS) {
            r->act[r->n_act++] = r->next;
        }
        r->next++;
    }
    int cnt = 0;
    for (int i = 0; i < r->n_act;) {
        const edge_t *e = &r->e[r->act[i]];
        if (e->ymax <= sy) {
            r->act[i] = r->act[--r->n_act];   /* finished: drop it */
            continue;
        }
        const float x = e->x + (sy - e->ymin) * e->dxdy;
        int m = cnt++;          /* insertion sort, a handful of crossings */
        while (m > 0 && xs[m - 1] > x) {
            xs[m] = xs[m - 1];
            m--;
        }
        xs[m] = x;
        i++;
    }
    return cnt;
}

static void cov_span(int x0, int x1, float xa, float xb)
{
    if (xa < (float)x0) xa = (float)x0;
    if (xb > (float)x1) xb = (float)x1;
    if (xb <= xa) {
        return;
    }
    const int ia = (int)xa, ib = (int)xb;   /* xa, xb >= 0: truncation is floor */
    if (ia == ib) {
        s_cov[ia] += (xb - xa) * SUB_W;
        return;
    }
    s_cov[ia] += ((float)(ia + 1) - xa) * SUB_W;
    s_cov[ib] += (xb - (float)ib) * SUB_W;
    s_run[ia + 1] += SUB_W;     /* full pixels ia+1 .. ib-1 */
    s_run[ib] -= SUB_W;
}

static void poly_row(poly_rows_t *r, int y, int x0, int x1, uint16_t *line, uint16_t color)
{
    if (r->n == 0 || y < r->y0 || y >= r->y1) {
        return;
    }
    const int lo = clampi((int)floorf(r->xmin), x0, x1);
    const int hi = clampi((int)ceilf(r->xmax) + 1, x0, x1);
    if (hi <= lo) {
        return;
    }
    memset(&s_cov[lo], 0, (size_t)(hi - lo + 2) * sizeof(float));
    memset(&s_run[lo], 0, (size_t)(hi - lo + 2) * sizeof(float));
    bool touched = false;
    for (int k = 0; k < SUBROWS; k++) {
        float xs[MAX_CROSS];
        const int cnt = poly_line(r, (float)y + ((float)k + 0.5f) * SUB_W, xs);
        for (int m = 0; m + 1 < cnt; m += 2) {
            cov_span(lo, hi, xs[m], xs[m + 1]);
            touched = true;
        }
    }
    if (!touched) {
        return;
    }
    float run = 0.0f;
    for (int x = lo; x < hi; x++) {
        run += s_run[x];
        line[x] = mix(line[x], color, s_cov[x] + run);
    }
}

/* ---------- frame ---------- */

/* Large per-frame state stays off the caller's stack */
static ellipse_rows_t s_head;
static poly_rows_t s_eye[2];

void face_raster_shapes(uint16_t *fb, int stride, face_rect_t clip, const face_shapes_t *s,
                        uint16_t bg, uint16_t body, uint16_t eyes)
{
    clip = face_rect_clip_screen(clip);
    if (face_rect_empty(clip)) {
        return;
    }
    ellipse_prepare(&s->head, &s_head);
    poly_prepare(&s->eye[0], &s_eye[0]);
    poly_prepare(&s->eye[1], &s_eye[1]);

    const size_t bytes = (size_t)(clip.x1 - clip.x0) * sizeof(uint16_t);
    for (int y = clip.y0; y < clip.y1; y++) {
        head_row(&s_head, y, clip.x0, clip.x1, s_line, bg, body);
        poly_row(&s_eye[0], y, clip.x0, clip.x1, s_line, eyes);
        poly_row(&s_eye[1], y, clip.x0, clip.x1, s_line, eyes);
        memcpy(fb + (size_t)y * (size_t)stride + clip.x0, &s_line[clip.x0], bytes);
    }
}
