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
 * That alone is not compositing: with one layer beneath the framebuffer,
 * a viewport over another viewport cleared to the backdrop and wiped it,
 * and the one beneath painted straight through it whenever it redrew a
 * line. So on the WPS and the FM screen a viewport declared after one it
 * overlaps is *lifted*: it draws into a surface of its own
 * (lcd-layers.c), its clears make the surface transparent rather than
 * touch the framebuffer, and its background - the %Vt veil, or the
 * backdrop for a viewport without one - is laid over whatever is beneath
 * when the panel is updated. Declaration order is z order, as it always
 * was for a full redraw, and now for every other redraw too.
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
#include "skin_engine.h"
#include "wps_internals.h"
#ifdef HAVE_LCD_LAYERS
#include "lcd-layers.h"
#endif

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

#if defined(HAVE_LCD_LAYERS) && !defined(__PCTOOL__)

/* The backdrop the pass would have cleared to, taken before a lifted
 * viewport turns it off to draw. */
static fb_data *lift_backdrop;
/* A skin's surfaces were baked into the framebuffer; a viewport lifted
 * again now would show its own old pixels through its glass. */
static bool flattened[SKINNABLE_SCREENS_COUNT];

/* Which skin this is, as a surface tag, or -1 for one that does not lift:
 * the status bar, the WPS and the FM screen do, each dropping its own
 * surfaces when it goes. */
static int skin_of(struct gui_wps *gwps)
{
    if (gwps->display->screen_type != SCREEN_MAIN)
        return -1;
    if (gwps == skin_get_gwps(CUSTOM_STATUSBAR, SCREEN_MAIN))
        return CUSTOM_STATUSBAR;
    if (gwps == skin_get_gwps(WPS, SCREEN_MAIN))
        return WPS;
#if CONFIG_TUNER
    if (gwps == skin_get_gwps(FM_SCREEN, SCREEN_MAIN))
        return FM_SCREEN;
#endif
    return -1;
}

/* Puts something on the panel, as opposed to into the backdrop buffer or
 * nowhere: the default viewport does not draw once there are others, and
 * the one that builds %Cb draws into the backdrop. */
static bool draws(char *buf, const struct skin_viewport *svp,
                  const struct skin_element *el)
{
    return !(svp->hidden_flags & VP_NEVER_VISIBLE) &&
           !svp->output_to_backdrop_buffer &&
           !svp->builds_backdrop &&
           !(svp->label == VP_DEFAULT_LABEL &&
             SKINOFFSETTOPTR(buf, el->next));
}

static bool overlap(const struct viewport *a, const struct viewport *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width &&
           a->y < b->y + b->height && b->y < a->y + a->height;
}

void skin_layer_begin(struct gui_wps *gwps, bool full)
{
    struct wps_data *data = gwps->data;
    struct screen *display = gwps->display;
    char *buf = get_skin_buffer(data);
    struct skin_element *el, *lo;
    struct skin_element *tree = SKINOFFSETTOPTR(buf, data->tree);
    struct skin_viewport *base = tree ? SKINOFFSETTOPTR(buf, tree->data)
                                      : NULL;
    int skin = skin_of(gwps);

    /* Lifting depends on where viewports are, which only a full pass can
     * change - and a skin like Snappy Animated has a hundred and fifty of
     * them to compare, pair by pair. */
    if (skin < 0 || !full)
        return;

    /* Everything renders again, so every surface is taken again, in
     * order, and none is left behind by a viewport that has gone. */
    lcd_surface_put_tag(skin);
    flattened[skin] = false;

    /* Hidden viewports count: lifting must not come and go with %Vd, or
     * a viewport would change surfaces every time one under it did. The
     * info viewport is where the lists draw, so it stays on the panel. */
    for (el = tree; el;
         el = SKINOFFSETTOPTR(buf, el->next))
    {
        struct skin_viewport *svp = SKINOFFSETTOPTR(buf, el->data);
        if (!svp)
            continue;
        svp->lifted = false;
        if (!draws(buf, svp, el) || svp->is_infovp)
            continue;
        for (lo = tree; lo != el;
             lo = SKINOFFSETTOPTR(buf, lo->next))
        {
            struct skin_viewport *lsvp = SKINOFFSETTOPTR(buf, lo->data);
            if (lsvp && draws(buf, lsvp, lo) && overlap(&lsvp->vp, &svp->vp))
            {
                svp->lifted = true;
                break;
            }
        }

        /* A lifted viewport never clears the framebuffer, so on a full
         * pass the ground beneath it is cleared here, before anything
         * beneath it draws. */
        if (svp->lifted)
        {
            struct viewport ground = svp->vp;
            ground.buffer = NULL;
            ground.bg_pattern = base ? base->vp.bg_pattern
                                      : ground.bg_pattern;
            skin_backdrop_show(data->backdrop_id);
            display->set_viewport(&ground);
            display->clear_viewport();
            display->set_viewport(NULL);
        }
    }
}

bool skin_layer_enter(struct gui_wps *gwps, struct skin_viewport *svp,
                      int z)
{
    struct viewport *vp = &svp->vp;
    struct lcd_surface *s = NULL;
    int skin;

    if (!svp->lifted)
        return false;
    skin = skin_of(gwps);
    /* The status bar is drawn over the screen it is on. */
    if (skin >= 0 && !flattened[skin])
        s = lcd_surface_get(svp, skin, vp->x, vp->y, vp->width, vp->height,
                            (skin == CUSTOM_STATUSBAR ? 2000 : 1000) + z);
    if (!s)
    {
        /* No room: this one draws the old way until the next full pass. */
        svp->lifted = false;
        return false;
    }
    vp->buffer = &s->fb;
    /* The lcd's backdrop is addressed relative to the framebuffer, so it
     * cannot be drawn from into a surface, and every background fill has
     * to leave the key behind: the background is the veil. */
    lift_backdrop = lcd_get_backdrop();
    gwps->display->backdrop_show(NULL);
    svp->layer_bg = vp->bg_pattern;
    vp->bg_pattern = LCD_LAYER_KEY;
    return s->fresh;
}

void skin_layer_exit(struct skin_viewport *svp)
{
    struct lcd_surface *s;

    if (!svp->lifted)
        return;
    svp->vp.bg_pattern = svp->layer_bg;
    s = lcd_surface_find(svp);
    if (s)
    {
        s->fresh = false;
        /* skin_render() resets the last viewport's buffer on its way out,
         * and the scroll engine draws through it. */
        svp->vp.buffer = &s->fb;
    }
}

void skin_layer_hide(struct screen *display, struct skin_viewport *svp)
{
    struct lcd_surface *s = svp->lifted ? lcd_surface_find(svp) : NULL;

    if (!s)
    {
        skin_layer_clear_viewport(display, svp);
        return;
    }
    lcd_scroll_stop_viewport(&svp->vp);
    lcd_surface_put(s);
}

void skin_layer_leave(int skin)
{
    lcd_surface_put_tag(skin);
}

void skin_layer_flatten(void)
{
    int i;
    if (!lcd_surface_flatten())
        return;
    for (i = 0; i < SKINNABLE_SCREENS_COUNT; i++)
        flattened[i] = true;
}

unsigned skin_layer_bg(const struct skin_viewport *svp)
{
    return (svp->lifted && svp->vp.bg_pattern == LCD_LAYER_KEY)
           ? svp->layer_bg : svp->vp.bg_pattern;
}

bool skin_layer_set_bg(struct skin_viewport *svp, unsigned colour)
{
    if (!svp->lifted || svp->vp.bg_pattern != LCD_LAYER_KEY)
        return false;
    svp->layer_bg = colour;
    return true;
}

/* A lifted viewport's clear: the surface goes transparent and gets its
 * background back as a veil, so whatever is beneath shows through it -
 * live, not as it was when the viewport cleared. */
static bool clear_lifted(struct skin_viewport *svp, int veil)
{
    struct viewport *vp = &svp->vp;
    struct lcd_surface *s = svp->lifted ? lcd_surface_from_fb(vp->buffer)
                                        : NULL;
    if (!s)
        return false;

    lcd_surface_clear(s);
    if (veil >= 0)
        lcd_surface_veil(s, s->x, s->y, s->w, s->h, svp->layer_bg,
                         svp->layer_bg, (unsigned)veil * 255 / 100, NULL);
    else if (lift_backdrop)
        /* No %Vt is the old opaque clear: the backdrop, or the colour. */
        lcd_surface_veil(s, s->x, s->y, s->w, s->h, 0, 0, 0, lift_backdrop);
    else
        lcd_surface_veil(s, s->x, s->y, s->w, s->h, svp->layer_bg,
                         svp->layer_bg, 255, NULL);

    lcd_scroll_stop_viewport(vp);
    vp->flags &= ~(VP_FLAG_VP_SET_CLEAN);
    return true;
}
#endif /* HAVE_LCD_LAYERS && !__PCTOOL__ */

void skin_layer_clear_viewport(struct screen *display,
                               struct skin_viewport *svp)
{
#if (LCD_DEPTH > 1) || (defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1))
    int veil = svp->clear_veil;
#else
    int veil = -1;
#endif

#if defined(HAVE_LCD_LAYERS) && !defined(__PCTOOL__)
    if (clear_lifted(svp, veil))
        return;
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
#ifdef HAVE_LCD_LAYERS
    {
        /* In a surface there is nothing to blend with until the panel is
         * updated: the rectangle becomes one of its veils. */
        struct lcd_surface *s = lcd_surface_from_fb(vp->buffer);
        if (s)
        {
            lcd_surface_veil(s, vp->x + x, vp->y + y, w, h,
                             (fb_data)start_colour, (fb_data)end_colour,
                             a, NULL);
            return true;
        }
    }
#endif
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
