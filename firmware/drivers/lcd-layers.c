/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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

/* Retained layers, composed at present time.
 *
 * Why here and not in the overlay. Every overlay glitch so far had the same
 * shape: an overlay drew into the framebuffer everyone else draws into, and
 * then had to get its pixels back out - by asking the screen underneath to
 * repaint (flicker), by saving and restoring what it covered (stale the
 * moment the screen repainted on its own), or by forgetting (ink left
 * behind). No single component owned the final frame.
 *
 * So the overlay does not go into the framebuffer at all. It lives in a
 * layer of its own, and the only place the two meet is the copy to the
 * panel: lcd_update() and lcd_update_rect() lay the layers over the
 * framebuffer, let the target present it, and put the framebuffer's own
 * pixels straight back. The framebuffer never holds overlay pixels outside
 * that call, so a screen repainting underneath cannot eat the overlay and
 * dismissing the overlay is clearing its layer.
 *
 * All layers are composed on every update while they have content, not only
 * inside the updated rectangle: a page-flipping driver replays regions the
 * back plane missed out of the framebuffer, and those replays have to see
 * the overlay too or the two planes disagree and it flickers.
 *
 * The cost is proportional to what the layers cover - a few strips and a
 * ring for the stick - and nothing at all while they are empty. Only the UI
 * thread draws, so there is no locking. */

#include "config.h"
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "lcd-layers.h"
#include "lcd-transition.h"

#define MAX_RECTS 8

struct lrect { short x, y, w, h; };

struct layer
{
    fb_data *px;
    struct frame_buffer_t fb;
    struct lrect r[MAX_RECTS];
    int n;
    bool ready;
};

static fb_data overlay_px[LCD_HEIGHT * LCD_WIDTH];
/* What the composed pixels replaced, at the same offsets. */
static fb_data saved_px[LCD_HEIGHT * LCD_WIDTH];

static struct layer layers[LCD_LAYER_COUNT] =
{
    [LCD_LAYER_OVERLAY] = { .px = overlay_px },
};

static bool composed;

extern struct frame_buffer_t lcd_framebuffer_default;

static inline fb_data *base_px(void)
{
    return lcd_framebuffer_default.fb_ptr;
}

#define LAYER_STRIDE LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride)

static struct layer *get(enum lcd_layer l)
{
    struct layer *ly = &layers[l];
    if (!ly->ready)
    {
        size_t i;
        for (i = 0; i < ARRAYLEN(overlay_px); i++)
            ly->px[i] = LCD_LAYER_KEY;
        /* Same geometry as the framebuffer, and the same address function:
         * it resolves against the current viewport's buffer, which is this
         * one while the owner draws. */
        ly->fb = lcd_framebuffer_default;
        ly->fb.fb_ptr = ly->px;
        ly->ready = true;
    }
    return ly;
}

struct frame_buffer_t *lcd_layer_fb(enum lcd_layer layer)
{
    return &get(layer)->fb;
}

bool lcd_layer_has_content(enum lcd_layer layer)
{
    return layers[layer].n > 0;
}

static bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > LCD_WIDTH)  *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

void lcd_layer_mark(enum lcd_layer layer, int x, int y, int w, int h)
{
    struct layer *ly = get(layer);
    struct lrect *r;
    int i;

    if (!clip(&x, &y, &w, &h))
        return;

    /* Already covered by one we have? */
    for (i = 0; i < ly->n; i++)
    {
        r = &ly->r[i];
        if (x >= r->x && y >= r->y &&
            x + w <= r->x + r->w && y + h <= r->y + r->h)
            return;
    }

    if (ly->n == MAX_RECTS)
    {
        /* Out of slots: fold into the last one. Bigger, never wrong. */
        r = &ly->r[MAX_RECTS - 1];
        int x2 = MAX(r->x + r->w, x + w), y2 = MAX(r->y + r->h, y + h);
        r->x = MIN(r->x, x);
        r->y = MIN(r->y, y);
        r->w = x2 - r->x;
        r->h = y2 - r->y;
        return;
    }

    r = &ly->r[ly->n++];
    r->x = x; r->y = y; r->w = w; r->h = h;
}

void lcd_layer_clear(enum lcd_layer layer,
                     void (*cleared)(int x, int y, int w, int h))
{
    struct layer *ly = get(layer);
    int i, row, col;

    for (i = 0; i < ly->n; i++)
    {
        struct lrect *r = &ly->r[i];
        for (row = 0; row < r->h; row++)
        {
            fb_data *p = ly->px + (size_t)(r->y + row) * LAYER_STRIDE + r->x;
            for (col = 0; col < r->w; col++)
                p[col] = LCD_LAYER_KEY;
        }
        if (cleared)
            cleared(r->x, r->y, r->w, r->h);
    }
    ly->n = 0;
}


/* Surfaces.
 *
 * One arena the size of the panel holds them all. A surface keeps its own
 * columns and is given rows of the arena: its own rows if nothing is
 * there, the first free ones that fit if something is. The viewport
 * addresses it with panel coordinates through a buffer whose origin is
 * moved by the difference, so everything that writes a buffer at
 * absolute coordinates - the lcd driver, skin_art_fx.c, skin_layer.c -
 * works on a surface unchanged. */
#define MAX_SURFACES 64

/* Pages of these that no surface has touched are never made resident on
 * a hosted target, so a skin that lifts little costs little. */
static fb_data arena_px[LCD_HEIGHT * LCD_WIDTH];
static unsigned char arena_cov[LCD_HEIGHT * LCD_WIDTH];
static struct lcd_surface surfaces[MAX_SURFACES];
/* The ones in use, in ascending z. */
static struct lcd_surface *zorder[MAX_SURFACES];
static int nsurfaces;

static inline fb_data *arena_row(const struct lcd_surface *s, int row)
{
    return arena_px + (size_t)(s->row + row) * LAYER_STRIDE + s->x;
}

static bool arena_free(int x, int row, int w, int h)
{
    int i;
    for (i = 0; i < nsurfaces; i++)
    {
        const struct lcd_surface *s = zorder[i];
        if (x < s->x + s->w && s->x < x + w &&
            row < s->row + s->h && s->row < row + h)
            return false;
    }
    return true;
}

struct lcd_surface *lcd_surface_find(const void *owner)
{
    int i;
    for (i = 0; i < nsurfaces; i++)
        if (zorder[i]->owner == owner)
            return zorder[i];
    return NULL;
}

struct lcd_surface *lcd_surface_from_fb(const struct frame_buffer_t *fb)
{
    const struct lcd_surface *s = (const struct lcd_surface *)fb;
    if (s < surfaces || s >= surfaces + MAX_SURFACES || !s->owner)
        return NULL;
    return (struct lcd_surface *)s;
}

unsigned char *lcd_surface_coverage(const fb_data *px)
{
    if (px < arena_px || px >= arena_px + ARRAYLEN(arena_px))
        return NULL;
    return arena_cov + (px - arena_px);
}

void lcd_surface_clear(struct lcd_surface *s)
{
    int row, col;
    for (row = 0; row < s->h; row++)
    {
        fb_data *p = arena_row(s, row);
        for (col = 0; col < s->w; col++)
            p[col] = LCD_LAYER_KEY;
        memset(arena_cov + (p - arena_px), 255, s->w);
    }
    s->nveils = 0;
}

struct lcd_surface *lcd_surface_get(const void *owner, int tag, int x, int y,
                                    int w, int h, int z)
{
    struct lcd_surface *s = lcd_surface_find(owner);
    int row, i;

    if (!clip(&x, &y, &w, &h))
        return NULL;
    if (s && s->x == x && s->y == y && s->w == w && s->h == h)
        return s;
    if (s)
        lcd_surface_put(s);
    if (nsurfaces == MAX_SURFACES)
        return NULL;

    /* Its own rows, or the first that are free. */
    row = y;
    if (!arena_free(x, row, w, h))
    {
        for (row = 0; row + h <= LCD_HEIGHT; row++)
            if (arena_free(x, row, w, h))
                break;
        if (row + h > LCD_HEIGHT)
            return NULL;
    }

    for (s = surfaces; s->owner; s++)
        ;
    s->owner = owner;
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->row = row;
    s->z = z;
    s->tag = tag;
    s->fresh = true;
    s->fb = lcd_framebuffer_default;
    s->fb.fb_ptr = arena_px + (ptrdiff_t)(row - y) * LAYER_STRIDE;
    lcd_surface_clear(s);

    for (i = nsurfaces; i > 0 && zorder[i - 1]->z > z; i--)
        zorder[i] = zorder[i - 1];
    zorder[i] = s;
    nsurfaces++;
    return s;
}

void lcd_surface_put(struct lcd_surface *s)
{
    int i;
    for (i = 0; i < nsurfaces && zorder[i] != s; i++)
        ;
    if (i == nsurfaces)
        return;
    for (; i < nsurfaces - 1; i++)
        zorder[i] = zorder[i + 1];
    nsurfaces--;
    s->owner = NULL;
}

void lcd_surface_put_tag(int tag)
{
    int i = 0;
    while (i < nsurfaces)
    {
        if (zorder[i]->tag == tag)
            lcd_surface_put(zorder[i]);
        else
            i++;
    }
}

void lcd_surface_put_all(void)
{
    while (nsurfaces)
        lcd_surface_put(zorder[0]);
}

void lcd_surface_veil(struct lcd_surface *s, int x, int y, int w, int h,
                      fb_data top, fb_data bottom, unsigned alpha,
                      const fb_data *src)
{
    struct lcd_veil *v;
    int i;

    if (!src && alpha == 0)
        return;
    /* A partial redraw lays the same veil again; it must not darken. */
    for (i = 0; i < s->nveils; i++)
    {
        v = &s->veil[i];
        if (v->x == x && v->y == y && v->w == w && v->h == h &&
            v->top == top && v->bottom == bottom && v->alpha == alpha &&
            v->src == src)
            return;
    }
    /* ponytail: a surface's ninth veil is dropped; raise
     * LCD_SURFACE_VEILS if a skin needs more. */
    if (s->nveils == LCD_SURFACE_VEILS)
        return;
    v = &s->veil[s->nveils++];
    v->x = x; v->y = y; v->w = w; v->h = h;
    v->top = top; v->bottom = bottom;
    v->alpha = MIN(alpha, 255);
    v->src = src;
}

/* RGB565 lerp, a of 255 towards src. */
static inline fb_data blend565(fb_data dst, fb_data src, unsigned a)
{
    unsigned na = 255 - a;
    unsigned r = (((src >> 11) & 0x1f) * a + ((dst >> 11) & 0x1f) * na) / 255;
    unsigned g = (((src >>  5) & 0x3f) * a + ((dst >>  5) & 0x3f) * na) / 255;
    unsigned b = (((src      ) & 0x1f) * a + ((dst      ) & 0x1f) * na) / 255;
    return (fb_data)((r << 11) | (g << 5) | b);
}

/* Painter's order: the veils - the surface's background - over what is
 * beneath, wherever any of it shows through, then the surface's own
 * pixels over that, by their coverage. */
static void paint_surface(const struct lcd_surface *s, fb_data *base)
{
    int i, row, c;

    for (i = 0; i < s->nveils; i++)
    {
        const struct lcd_veil *v = &s->veil[i];
        int x = v->x, y = v->y, w = v->w, h = v->h;
        int x0 = MAX(x, s->x), y0 = MAX(y, s->y);
        int x1 = MIN(x + w, s->x + s->w), y1 = MIN(y + h, s->y + s->h);

        for (row = y0; row < y1; row++)
        {
            const fb_data *sp = arena_row(s, row - s->y) + (x0 - s->x);
            size_t off = (size_t)row * LAYER_STRIDE + x0;
            fb_data *bp = base + off;
            fb_data colour = blend565(v->top, v->bottom,
                                      h > 1 ? (row - y) * 255 / (h - 1) : 0);
            const unsigned char *cp = arena_cov + (sp - arena_px);
            for (c = 0; c < x1 - x0; c++)
            {
                if (sp[c] != LCD_LAYER_KEY && cp[c] == 255)
                    continue;
                bp[c] = v->src ? v->src[off + c]
                               : blend565(bp[c], colour, v->alpha);
            }
        }
    }

    for (row = 0; row < s->h; row++)
    {
        const fb_data *sp = arena_row(s, row);
        const unsigned char *cp = arena_cov + (sp - arena_px);
        fb_data *bp = base + (size_t)(s->y + row) * LAYER_STRIDE + s->x;
        for (c = 0; c < s->w; c++)
        {
            if (sp[c] == LCD_LAYER_KEY)
                continue;
            bp[c] = cp[c] == 255 ? sp[c] : blend565(bp[c], sp[c], cp[c]);
        }
    }
}

bool lcd_surface_flatten(void)
{
    int i;
    if (composed || !nsurfaces)
        return false;
    for (i = 0; i < nsurfaces; i++)
        paint_surface(zorder[i], base_px());
    lcd_surface_put_all();
    return true;
}

static bool any_content(void)
{
    int l;
    if (nsurfaces)
        return true;
    for (l = 0; l < LCD_LAYER_COUNT; l++)
        if (layers[l].n)
            return true;
    return false;
}

enum pass { SAVE, PAINT, RESTORE };

static void copy_rect(enum pass pass, int x, int y, int w, int h)
{
    fb_data *base = base_px();
    int row;

    for (row = 0; row < h; row++)
    {
        size_t off = (size_t)(y + row) * LAYER_STRIDE + x;
        if (pass == SAVE)
            memcpy(saved_px + off, base + off, w * sizeof(fb_data));
        else
            memcpy(base + off, saved_px + off, w * sizeof(fb_data));
    }
}

static void run_pass(enum pass pass)
{
    fb_data *base = base_px();
    int l, i, row, c;

    /* Surfaces first: they are under the layers. */
    for (i = 0; i < nsurfaces; i++)
    {
        const struct lcd_surface *s = zorder[i];
        if (pass == PAINT)
            paint_surface(s, base);
        else
            copy_rect(pass, s->x, s->y, s->w, s->h);
    }

    for (l = 0; l < LCD_LAYER_COUNT; l++)
    {
        const struct layer *ly = &layers[l];
        for (i = 0; i < ly->n; i++)
        {
            const struct lrect *r = &ly->r[i];
            if (pass != PAINT)
            {
                copy_rect(pass, r->x, r->y, r->w, r->h);
                continue;
            }
            for (row = 0; row < r->h; row++)
            {
                size_t off = (size_t)(r->y + row) * LAYER_STRIDE + r->x;
                const fb_data *lp = ly->px + off;
                fb_data *bp = base + off;
                for (c = 0; c < r->w; c++)
                    if (lp[c] != LCD_LAYER_KEY)
                        bp[c] = lp[c];
            }
        }
    }
}

/* Lay the layers over the framebuffer, or take them back off.
 *
 * Saving is a whole pass of its own before anything is painted, so where
 * two rectangles overlap the second cannot save a pixel the first already
 * replaced: everything saved is the framebuffer's own. Taking the layers
 * off is copying the saved rectangles back, which restores exactly what
 * was there - pixels a layer left transparent were never changed. */
void lcd_layers_compose(bool on)
{
    if (on == composed || (on && !any_content()))
        return;

    if (on)
    {
        run_pass(SAVE);
        run_pass(PAINT);
    }
    else
        run_pass(RESTORE);
    composed = on;
}

/* The update functions everything calls. The target's own are renamed to
 * *_base by its driver. */
void lcd_update(void)
{
#if defined(HAVE_LCD_TRANSITIONS) && !defined(BOOTLOADER)
    if (lcd_transition_hold())
        return;
#endif
    if (composed || !any_content())
    {
        lcd_update_base();
        return;
    }
    lcd_layers_compose(true);
    lcd_update_base();
    lcd_layers_compose(false);
}

void lcd_update_rect(int x, int y, int width, int height)
{
#if defined(HAVE_LCD_TRANSITIONS) && !defined(BOOTLOADER)
    if (lcd_transition_hold())
        return;
#endif
    if (composed || !any_content())
    {
        lcd_update_rect_base(x, y, width, height);
        return;
    }
    lcd_layers_compose(true);
    lcd_update_rect_base(x, y, width, height);
    lcd_layers_compose(false);
}
