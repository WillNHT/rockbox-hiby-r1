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

/* Layer 1 of the Canvas work: the grammar.
 *
 * Pure C. Integer arithmetic. No allocation, no timer, no Rockbox header.
 * Compiles into the firmware AND into the host test harness, exactly like
 * apps/stick.c does. Every pixel decision lives here so it can be tested in
 * ten seconds on a laptop instead of ten minutes on a device.
 *
 * Pixels are RGB565, because the panel is and because 32bpp doubles every
 * bandwidth figure to buy colour depth the panel probably cannot show.
 * Alpha is a separate 8-bit plane where a surface has one, and runs
 * 0 = transparent .. 255 = opaque. Note that this is the opposite way round
 * from Rockbox's own 4-bit font alpha, which counts down from opaque.
 */

#ifndef _CANVAS_H
#define _CANVAS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint16_t canvas_px;     /* RGB565, host byte order */

#define CANVAS_RGB(r, g, b) \
    ((canvas_px)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

#define CANVAS_R(c) (((c) >> 8) & 0xf8)
#define CANVAS_G(c) (((c) >> 3) & 0xfc)
#define CANVAS_B(c) (((c) << 3) & 0xf8)

/* How far a surface is downsampled before it is blurred. Blur cost is
 * quadratic in resolution and linear in radius, so this is the difference
 * between "a millisecond" and "not on this device". */
#define CANVAS_BLUR_SHIFT   2
#define CANVAS_BLUR_FACTOR  (1 << CANVAS_BLUR_SHIFT)

#define CANVAS_MAX_LAYERS   8

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */

struct canvas_rect
{
    int x, y, w, h;
};

bool canvas_rect_empty(const struct canvas_rect *r);
/* a := a n b; returns false and empties a if they do not overlap */
bool canvas_rect_intersect(struct canvas_rect *a, const struct canvas_rect *b);
/* a := smallest rect containing both. An empty a is replaced by b. */
void canvas_rect_union(struct canvas_rect *a, const struct canvas_rect *b);
bool canvas_rect_contains(const struct canvas_rect *a, const struct canvas_rect *b);
long canvas_rect_area(const struct canvas_rect *r);

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

struct canvas_surface
{
    canvas_px *px;      /* w*h pixels, stride apart */
    uint8_t   *alpha;   /* NULL for a fully opaque surface */
    int w, h;
    int stride;         /* in pixels, >= w */
};

void canvas_surface_init(struct canvas_surface *s, canvas_px *px,
                         uint8_t *alpha, int w, int h, int stride);
/* Bytes a surface of this geometry needs, alpha plane included or not. */
size_t canvas_surface_bytes(int w, int h, bool with_alpha);

static inline canvas_px *canvas_at(const struct canvas_surface *s, int x, int y)
{
    return s->px + (size_t)y * s->stride + x;
}

static inline uint8_t *canvas_alpha_at(const struct canvas_surface *s, int x, int y)
{
    return s->alpha ? s->alpha + (size_t)y * s->stride + x : NULL;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Blend src over dst by a (0..255). Rounds, so a==255 is exactly src. */
canvas_px canvas_blend_px(canvas_px dst, canvas_px src, unsigned a);

void canvas_fill(struct canvas_surface *s, const struct canvas_rect *r,
                 canvas_px colour);
/* Fill including the alpha plane, for building a layer from scratch. */
void canvas_fill_a(struct canvas_surface *s, const struct canvas_rect *r,
                   canvas_px colour, uint8_t alpha);

/* Straight copy of src's srect to (dx, dy) in dst. Clipped both ends. */
void canvas_blit(struct canvas_surface *dst, int dx, int dy,
                 const struct canvas_surface *src,
                 const struct canvas_rect *srect);

/* src over dst, using src's alpha plane scaled by opacity (0..255). */
void canvas_blend(struct canvas_surface *dst, int dx, int dy,
                  const struct canvas_surface *src,
                  const struct canvas_rect *srect, unsigned opacity);

/* ------------------------------------------------------------------ */
/* The compositor                                                      */
/* ------------------------------------------------------------------ */

enum canvas_blend_mode
{
    CANVAS_COPY,        /* ignore alpha - the bottom layer wants this */
    CANVAS_OVER,        /* source-over, the usual one */
};

struct canvas_layer
{
    struct canvas_surface *surf;
    int x, y;                   /* position in target coordinates */
    uint8_t opacity;            /* 0..255, folded into the blend */
    uint8_t blend;              /* enum canvas_blend_mode */
    bool visible;
    struct canvas_rect painted; /* where it landed last composite */
};

struct canvas
{
    struct canvas_surface *target;
    struct canvas_layer layers[CANVAS_MAX_LAYERS];
    int nlayers;
    struct canvas_rect damage;  /* target coordinates, this frame */
    unsigned long composites;
    unsigned long px_touched;
};

void canvas_init(struct canvas *c, struct canvas_surface *target);

/* Returns the layer index, or -1 if the stack is full. Layers composite
 * back to front in the order they were added. */
int canvas_add_layer(struct canvas *c, struct canvas_surface *surf,
                     int x, int y, enum canvas_blend_mode blend);

/* Every one of these damages what needs damaging by itself. A layer that
 * moves damages both where it was and where it is going - which is the
 * whole reason an overlay drawn through the compositor cannot leave a
 * trail behind it, however slowly it moves. */
void canvas_layer_move(struct canvas *c, int idx, int x, int y);
void canvas_layer_set_opacity(struct canvas *c, int idx, unsigned opacity);
void canvas_layer_set_visible(struct canvas *c, int idx, bool visible);
/* The layer redrew part of its own surface. r is in layer coordinates. */
void canvas_layer_damage(struct canvas *c, int idx, const struct canvas_rect *r);
void canvas_layer_damage_all(struct canvas *c, int idx);

void canvas_damage(struct canvas *c, const struct canvas_rect *r);
void canvas_damage_all(struct canvas *c);
bool canvas_has_damage(const struct canvas *c);

/* Composite the damaged region back to front into the target. Returns the
 * region actually touched (empty if there was nothing to do) and clears
 * the damage. */
struct canvas_rect canvas_composite(struct canvas *c);

/* ------------------------------------------------------------------ */
/* Effects                                                             */
/* ------------------------------------------------------------------ */

/* Scratch a blur of this rectangle needs, in pixels. Two downsampled
 * buffers: one to reduce into, one to ping-pong the separable passes. */
size_t canvas_blur_scratch_px(int w, int h);

/* Box blur r in place: downsample by CANVAS_BLUR_FACTOR, blur small with
 * two separable passes, bilinear back up. radius is in *source* pixels.
 * Returns false if the scratch is too small or the rect degenerate. */
bool canvas_blur(struct canvas_surface *s, const struct canvas_rect *r,
                 int radius, canvas_px *scratch, size_t scratch_px);

/* Write a rounded-rectangle coverage ramp into the surface's alpha plane.
 * Antialiased: the corner is sampled, not stepped, which is where the
 * "antialiased shapes" row of the parity table is actually paid for.
 * Needs an alpha plane; does nothing without one. */
void canvas_round_rect(struct canvas_surface *s, const struct canvas_rect *r,
                       int radius, uint8_t inside, uint8_t outside);

/* Filled and stroked rounded rectangles, blended straight into dst with
 * an antialiased edge. A circle is one of these with corner == w/2, which
 * is how the joystick overlay draws its rings without a bitmap and without
 * the 32-gon it used to approximate them with. Stroke is centred on the
 * boundary, so a 2 px stroke puts one pixel either side of it. */
void canvas_fill_round_rect(struct canvas_surface *dst,
                            const struct canvas_rect *r, int corner,
                            canvas_px colour, unsigned opacity);
void canvas_stroke_round_rect(struct canvas_surface *dst,
                              const struct canvas_rect *r, int corner,
                              int thickness, canvas_px colour,
                              unsigned opacity);

/* A soft drop shadow of rect r, drawn into dst. offset moves it, radius
 * softens it. Cheap: the shadow is a rounded rect rendered directly as a
 * coverage falloff rather than a blurred buffer. */
void canvas_shadow(struct canvas_surface *dst, const struct canvas_rect *r,
                   int corner, int radius, int dx, int dy,
                   canvas_px colour, unsigned opacity);

/* Vertical or horizontal two-stop gradient. With dither on, an ordered
 * 4x4 threshold breaks up RGB565 banding for the cost of a lookup and an
 * add per pixel - no bandwidth at all, which is why this is the answer to
 * banding rather than going 32bpp. */
void canvas_gradient(struct canvas_surface *s, const struct canvas_rect *r,
                     canvas_px from, canvas_px to, bool vertical, bool dither);

/* Nine-slice: stretch src into drect keeping its corners intact. */
void canvas_9slice(struct canvas_surface *dst, const struct canvas_rect *drect,
                   const struct canvas_surface *src,
                   int left, int top, int right, int bottom);

/* Flip srect vertically into dst at (dx, dy), fading alpha from `top` to
 * `bottom` down the height. The classic album-art reflection. */
void canvas_reflect(struct canvas_surface *dst, int dx, int dy,
                    const struct canvas_surface *src,
                    const struct canvas_rect *srect,
                    int height, unsigned top, unsigned bottom);

/* ------------------------------------------------------------------ */
/* Text coverage                                                       */
/* ------------------------------------------------------------------ */

/* Blend a coverage map (0 = none, 255 = full) as `colour` into dst. This
 * is the 8-bit end of the text pipeline: a rasteriser hands over coverage,
 * this puts it on the screen.
 *
 * gamma == true applies a perceptual correction to the coverage before
 * blending. Blending in RGB565 is blending in a gamma-encoded space, which
 * makes light text on dark ground bloom and dark text on light ground go
 * thin; correcting the coverage is the cheap approximation of doing the
 * blend in linear light, and it is per-pixel a single table lookup. */
void canvas_blend_coverage(struct canvas_surface *dst, int dx, int dy,
                           const uint8_t *cov, int cov_stride,
                           int w, int h, canvas_px colour, bool gamma);

/* Expand Rockbox's own 4-bit font alpha (packed two per byte, counting
 * 0 = opaque .. 15 = transparent) into an 8-bit coverage map. Lets the
 * existing font files feed the 8-bit path unchanged. */
void canvas_expand_font4(uint8_t *cov, int cov_stride,
                         const uint8_t *src, int src_stride_px,
                         int w, int h);

/* ------------------------------------------------------------------ */
/* Animation                                                           */
/* ------------------------------------------------------------------ */

enum canvas_ease
{
    CANVAS_EASE_LINEAR,
    CANVAS_EASE_OUT_QUAD,
    CANVAS_EASE_IN_OUT_QUAD,
    CANVAS_EASE_OUT_CUBIC,
    CANVAS_EASE_IN_OUT_CUBIC,
};

struct canvas_anim
{
    int32_t from, to;
    unsigned start_ms, dur_ms;
    uint8_t ease;
    bool active;
};

void canvas_anim_start(struct canvas_anim *a, int32_t from, int32_t to,
                       unsigned dur_ms, enum canvas_ease ease, unsigned now_ms);
void canvas_anim_stop(struct canvas_anim *a);
/* Current value. Reaches `to` exactly, and stays there once finished. */
int32_t canvas_anim_value(const struct canvas_anim *a, unsigned now_ms);
bool canvas_anim_done(const struct canvas_anim *a, unsigned now_ms);
/* Progress 0..256 after easing - useful for driving anything else. */
int canvas_ease_q8(enum canvas_ease ease, int t_q8);

#endif /* _CANVAS_H */
