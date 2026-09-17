/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Canvas - surfaces, compositing, effects and animation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <string.h>
#include "canvas.h"

/* ------------------------------------------------------------------ */
/* Small integer helpers                                               */
/* ------------------------------------------------------------------ */

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static unsigned isqrt32(unsigned n)
{
    unsigned res = 0;
    unsigned bit = 1u << 30;

    while (bit > n)
        bit >>= 2;

    while (bit)
    {
        if (n >= res + bit)
        {
            n -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */

bool canvas_rect_empty(const struct canvas_rect *r)
{
    return r->w <= 0 || r->h <= 0;
}

long canvas_rect_area(const struct canvas_rect *r)
{
    if (canvas_rect_empty(r))
        return 0;
    return (long)r->w * r->h;
}

bool canvas_rect_intersect(struct canvas_rect *a, const struct canvas_rect *b)
{
    int x1 = a->x > b->x ? a->x : b->x;
    int y1 = a->y > b->y ? a->y : b->y;
    int x2 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y2 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

    if (x2 <= x1 || y2 <= y1)
    {
        a->w = a->h = 0;
        return false;
    }

    a->x = x1; a->y = y1; a->w = x2 - x1; a->h = y2 - y1;
    return true;
}

void canvas_rect_union(struct canvas_rect *a, const struct canvas_rect *b)
{
    int x1, y1, x2, y2;

    if (canvas_rect_empty(b))
        return;
    if (canvas_rect_empty(a))
    {
        *a = *b;
        return;
    }

    x1 = a->x < b->x ? a->x : b->x;
    y1 = a->y < b->y ? a->y : b->y;
    x2 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    y2 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

    a->x = x1; a->y = y1; a->w = x2 - x1; a->h = y2 - y1;
}

bool canvas_rect_contains(const struct canvas_rect *a, const struct canvas_rect *b)
{
    if (canvas_rect_empty(b))
        return true;
    if (canvas_rect_empty(a))
        return false;
    return b->x >= a->x && b->y >= a->y &&
           b->x + b->w <= a->x + a->w &&
           b->y + b->h <= a->y + a->h;
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

void canvas_surface_init(struct canvas_surface *s, canvas_px *px,
                         uint8_t *alpha, int w, int h, int stride)
{
    s->px = px;
    s->alpha = alpha;
    s->w = w;
    s->h = h;
    s->stride = stride > 0 ? stride : w;
}

size_t canvas_surface_bytes(int w, int h, bool with_alpha)
{
    size_t px = (size_t)w * h;
    return px * sizeof(canvas_px) + (with_alpha ? px : 0);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

canvas_px canvas_blend_px(canvas_px dst, canvas_px src, unsigned a)
{
    uint32_t s, d, p;
    unsigned a5;

    if (a >= 255)
        return src;
    if (a == 0)
        return dst;

    /* 5-bit alpha keeps the packed multiply inside 32 bits */
    a5 = (a + 4) >> 3;              /* 0..32 */
    s = ((uint32_t)src | ((uint32_t)src << 16)) & 0x07e0f81fu;
    d = ((uint32_t)dst | ((uint32_t)dst << 16)) & 0x07e0f81fu;
    p = ((s * a5) + (d * (32 - a5))) >> 5;
    p &= 0x07e0f81fu;

    return (canvas_px)(p | (p >> 16));
}

/* Clip a rectangle against a surface. */
static bool clip_to_surface(const struct canvas_surface *s,
                            struct canvas_rect *r)
{
    struct canvas_rect bound = { 0, 0, s->w, s->h };
    return canvas_rect_intersect(r, &bound);
}

void canvas_fill(struct canvas_surface *s, const struct canvas_rect *r,
                 canvas_px colour)
{
    struct canvas_rect c = *r;
    int y, x;

    if (!clip_to_surface(s, &c))
        return;

    for (y = 0; y < c.h; y++)
    {
        canvas_px *row = canvas_at(s, c.x, c.y + y);
        for (x = 0; x < c.w; x++)
            row[x] = colour;
    }
}

void canvas_fill_a(struct canvas_surface *s, const struct canvas_rect *r,
                   canvas_px colour, uint8_t alpha)
{
    struct canvas_rect c = *r;
    int y;

    canvas_fill(s, r, colour);

    if (!s->alpha || !clip_to_surface(s, &c))
        return;

    for (y = 0; y < c.h; y++)
        memset(canvas_alpha_at(s, c.x, c.y + y), alpha, c.w);
}

/* Clip a source rectangle and a destination position against both
 * surfaces, adjusting all three together. */
static bool clip_blit(const struct canvas_surface *dst, int *dx, int *dy,
                      const struct canvas_surface *src,
                      struct canvas_rect *sr)
{
    struct canvas_rect sbound = { 0, 0, src->w, src->h };

    if (!canvas_rect_intersect(sr, &sbound))
        return false;

    if (*dx < 0) { sr->x -= *dx; sr->w += *dx; *dx = 0; }
    if (*dy < 0) { sr->y -= *dy; sr->h += *dy; *dy = 0; }
    if (*dx + sr->w > dst->w) sr->w = dst->w - *dx;
    if (*dy + sr->h > dst->h) sr->h = dst->h - *dy;

    return sr->w > 0 && sr->h > 0;
}

void canvas_blit(struct canvas_surface *dst, int dx, int dy,
                 const struct canvas_surface *src,
                 const struct canvas_rect *srect)
{
    struct canvas_rect s = *srect;
    int y;

    if (!clip_blit(dst, &dx, &dy, src, &s))
        return;

    for (y = 0; y < s.h; y++)
    {
        memcpy(canvas_at(dst, dx, dy + y),
               canvas_at(src, s.x, s.y + y),
               (size_t)s.w * sizeof(canvas_px));

        if (dst->alpha)
        {
            if (src->alpha)
                memcpy(canvas_alpha_at(dst, dx, dy + y),
                       canvas_alpha_at(src, s.x, s.y + y), s.w);
            else
                memset(canvas_alpha_at(dst, dx, dy + y), 0xff, s.w);
        }
    }
}

void canvas_blend(struct canvas_surface *dst, int dx, int dy,
                  const struct canvas_surface *src,
                  const struct canvas_rect *srect, unsigned opacity)
{
    struct canvas_rect s = *srect;
    int x, y;

    if (opacity == 0)
        return;

    if (!clip_blit(dst, &dx, &dy, src, &s))
        return;

    /* nothing to blend against: a straight copy is the same picture */
    if (!src->alpha && opacity >= 255)
    {
        canvas_blit(dst, dx, dy, src, &s);
        return;
    }

    for (y = 0; y < s.h; y++)
    {
        canvas_px *d = canvas_at(dst, dx, dy + y);
        const canvas_px *sp = canvas_at(src, s.x, s.y + y);
        const uint8_t *sa = src->alpha ?
            canvas_alpha_at(src, s.x, s.y + y) : NULL;
        uint8_t *da = dst->alpha ? canvas_alpha_at(dst, dx, dy + y) : NULL;

        for (x = 0; x < s.w; x++)
        {
            unsigned a = sa ? sa[x] : 255u;

            if (opacity < 255)
                a = (a * opacity + 127) / 255;

            if (a == 0)
                continue;

            d[x] = canvas_blend_px(d[x], sp[x], a);

            if (da)
            {
                unsigned out = a + (da[x] * (255 - a) + 127) / 255;
                da[x] = (uint8_t)(out > 255 ? 255 : out);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* The compositor                                                      */
/* ------------------------------------------------------------------ */

void canvas_init(struct canvas *c, struct canvas_surface *target)
{
    memset(c, 0, sizeof(*c));
    c->target = target;
}

static struct canvas_rect layer_rect(const struct canvas_layer *l)
{
    struct canvas_rect r = { l->x, l->y, l->surf->w, l->surf->h };
    return r;
}

int canvas_add_layer(struct canvas *c, struct canvas_surface *surf,
                     int x, int y, enum canvas_blend_mode blend)
{
    struct canvas_layer *l;
    struct canvas_rect r;

    if (c->nlayers >= CANVAS_MAX_LAYERS || !surf)
        return -1;

    l = &c->layers[c->nlayers];
    l->surf = surf;
    l->x = x;
    l->y = y;
    l->opacity = 255;
    l->blend = (uint8_t)blend;
    l->visible = true;
    l->painted.x = l->painted.y = l->painted.w = l->painted.h = 0;

    r = layer_rect(l);
    canvas_damage(c, &r);

    return c->nlayers++;
}

static struct canvas_layer *layer_of(struct canvas *c, int idx)
{
    if (idx < 0 || idx >= c->nlayers)
        return NULL;
    return &c->layers[idx];
}

void canvas_layer_move(struct canvas *c, int idx, int x, int y)
{
    struct canvas_layer *l = layer_of(c, idx);
    struct canvas_rect old, now;

    if (!l || (l->x == x && l->y == y))
        return;

    /* Where it was and where it is going, both. This is the entire reason
     * a compositor cannot smear: the vacated pixels are damage by
     * definition, not something the mover has to remember to repaint. */
    old = layer_rect(l);
    l->x = x;
    l->y = y;
    now = layer_rect(l);

    canvas_damage(c, &old);
    canvas_damage(c, &now);
}

void canvas_layer_set_opacity(struct canvas *c, int idx, unsigned opacity)
{
    struct canvas_layer *l = layer_of(c, idx);

    if (!l || l->opacity == (uint8_t)opacity)
        return;

    l->opacity = (uint8_t)(opacity > 255 ? 255 : opacity);
    canvas_layer_damage_all(c, idx);
}

void canvas_layer_set_visible(struct canvas *c, int idx, bool visible)
{
    struct canvas_layer *l = layer_of(c, idx);

    if (!l || l->visible == visible)
        return;

    l->visible = visible;
    canvas_layer_damage_all(c, idx);
}

void canvas_layer_damage(struct canvas *c, int idx, const struct canvas_rect *r)
{
    struct canvas_layer *l = layer_of(c, idx);
    struct canvas_rect d;

    if (!l)
        return;

    d = *r;
    d.x += l->x;
    d.y += l->y;
    canvas_damage(c, &d);
}

void canvas_layer_damage_all(struct canvas *c, int idx)
{
    struct canvas_layer *l = layer_of(c, idx);
    struct canvas_rect r;

    if (!l)
        return;

    r = layer_rect(l);
    canvas_damage(c, &r);
    /* a layer that went invisible still owes where it used to be */
    canvas_damage(c, &l->painted);
}

void canvas_damage(struct canvas *c, const struct canvas_rect *r)
{
    struct canvas_rect d = *r;

    if (!clip_to_surface(c->target, &d))
        return;

    canvas_rect_union(&c->damage, &d);
}

void canvas_damage_all(struct canvas *c)
{
    struct canvas_rect r = { 0, 0, c->target->w, c->target->h };
    canvas_damage(c, &r);
}

bool canvas_has_damage(const struct canvas *c)
{
    return !canvas_rect_empty(&c->damage);
}

struct canvas_rect canvas_composite(struct canvas *c)
{
    struct canvas_rect d = c->damage;
    struct canvas_rect none = { 0, 0, 0, 0 };
    int i;

    if (canvas_rect_empty(&d))
        return none;

    for (i = 0; i < c->nlayers; i++)
    {
        struct canvas_layer *l = &c->layers[i];
        struct canvas_rect hit;
        struct canvas_rect src;

        if (!l->visible || l->opacity == 0)
        {
            l->painted = none;
            continue;
        }

        hit = layer_rect(l);
        l->painted = hit;

        if (!canvas_rect_intersect(&hit, &d))
            continue;

        /* the part of the layer's own surface that lands in the damage */
        src.x = hit.x - l->x;
        src.y = hit.y - l->y;
        src.w = hit.w;
        src.h = hit.h;

        if (l->blend == CANVAS_COPY && l->opacity == 255)
            canvas_blit(c->target, hit.x, hit.y, l->surf, &src);
        else
            canvas_blend(c->target, hit.x, hit.y, l->surf, &src, l->opacity);

        c->px_touched += canvas_rect_area(&hit);
    }

    c->composites++;
    c->damage = none;
    return d;
}

/* ------------------------------------------------------------------ */
/* Blur                                                                */
/* ------------------------------------------------------------------ */

size_t canvas_blur_scratch_px(int w, int h)
{
    int dw = (w + CANVAS_BLUR_FACTOR - 1) >> CANVAS_BLUR_SHIFT;
    int dh = (h + CANVAS_BLUR_FACTOR - 1) >> CANVAS_BLUR_SHIFT;

    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    return (size_t)dw * dh * 2;
}

/* One separable box pass over `lines` lines of `len` elements, where
 * element (l, i) sits at [l*lstep + i*step]. Edges clamp rather than
 * darken, so a blurred panel does not get a dark rim. */
static void box_blur_axis(const canvas_px *in, canvas_px *out,
                          int lines, int len, int step, int lstep,
                          int radius)
{
    int l, i;
    int win;

    if (radius < 1)
        radius = 1;
    if (radius > len)
        radius = len;
    win = 2 * radius + 1;

    for (l = 0; l < lines; l++)
    {
        const canvas_px *ip = in + (size_t)l * lstep;
        canvas_px *op = out + (size_t)l * lstep;
        int sr = 0, sg = 0, sb = 0;
        int k;

        for (k = -radius; k <= radius; k++)
        {
            canvas_px p = ip[(size_t)clampi(k, 0, len - 1) * step];
            sr += CANVAS_R(p); sg += CANVAS_G(p); sb += CANVAS_B(p);
        }

        for (i = 0; i < len; i++)
        {
            canvas_px add, rem;

            op[(size_t)i * step] =
                CANVAS_RGB(sr / win, sg / win, sb / win);

            add = ip[(size_t)clampi(i + radius + 1, 0, len - 1) * step];
            rem = ip[(size_t)clampi(i - radius, 0, len - 1) * step];

            sr += CANVAS_R(add) - CANVAS_R(rem);
            sg += CANVAS_G(add) - CANVAS_G(rem);
            sb += CANVAS_B(add) - CANVAS_B(rem);
        }
    }
}

bool canvas_blur(struct canvas_surface *s, const struct canvas_rect *r,
                 int radius, canvas_px *scratch, size_t scratch_px)
{
    struct canvas_rect c = *r;
    canvas_px *small, *tmp;
    int dw, dh, x, y, small_radius;

    if (!scratch || !clip_to_surface(s, &c))
        return false;

    dw = (c.w + CANVAS_BLUR_FACTOR - 1) >> CANVAS_BLUR_SHIFT;
    dh = (c.h + CANVAS_BLUR_FACTOR - 1) >> CANVAS_BLUR_SHIFT;
    if (dw < 2 || dh < 2)
        return false;

    if (scratch_px < (size_t)dw * dh * 2)
        return false;

    small = scratch;
    tmp = scratch + (size_t)dw * dh;

    /* reduce: box-average each CANVAS_BLUR_FACTOR square */
    for (y = 0; y < dh; y++)
    {
        for (x = 0; x < dw; x++)
        {
            int sr = 0, sg = 0, sb = 0, n = 0;
            int by, bx;

            for (by = 0; by < CANVAS_BLUR_FACTOR; by++)
            {
                int sy = (y << CANVAS_BLUR_SHIFT) + by;
                if (sy >= c.h)
                    break;
                for (bx = 0; bx < CANVAS_BLUR_FACTOR; bx++)
                {
                    canvas_px p;
                    int sx = (x << CANVAS_BLUR_SHIFT) + bx;
                    if (sx >= c.w)
                        break;
                    p = *canvas_at(s, c.x + sx, c.y + sy);
                    sr += CANVAS_R(p); sg += CANVAS_G(p); sb += CANVAS_B(p);
                    n++;
                }
            }

            small[(size_t)y * dw + x] = CANVAS_RGB(sr / n, sg / n, sb / n);
        }
    }

    small_radius = radius >> CANVAS_BLUR_SHIFT;
    if (small_radius < 1)
        small_radius = 1;

    box_blur_axis(small, tmp, dh, dw, 1, dw, small_radius);   /* horizontal */
    box_blur_axis(tmp, small, dw, dh, dw, 1, small_radius);   /* vertical */

    /* expand: bilinear, so the low-resolution grid does not show */
    for (y = 0; y < c.h; y++)
    {
        int fy = ((y << 8) >> CANVAS_BLUR_SHIFT) - 128;
        int y0, y1, wy;

        if (fy < 0) fy = 0;
        y0 = fy >> 8;
        if (y0 > dh - 1) y0 = dh - 1;
        y1 = y0 + 1 < dh ? y0 + 1 : dh - 1;
        wy = fy & 0xff;

        for (x = 0; x < c.w; x++)
        {
            int fx = ((x << 8) >> CANVAS_BLUR_SHIFT) - 128;
            int x0, x1, wx;
            canvas_px p00, p01, p10, p11;
            int top, bot, red, grn, blu;

            if (fx < 0) fx = 0;
            x0 = fx >> 8;
            if (x0 > dw - 1) x0 = dw - 1;
            x1 = x0 + 1 < dw ? x0 + 1 : dw - 1;
            wx = fx & 0xff;

            p00 = small[(size_t)y0 * dw + x0];
            p01 = small[(size_t)y0 * dw + x1];
            p10 = small[(size_t)y1 * dw + x0];
            p11 = small[(size_t)y1 * dw + x1];

            top = CANVAS_R(p00) * (256 - wx) + CANVAS_R(p01) * wx;
            bot = CANVAS_R(p10) * (256 - wx) + CANVAS_R(p11) * wx;
            red = (top * (256 - wy) + bot * wy) >> 16;

            top = CANVAS_G(p00) * (256 - wx) + CANVAS_G(p01) * wx;
            bot = CANVAS_G(p10) * (256 - wx) + CANVAS_G(p11) * wx;
            grn = (top * (256 - wy) + bot * wy) >> 16;

            top = CANVAS_B(p00) * (256 - wx) + CANVAS_B(p01) * wx;
            bot = CANVAS_B(p10) * (256 - wx) + CANVAS_B(p11) * wx;
            blu = (top * (256 - wy) + bot * wy) >> 16;

            *canvas_at(s, c.x + x, c.y + y) = CANVAS_RGB(red, grn, blu);
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Rounded rectangles, shadows                                         */
/* ------------------------------------------------------------------ */

/* Signed distance from the pixel centre to the boundary of a rounded
 * rectangle, in sixteenths of a pixel. Negative inside. One expression for
 * the whole shape, corners included - which is what makes the curve
 * antialiased rather than stepped, and what lets the shadow fall off
 * without ever allocating a second buffer to blur. */
static int rounded_sdf_q4(const struct canvas_rect *r, int radius,
                          int px, int py)
{
    int x1 = r->x << 4, x2 = (r->x + r->w) << 4;
    int y1 = r->y << 4, y2 = (r->y + r->h) << 4;
    int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
    int hw = (x2 - x1) / 2, hh = (y2 - y1) / 2;
    int rad = radius << 4;
    int qx, qy, mx, my, outside, inside;

    if (rad > hw) rad = hw;
    if (rad > hh) rad = hh;
    if (rad < 0) rad = 0;

    qx = (px << 4) + 8 - cx;
    if (qx < 0) qx = -qx;
    qx -= hw - rad;

    qy = (py << 4) + 8 - cy;
    if (qy < 0) qy = -qy;
    qy -= hh - rad;

    mx = qx > 0 ? qx : 0;
    my = qy > 0 ? qy : 0;

    outside = (int)isqrt32((unsigned)(mx * mx + my * my));
    inside = (qx > qy ? qx : qy);
    if (inside > 0)
        inside = 0;

    return outside + inside - rad;
}

/* Coverage 0..255, feathered over the one pixel straddling the boundary. */
static unsigned rounded_coverage(const struct canvas_rect *r, int radius,
                                 int px, int py)
{
    int d = rounded_sdf_q4(r, radius, px, py);

    if (d <= -8)
        return 255;
    if (d >= 8)
        return 0;

    return (unsigned)(((8 - d) * 255) / 16);
}

void canvas_round_rect(struct canvas_surface *s, const struct canvas_rect *r,
                       int radius, uint8_t inside, uint8_t outside)
{
    struct canvas_rect c = { 0, 0, s->w, s->h };
    int x, y;

    if (!s->alpha)
        return;

    for (y = c.y; y < c.y + c.h; y++)
    {
        uint8_t *a = canvas_alpha_at(s, 0, y);

        for (x = c.x; x < c.x + c.w; x++)
        {
            unsigned cov = rounded_coverage(r, radius, x, y);
            a[x] = (uint8_t)(outside + ((inside - outside) * (int)cov) / 255);
        }
    }
}

/* Walk the pixels a rounded rect can possibly touch, given how far outside
 * its boundary the caller cares about. */
#define ROUND_AREA(dst, r, pad, ...) do {                                  \
    struct canvas_rect area_;                                              \
    int px_, py_;                                                          \
    area_.x = (r)->x - (pad); area_.y = (r)->y - (pad);                    \
    area_.w = (r)->w + 2 * (pad); area_.h = (r)->h + 2 * (pad);            \
    if (clip_to_surface((dst), &area_))                                    \
        for (py_ = area_.y; py_ < area_.y + area_.h; py_++)                \
            for (px_ = area_.x; px_ < area_.x + area_.w; px_++)            \
                __VA_ARGS__                                                 \
} while (0)

void canvas_fill_round_rect(struct canvas_surface *dst,
                            const struct canvas_rect *r, int corner,
                            canvas_px colour, unsigned opacity)
{
    ROUND_AREA(dst, r, 1, {
        unsigned cov = rounded_coverage(r, corner, px_, py_);
        unsigned a;

        if (!cov)
            continue;

        a = opacity >= 255 ? cov : (cov * opacity + 127) / 255;
        if (!a)
            continue;

        *canvas_at(dst, px_, py_) =
            canvas_blend_px(*canvas_at(dst, px_, py_), colour, a);

        if (dst->alpha)
        {
            uint8_t *da = canvas_alpha_at(dst, px_, py_);
            unsigned out = a + (*da * (255 - a) + 127) / 255;
            *da = (uint8_t)(out > 255 ? 255 : out);
        }
    });
}

void canvas_stroke_round_rect(struct canvas_surface *dst,
                              const struct canvas_rect *r, int corner,
                              int thickness, canvas_px colour,
                              unsigned opacity)
{
    int half;

    if (thickness < 1)
        thickness = 1;
    half = (thickness << 4) / 2;

    ROUND_AREA(dst, r, thickness + 1, {
        int d = rounded_sdf_q4(r, corner, px_, py_);
        int off;
        unsigned cov, a;

        if (d < 0)
            d = -d;
        off = d - half;

        if (off >= 8)
            continue;
        cov = off <= -8 ? 255u : (unsigned)(((8 - off) * 255) / 16);

        a = opacity >= 255 ? cov : (cov * opacity + 127) / 255;
        if (!a)
            continue;

        *canvas_at(dst, px_, py_) =
            canvas_blend_px(*canvas_at(dst, px_, py_), colour, a);

        if (dst->alpha)
        {
            uint8_t *da = canvas_alpha_at(dst, px_, py_);
            unsigned out = a + (*da * (255 - a) + 127) / 255;
            *da = (uint8_t)(out > 255 ? 255 : out);
        }
    });
}

void canvas_shadow(struct canvas_surface *dst, const struct canvas_rect *r,
                   int corner, int radius, int dx, int dy,
                   canvas_px colour, unsigned opacity)
{
    struct canvas_rect shape = *r;
    struct canvas_rect area;
    int x, y;

    if (radius < 1)
        radius = 1;

    shape.x += dx;
    shape.y += dy;

    area.x = shape.x - radius;
    area.y = shape.y - radius;
    area.w = shape.w + 2 * radius;
    area.h = shape.h + 2 * radius;

    if (!clip_to_surface(dst, &area))
        return;

    for (y = area.y; y < area.y + area.h; y++)
    {
        canvas_px *row = canvas_at(dst, 0, y);

        for (x = area.x; x < area.x + area.w; x++)
        {
            int d = rounded_sdf_q4(&shape, corner, x, y);
            unsigned cov, a;

            if (d <= 0)
            {
                cov = 255;
            }
            else if (d >= (radius << 4))
            {
                continue;
            }
            else
            {
                /* quadratic falloff over `radius` - reads as a shadow
                 * rather than as a halo with an edge to it */
                unsigned f = (unsigned)(((radius << 4) - d) * 255) / (unsigned)(radius << 4);
                cov = (f * f) / 255;
            }

            if (!cov)
                continue;

            a = (cov * opacity + 127) / 255;
            row[x] = canvas_blend_px(row[x], colour, a);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Gradients                                                           */
/* ------------------------------------------------------------------ */

static const uint8_t bayer4[16] =
{
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

void canvas_gradient(struct canvas_surface *s, const struct canvas_rect *r,
                     canvas_px from, canvas_px to, bool vertical, bool dither)
{
    struct canvas_rect c = *r;
    int x, y, span;

    if (!clip_to_surface(s, &c))
        return;

    span = (vertical ? c.h : c.w) - 1;
    if (span < 1)
        span = 1;

    for (y = 0; y < c.h; y++)
    {
        canvas_px *row = canvas_at(s, c.x, c.y + y);

        for (x = 0; x < c.w; x++)
        {
            int t = vertical ? y : x;
            int rr = CANVAS_R(from) + (CANVAS_R(to) - CANVAS_R(from)) * t / span;
            int gg = CANVAS_G(from) + (CANVAS_G(to) - CANVAS_G(from)) * t / span;
            int bb = CANVAS_B(from) + (CANVAS_B(to) - CANVAS_B(from)) * t / span;

            if (dither)
            {
                /* Ordered dither in the bits RGB565 is about to throw
                 * away: 3 for red and blue, 2 for green. */
                int d = bayer4[((y & 3) << 2) | (x & 3)];
                rr = clampi(rr + ((d * 8) >> 4) - 4, 0, 255);
                gg = clampi(gg + ((d * 4) >> 4) - 2, 0, 255);
                bb = clampi(bb + ((d * 8) >> 4) - 4, 0, 255);
            }

            row[x] = CANVAS_RGB(rr, gg, bb);
        }
    }
}

canvas_px canvas_average(const struct canvas_surface *s,
                         const struct canvas_rect *r)
{
    struct canvas_rect c = *r;
    unsigned long sr = 0, sg = 0, sb = 0, n = 0;
    int x, y, sx, sy;

    if (!clip_to_surface(s, &c))
        return 0;

    sx = c.w > 32 ? c.w / 32 : 1;
    sy = c.h > 32 ? c.h / 32 : 1;

    for (y = c.y + sy / 2; y < c.y + c.h; y += sy)
    {
        const canvas_px *row = canvas_at(s, 0, y);
        for (x = c.x + sx / 2; x < c.x + c.w; x += sx)
        {
            sr += CANVAS_R(row[x]);
            sg += CANVAS_G(row[x]);
            sb += CANVAS_B(row[x]);
            n++;
        }
    }
    if (!n)
        return 0;
    return CANVAS_RGB(sr / n, sg / n, sb / n);
}

void canvas_wash(struct canvas_surface *s, const struct canvas_rect *r,
                 canvas_px top, canvas_px bottom,
                 unsigned a_top, unsigned a_bottom, unsigned desat)
{
    struct canvas_rect c = *r;
    int x, y, span;

    if (a_top > 255) a_top = 255;
    if (a_bottom > 255) a_bottom = 255;
    if (desat > 255) desat = 255;

    if (!clip_to_surface(s, &c))
        return;

    /* Positions are measured in the unclipped rectangle, so a wash that
     * hangs off the surface still has its stops where it was asked to. */
    span = r->h - 1;
    if (span < 1)
        span = 1;

    for (y = 0; y < c.h; y++)
    {
        canvas_px *row = canvas_at(s, 0, c.y + y);
        int t = c.y + y - r->y;
        int cr = CANVAS_R(top) + (CANVAS_R(bottom) - CANVAS_R(top)) * t / span;
        int cg = CANVAS_G(top) + (CANVAS_G(bottom) - CANVAS_G(top)) * t / span;
        int cb = CANVAS_B(top) + (CANVAS_B(bottom) - CANVAS_B(top)) * t / span;
        int a  = (int)a_top + ((int)a_bottom - (int)a_top) * t / span;

        for (x = c.x; x < c.x + c.w; x++)
        {
            int pr = CANVAS_R(row[x]);
            int pg = CANVAS_G(row[x]);
            int pb = CANVAS_B(row[x]);
            int d = bayer4[((y & 3) << 2) | (x & 3)];

            if (desat)
            {
                /* Rec. 601 luma, weights out of 256 */
                int l = (pr * 77 + pg * 150 + pb * 29) >> 8;
                pr += (l - pr) * (int)desat / 255;
                pg += (l - pg) * (int)desat / 255;
                pb += (l - pb) * (int)desat / 255;
            }

            pr += (cr - pr) * a / 255;
            pg += (cg - pg) * a / 255;
            pb += (cb - pb) * a / 255;

            pr = clampi(pr + ((d * 8) >> 4) - 4, 0, 255);
            pg = clampi(pg + ((d * 4) >> 4) - 2, 0, 255);
            pb = clampi(pb + ((d * 8) >> 4) - 4, 0, 255);

            row[x] = CANVAS_RGB(pr, pg, pb);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Nine-slice                                                          */
/* ------------------------------------------------------------------ */

/* Nearest-neighbour scaled blit. Nine-slice only ever stretches edges,
 * which are one pixel of detail repeated, so nearest is exactly right and
 * bilinear would only blur them. */
static void blit_scaled(struct canvas_surface *dst, const struct canvas_rect *d,
                        const struct canvas_surface *src,
                        const struct canvas_rect *sr)
{
    struct canvas_rect c = *d;
    int x, y;

    if (canvas_rect_empty(d) || canvas_rect_empty(sr))
        return;
    if (!clip_to_surface(dst, &c))
        return;

    for (y = c.y; y < c.y + c.h; y++)
    {
        int sy = sr->y + (int)((long)(y - d->y) * sr->h / d->h);
        canvas_px *drow;

        if (sy < 0 || sy >= src->h)
            continue;
        drow = canvas_at(dst, 0, y);

        for (x = c.x; x < c.x + c.w; x++)
        {
            int sx = sr->x + (int)((long)(x - d->x) * sr->w / d->w);

            if (sx < 0 || sx >= src->w)
                continue;
            drow[x] = *canvas_at(src, sx, sy);
        }
    }
}

void canvas_9slice(struct canvas_surface *dst, const struct canvas_rect *drect,
                   const struct canvas_surface *src,
                   int left, int top, int right, int bottom)
{
    struct canvas_rect s, d;
    int mid_sw = src->w - left - right;
    int mid_sh = src->h - top - bottom;
    int mid_dw = drect->w - left - right;
    int mid_dh = drect->h - top - bottom;

    if (mid_sw < 0 || mid_sh < 0 || mid_dw < 0 || mid_dh < 0)
        return;

    /* corners, unscaled */
    s.x = 0; s.y = 0; s.w = left; s.h = top;
    canvas_blit(dst, drect->x, drect->y, src, &s);

    s.x = src->w - right; s.w = right;
    canvas_blit(dst, drect->x + drect->w - right, drect->y, src, &s);

    s.x = 0; s.y = src->h - bottom; s.w = left; s.h = bottom;
    canvas_blit(dst, drect->x, drect->y + drect->h - bottom, src, &s);

    s.x = src->w - right; s.w = right;
    canvas_blit(dst, drect->x + drect->w - right,
                drect->y + drect->h - bottom, src, &s);

    /* edges, stretched along one axis */
    s.x = left; s.y = 0; s.w = mid_sw; s.h = top;
    d.x = drect->x + left; d.y = drect->y; d.w = mid_dw; d.h = top;
    blit_scaled(dst, &d, src, &s);

    s.y = src->h - bottom; s.h = bottom;
    d.y = drect->y + drect->h - bottom; d.h = bottom;
    blit_scaled(dst, &d, src, &s);

    s.x = 0; s.y = top; s.w = left; s.h = mid_sh;
    d.x = drect->x; d.y = drect->y + top; d.w = left; d.h = mid_dh;
    blit_scaled(dst, &d, src, &s);

    s.x = src->w - right; s.w = right;
    d.x = drect->x + drect->w - right; d.w = right;
    blit_scaled(dst, &d, src, &s);

    /* centre, stretched both ways */
    s.x = left; s.y = top; s.w = mid_sw; s.h = mid_sh;
    d.x = drect->x + left; d.y = drect->y + top;
    d.w = mid_dw; d.h = mid_dh;
    blit_scaled(dst, &d, src, &s);
}

/* ------------------------------------------------------------------ */
/* Reflection                                                          */
/* ------------------------------------------------------------------ */

void canvas_reflect(struct canvas_surface *dst, int dx, int dy,
                    const struct canvas_surface *src,
                    const struct canvas_rect *srect,
                    int height, unsigned top, unsigned bottom)
{
    struct canvas_rect s = *srect;
    struct canvas_rect sbound = { 0, 0, src->w, src->h };
    int j, x;

    if (!canvas_rect_intersect(&s, &sbound))
        return;

    if (height > s.h)
        height = s.h;

    for (j = 0; j < height; j++)
    {
        int sy = s.y + s.h - 1 - j;
        int ty = dy + j;
        unsigned a = top + ((int)bottom - (int)top) * j / (height > 1 ? height - 1 : 1);
        canvas_px *d;
        const canvas_px *sp;

        if (ty < 0 || ty >= dst->h)
            continue;

        d = canvas_at(dst, 0, ty);
        sp = canvas_at(src, 0, sy);

        for (x = 0; x < s.w; x++)
        {
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w)
                continue;
            d[tx] = canvas_blend_px(d[tx], sp[s.x + x], a);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Vinyl                                                               */
/* ------------------------------------------------------------------ */

/* sin(0..90 deg) in Q14 */
static const int16_t sin_q14[91] =
{
        0,   286,   572,   857,  1143,  1428,  1713,  1997,  2280,  2563,
     2845,  3126,  3406,  3686,  3964,  4240,  4516,  4790,  5063,  5334,
     5604,  5872,  6138,  6402,  6664,  6924,  7182,  7438,  7692,  7943,
     8192,  8438,  8682,  8923,  9162,  9397,  9630,  9860, 10087, 10311,
    10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365,
    12551, 12733, 12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044,
    14189, 14330, 14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296,
    15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083,
    16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382,
    16384,
};

static int sin_deg_q14(int deg)
{
    deg %= 360;
    if (deg < 0)
        deg += 360;
    if (deg <= 90)  return  sin_q14[deg];
    if (deg <= 180) return  sin_q14[180 - deg];
    if (deg <= 270) return -sin_q14[deg - 180];
    return -sin_q14[360 - deg];
}

void canvas_vinyl(struct canvas_surface *dst, int cx, int cy, int radius,
                  int angle_deg, const struct canvas_surface *label,
                  int label_pct, canvas_px accent, canvas_px ground)
{
    const canvas_px groove   = CANVAS_RGB(22, 22, 25);
    const canvas_px groove_l = CANVAS_RGB(34, 34, 38);
    const canvas_px gap      = CANVAS_RGB(12, 12, 14);
    const canvas_px rim      = CANVAS_RGB(58, 58, 62);
    const canvas_px shine    = CANVAS_RGB(84, 84, 90);
    canvas_px stripe;
    int32_t r2, rim2, lab_r, lab2, hole2, ring2;
    int32_t gap_in[3], gap_out[3];
    int c, s, x, y, i;

    if (radius <= 0)
        return;
    if (label_pct < 0)
        label_pct = 0;
    if (label_pct > 90)
        label_pct = 90;

    c = sin_deg_q14(90 - angle_deg);
    s = sin_deg_q14(angle_deg);

    r2    = (int32_t)radius * radius;
    rim2  = (int32_t)(radius - 2) * (radius - 2);
    lab_r = radius * label_pct / 100;
    lab2  = (int32_t)lab_r * lab_r;
    ring2 = (int32_t)(lab_r + 3) * (lab_r + 3);
    hole2 = (int32_t)(radius / 40 + 2) * (radius / 40 + 2);

    /* The silent gaps between tracks: three thin dark rings. */
    for (i = 0; i < 3; i++)
    {
        int rr = lab_r + (radius - lab_r) * (i + 1) / 4;
        gap_in[i]  = (int32_t)(rr - 1) * (rr - 1);
        gap_out[i] = (int32_t)(rr + 1) * (rr + 1);
    }

    /* Half-way between the accent and black: the mark on a bare label. */
    stripe = CANVAS_RGB(CANVAS_R(accent) / 2, CANVAS_G(accent) / 2,
                        CANVAS_B(accent) / 2);

    for (y = -radius; y <= radius; y++)
    {
        int ty = cy + y;
        canvas_px *row;

        if (ty < 0 || ty >= dst->h)
            continue;
        row = canvas_at(dst, 0, ty);

        for (x = -radius; x <= radius; x++)
        {
            int tx = cx + x;
            int32_t d2 = (int32_t)x * x + (int32_t)y * y;
            canvas_px p;

            if (tx < 0 || tx >= dst->w || d2 > r2)
                continue;

            if (d2 <= hole2)
                p = ground;
            else if (d2 <= lab2)
            {
                /* Where this pixel was before the record turned. */
                int u = (c * x + s * y) >> 14;
                int v = (c * y - s * x) >> 14;

                if (label && label->w > 0 && label->h > 0)
                {
                    int sx = (u + lab_r) * label->w / (2 * lab_r + 1);
                    int sy = (v + lab_r) * label->h / (2 * lab_r + 1);
                    if (sx < 0) sx = 0;
                    if (sy < 0) sy = 0;
                    if (sx >= label->w) sx = label->w - 1;
                    if (sy >= label->h) sy = label->h - 1;
                    p = *canvas_at(label, sx, sy);
                }
                else if (u > 0 && v > -lab_r / 8 && v < lab_r / 8)
                    p = stripe;
                else
                    p = accent;
            }
            else if (d2 <= ring2)
                p = ground;
            else if (d2 > rim2)
                p = rim;
            else
            {
                p = ((d2 >> 7) % 3) ? groove : groove_l;
                for (i = 0; i < 3; i++)
                    if (d2 >= gap_in[i] && d2 <= gap_out[i])
                        p = gap;
                /* sin(2 theta) > 0.96: two narrow wedges of light, top
                 * left and bottom right, that do not turn with the
                 * record, brightest at their middle. */
                {
                    /* int32 is enough for a radius up to ~500 px */
                    int32_t k = 200 * x * y - 96 * d2;
                    if (k > 0)
                        p = canvas_blend_px(p, shine,
                                            (unsigned)(k * 40 / d2 + 40));
                }
            }
            row[tx] = p;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Text coverage                                                       */
/* ------------------------------------------------------------------ */

/* coverage -> perceptual coverage, sampled every 16 and interpolated.
 * Approximates blending in linear light, which RGB565 is not. */
static const uint8_t gamma_cov[17] =
{
      0,  38,  61,  81,  98, 115, 130, 147, 158,
    172, 185, 197, 210, 222, 233, 245, 255
};

static inline unsigned gamma_apply(unsigned c)
{
    unsigned i = c >> 4;
    unsigned f = c & 15;
    unsigned a = gamma_cov[i];
    unsigned b = gamma_cov[i + 1];

    return a + ((b - a) * f) / 16;
}

void canvas_blend_coverage(struct canvas_surface *dst, int dx, int dy,
                           const uint8_t *cov, int cov_stride,
                           int w, int h, canvas_px colour, bool gamma)
{
    int x, y;
    int sx0 = 0, sy0 = 0;

    if (dx < 0) { sx0 = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy0 = -dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0)
        return;

    for (y = 0; y < h; y++)
    {
        canvas_px *d = canvas_at(dst, dx, dy + y);
        const uint8_t *c = cov + (size_t)(sy0 + y) * cov_stride + sx0;
        uint8_t *da = dst->alpha ? canvas_alpha_at(dst, dx, dy + y) : NULL;

        for (x = 0; x < w; x++)
        {
            unsigned a = c[x];

            if (!a)
                continue;
            if (gamma)
                a = gamma_apply(a);

            d[x] = canvas_blend_px(d[x], colour, a);

            if (da)
            {
                unsigned out = a + (da[x] * (255 - a) + 127) / 255;
                da[x] = (uint8_t)(out > 255 ? 255 : out);
            }
        }
    }
}

void canvas_expand_font4(uint8_t *cov, int cov_stride,
                         const uint8_t *src, int src_stride_px,
                         int w, int h)
{
    int x, y;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            size_t p = (size_t)y * src_stride_px + x;
            unsigned v = src[p >> 1];

            v = (p & 1) ? (v >> 4) : (v & 0x0f);
            /* font alpha counts down from opaque; coverage counts up */
            cov[(size_t)y * cov_stride + x] = (uint8_t)((15 - v) * 17);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Animation                                                           */
/* ------------------------------------------------------------------ */

int canvas_ease_q8(enum canvas_ease ease, int t_q8)
{
    int inv;

    t_q8 = clampi(t_q8, 0, 256);
    inv = 256 - t_q8;

    switch (ease)
    {
    case CANVAS_EASE_OUT_QUAD:
        return 256 - (inv * inv) / 256;

    case CANVAS_EASE_IN_OUT_QUAD:
        if (t_q8 < 128)
            return (2 * t_q8 * t_q8) / 256;
        return 256 - (2 * inv * inv) / 256;

    case CANVAS_EASE_OUT_CUBIC:
        return 256 - (inv * inv / 256) * inv / 256;

    case CANVAS_EASE_IN_OUT_CUBIC:
        if (t_q8 < 128)
            return (4 * t_q8 / 256) * t_q8 / 256 * t_q8;
        return 256 - (4 * inv / 256) * inv / 256 * inv;

    case CANVAS_EASE_LINEAR:
    default:
        return t_q8;
    }
}

void canvas_anim_start(struct canvas_anim *a, int32_t from, int32_t to,
                       unsigned dur_ms, enum canvas_ease ease, unsigned now_ms)
{
    a->from = from;
    a->to = to;
    a->start_ms = now_ms;
    a->dur_ms = dur_ms ? dur_ms : 1;
    a->ease = (uint8_t)ease;
    a->active = true;
}

void canvas_anim_stop(struct canvas_anim *a)
{
    a->active = false;
}

static int anim_progress_q8(const struct canvas_anim *a, unsigned now_ms)
{
    unsigned elapsed;

    if (now_ms <= a->start_ms)
        return 0;

    elapsed = now_ms - a->start_ms;
    if (elapsed >= a->dur_ms)
        return 256;

    return (int)((elapsed * 256u) / a->dur_ms);
}

bool canvas_anim_done(const struct canvas_anim *a, unsigned now_ms)
{
    return !a->active || anim_progress_q8(a, now_ms) >= 256;
}

int32_t canvas_anim_value(const struct canvas_anim *a, unsigned now_ms)
{
    int t;

    if (!a->active)
        return a->to;

    t = anim_progress_q8(a, now_ms);
    if (t >= 256)
        return a->to;

    t = canvas_ease_q8((enum canvas_ease)a->ease, t);

    return a->from + (int32_t)(((int64_t)(a->to - a->from) * t) >> 8);
}
