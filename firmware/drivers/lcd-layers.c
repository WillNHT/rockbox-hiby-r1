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


static bool any_content(void)
{
    int l;
    for (l = 0; l < LCD_LAYER_COUNT; l++)
        if (layers[l].n)
            return true;
    return false;
}

enum pass { SAVE, PAINT, RESTORE };

static void run_pass(enum pass pass)
{
    fb_data *base = base_px();
    int l, i, row, c;

    for (l = 0; l < LCD_LAYER_COUNT; l++)
    {
        const struct layer *ly = &layers[l];
        for (i = 0; i < ly->n; i++)
        {
            const struct lrect *r = &ly->r[i];
            for (row = 0; row < r->h; row++)
            {
                size_t off = (size_t)(r->y + row) * LAYER_STRIDE + r->x;
                switch (pass)
                {
                case SAVE:
                    memcpy(saved_px + off, base + off,
                           r->w * sizeof(fb_data));
                    break;
                case RESTORE:
                    memcpy(base + off, saved_px + off,
                           r->w * sizeof(fb_data));
                    break;
                case PAINT:
                {
                    const fb_data *lp = ly->px + off;
                    fb_data *bp = base + off;
                    for (c = 0; c < r->w; c++)
                        if (lp[c] != LCD_LAYER_KEY)
                            bp[c] = lp[c];
                    break;
                }
                }
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
#ifdef HAVE_LCD_TRANSITIONS
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
#ifdef HAVE_LCD_TRANSITIONS
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
