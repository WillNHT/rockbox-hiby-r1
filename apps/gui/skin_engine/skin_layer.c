/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Layering for the skin engine.
 *
 * Rockbox draws immediately. There is no scene, no z-order and no alpha:
 * a viewport clears its rectangle and then paints into it, and whatever
 * was underneath is gone. That is why a bar of chrome laid over album art
 * comes out as a black box - the box is the clear, filling with the
 * viewport's background colour because the clear has no idea there was a
 * picture there.
 *
 * The engine does have one layer, and only one: the backdrop buffer.
 * lcd_clear_viewport() copies from it instead of filling when it is set,
 * which makes it the single surface that survives being drawn over.
 * Everything here is built on that fact:
 *
 *   - %VB puts a viewport's *output* into the backdrop buffer, so a skin
 *     can compose its own layer at run time rather than loading a BMP.
 *     %Cb writing the blurred cover there is the case this was built for.
 *   - %Vt(veil) says how much of a viewport's own background colour is
 *     laid over that layer when the viewport clears. 0 is glass - the
 *     layer comes through untouched. 100 is the opaque box we started
 *     with. In between is a scrim, which is what text over a picture
 *     actually wants.
 *   - %dr's seventh parameter blends a rectangle over what the clear just
 *     restored, so a sheen is a highlight rather than a painted bar.
 *
 * None of this is compositing: there is still exactly one layer beneath
 * the framebuffer, and the reason a translucent viewport does not smear
 * is that it *does* clear every frame - it just clears to the layer
 * instead of to a colour. Anything that has to be transparent over
 * content which is not in the backdrop buffer cannot be done this way and
 * belongs on canvas_glue.c's overlay, which carries its own backing store.
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

#include "config.h"
#include <stdlib.h>
#include "lcd.h"
#include "scroll_engine.h"
#include "screen_access.h"
#include "skin_layer.h"
#include "wps_internals.h"

/* Blending needs real colour channels and a framebuffer laid out in them,
 * and blend565() works on RGB565 pixels only.
 * On a 1bpp or 2bpp target there is nothing to blend towards, so
 * everything below degrades to the opaque path it replaced. */
#if defined(HAVE_LCD_COLOR) && (LCD_DEPTH == 16) && !defined(__PCTOOL__)
#define SKIN_LAYER_CAN_BLEND
#endif

#ifdef SKIN_LAYER_CAN_BLEND

extern struct frame_buffer_t lcd_framebuffer_default;

/* RGB565 lerp. a is 0..255 of src.
 *
 * Deliberately not the gamma-aware blend from lcd-gamma.h: that one is for
 * glyph coverage, where a wrong answer shows up as a font that blooms. A
 * veil over a picture is a large flat area, and the linear-light version
 * costs three table lookups and three inverse lookups per pixel across a
 * 480x800 fill. */
static inline uint16_t blend565(uint16_t dst, uint16_t src, unsigned a)
{
    unsigned na = 255 - a;
    unsigned r = ((((src >> 11) & 0x1f) * a) + (((dst >> 11) & 0x1f) * na)) / 255;
    unsigned g = ((((src >>  5) & 0x3f) * a) + (((dst >>  5) & 0x3f) * na)) / 255;
    unsigned b = ((((src      ) & 0x1f) * a) + (((dst      ) & 0x1f) * na)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Where this viewport's pixels live, and how far apart its rows are.
 *
 * Not FBADDR(): that resolves against lcd_current_viewport, so it cannot
 * address a buffer the viewport is not set to - which is exactly the
 * backdrop, the thing being read from here. */
static inline fb_data *plane_at(fb_data *base, int stride, int x, int y)
{
    return base + (size_t)y * stride + x;
}

static bool vp_plane(struct screen *display, struct viewport *vp,
                     fb_data **base, int *stride)
{
    struct frame_buffer_t *fb = vp->buffer ? vp->buffer
                                           : &lcd_framebuffer_default;
    if (!fb->fb_ptr)
        return false;
    *base = (fb_data *)fb->fb_ptr;
    *stride = fb->stride ? (int)fb->stride : display->lcdwidth;
    return true;
}

#endif /* SKIN_LAYER_CAN_BLEND */

void skin_layer_clear_viewport(struct screen *display,
                               struct skin_viewport *svp)
{
#if (LCD_DEPTH > 1) || (defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1))
    int veil = svp->clear_veil;
#else
    int veil = -1;
#endif

    /* No %Vt on this viewport: byte for byte the call this replaced. */
    if (veil < 0)
    {
        display->clear_viewport();
        return;
    }

#ifdef SKIN_LAYER_CAN_BLEND
    {
        struct viewport *vp = &svp->vp;
        fb_data *base, *backdrop;
        int stride, y;
        unsigned a;

        backdrop = lcd_get_backdrop();

        /* A veil is a veil *over something*. With no backdrop loaded there
         * is no layer to let through, and clearing to nothing would leave
         * last frame's pixels to be drawn over for ever - the smear this
         * file exists to avoid. Say so by being opaque rather than by
         * looking broken. */
        if (!backdrop || display->screen_type != SCREEN_MAIN ||
            !vp_plane(display, vp, &base, &stride))
        {
            display->clear_viewport();
            return;
        }

        a = (unsigned)veil * 255 / 100;

        for (y = 0; y < vp->height; y++)
        {
            fb_data *d = plane_at(base, stride, vp->x, vp->y + y);
            const fb_data *s = plane_at(backdrop, stride, vp->x, vp->y + y);
            int x;

            if (a == 0)
            {
                for (x = 0; x < vp->width; x++)
                    d[x] = s[x];
            }
            else
            {
                for (x = 0; x < vp->width; x++)
                    d[x] = blend565(s[x], (uint16_t)vp->bg_pattern, a);
            }
        }

        /* clear_viewport() does these two as well as the fill, and a
         * viewport whose scroller was left running would keep writing text
         * into a rectangle nobody cleared. */
        lcd_scroll_stop_viewport(vp);
        vp->flags &= ~(VP_FLAG_VP_SET_CLEAN);
        return;
    }
#else
    display->clear_viewport();
#endif
}

bool skin_layer_fillrect(struct screen *display, struct viewport *vp,
                         int x, int y, int w, int h,
                         unsigned start_colour, unsigned end_colour,
                         unsigned alpha)
{
#ifdef SKIN_LAYER_CAN_BLEND
    fb_data *base;
    int stride, row;
    unsigned a = alpha * 255 / 100;

    if (alpha >= 100 || w <= 0 || h <= 0)
        return false;
    if (display->screen_type != SCREEN_MAIN)
        return false;
    if (!vp_plane(display, vp, &base, &stride))
        return false;

    /* Clip to the viewport; the caller's coordinates are its own. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > vp->width)  w = vp->width  - x;
    if (y + h > vp->height) h = vp->height - y;
    if (w <= 0 || h <= 0)
        return true;
    if (a == 0)
        return true;

    for (row = 0; row < h; row++)
    {
        fb_data *d = plane_at(base, stride, vp->x + x, vp->y + y + row);
        /* Two-stop vertical gradient, the axis %dr already uses. */
        unsigned t = (h > 1) ? (unsigned)row * 255 / (unsigned)(h - 1) : 0;
        uint16_t src = blend565((uint16_t)start_colour,
                                (uint16_t)end_colour, t);
        int col;

        for (col = 0; col < w; col++)
            d[col] = blend565(d[col], src, a);
    }
    return true;
#else
    (void)display; (void)vp; (void)x; (void)y; (void)w; (void)h;
    (void)start_colour; (void)end_colour; (void)alpha;
    return false;
#endif
}
