/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Album art, blurred and mirrored: the two things the compositor could do
 * that a skin had no way to ask for.
 *
 * The skin engine draws album art exactly once, at one size, at one place.
 * Everything a modern player does with a cover - throw it across the
 * background out of focus, stand it on its own reflection - was therefore
 * not a matter of writing a cleverer .wps, it was simply unavailable.
 * apps/canvas.c has had `canvas_blur` and `canvas_reflect` since it was
 * written and nothing called them. These two tags are the join.
 *
 * Where the output goes matters more than how it is drawn. A skin viewport
 * clears its own background, so anything painted full-screen and early is
 * wiped by the next viewport that sits on top of it. The backdrop buffer
 * is the one surface that survives that, because a viewport clear *is* a
 * copy from it - which is why %Cb belongs inside a %VB viewport and says
 * so if it is not. The reflection is the opposite case: it is a small
 * thing under the art, it wants to be over the backdrop, and it is drawn
 * straight to the screen.
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

#if defined(HAVE_ALBUMART) && defined(HAVE_COMPOSITOR) && !defined(__PCTOOL__)

#include <string.h>

#include "buffering.h"
#include "playback.h"
#include "canvas.h"
#include "canvas_glue.h"
#include "lcd.h"
#include "screen_access.h"
#include "skin_art_fx.h"
#include "skin_engine.h"
#include "wps_internals.h"

/* The art as the buffering layer holds it: RGB565, at whatever size %Cl
 * asked for.
 *
 * Asking playback rather than reading aa->draw_handle, which is what the
 * first version did and why nothing ever drew: draw_handle is not a
 * property of the album art, it is a one-shot message from %Cd to the
 * drawing code. %Cd sets it and skin_display.c clears it again in the same
 * pass, so anything else reading it sees -1 for ever. The handle these
 * effects want is the one playback is holding for this skin's slot, which
 * is where %Cd got it from in the first place. */
static int art_handle(struct gui_wps *gwps)
{
    struct wps_data *data = gwps->data;

    if (!data || data->playback_aa_slot < 0)
    {
        return -1;
    }

    {
        int h = playback_current_aa_hid(data->playback_aa_slot);
        return h;
    }
}

static struct bitmap *art_bitmap(struct gui_wps *gwps)
{
    struct bitmap *bmp;
    int handle = art_handle(gwps);

    if (handle < 0)
        return NULL;

    if (bufgetdata(handle, 0, (void *)&bmp) <= 0)
        return NULL;

    if (!bmp || bmp->width <= 0 || bmp->height <= 0)
        return NULL;

    return bmp;
}

/* A canvas view of whatever framebuffer this viewport is writing to.
 *
 * Not the LCD's: inside a %VB viewport that is the backdrop buffer, and
 * the whole point of %Cb is that it writes there. vp->buffer is where the
 * skin engine has already pointed it.
 *
 * Coordinates below are absolute (vp->x/vp->y are added by the caller),
 * because a backdrop buffer is the size of the panel whatever the
 * viewport's rectangle happens to be. */
extern struct frame_buffer_t lcd_framebuffer_default;

static bool target_surface(struct screen *display, struct viewport *vp,
                           struct canvas_surface *out)
{
    /* A viewport's buffer is NULL unless something pointed it somewhere
     * else - that is how a plain viewport says "the panel". Only %VB
     * viewports carry one of their own, and that is the backdrop buffer.
     * Treating NULL as a failure is why the first version of this drew
     * nothing at all and said nothing about it. */
    struct frame_buffer_t *fb = (vp && vp->buffer) ? vp->buffer
                                                   : &lcd_framebuffer_default;

    if (!fb->fb_ptr)
        return false;

    canvas_surface_init(out, fb->fb_ptr, NULL,
                        display->lcdwidth, display->lcdheight,
                        (int)LCD_NATIVE_STRIDE(fb->stride));
    return true;
}

/* Nearest-neighbour scale of src into dst's (dx,dy,dw,dh), cropped to
 * cover: the picture fills the rectangle and the overflow on the long axis
 * is thrown away either side, which is what "background" means - letterbox
 * bars on a backdrop would be worse than no backdrop.
 *
 * Nearest and not bilinear on purpose. This is about to be blurred with a
 * radius of tens of pixels; the difference between the two survives none
 * of that, and bilinear costs four reads and three blends per pixel on a
 * 480x800 fill that is already the expensive part of the frame. */
static void scale_cover(struct canvas_surface *dst, int dx, int dy,
                        int dw, int dh, const struct bitmap *src)
{
    int sw = src->width, sh = src->height;
    const fb_data *sp = (const fb_data *)src->data;
    int sstride = STRIDE_MAIN(sw, sh);
    /* 16.16 step through the source, and the offset that centres the crop */
    int32_t stepx, stepy, sx0, sy0;
    int x, y;

    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;

    /* cover: the *smaller* source step wins, so neither axis runs short */
    stepx = ((int32_t)sw << 16) / dw;
    stepy = ((int32_t)sh << 16) / dh;
    if (stepx > stepy)
        stepx = stepy;
    else
        stepy = stepx;

    sx0 = (((int32_t)sw << 16) - stepx * dw) / 2;
    sy0 = (((int32_t)sh << 16) - stepy * dh) / 2;

    for (y = 0; y < dh; y++)
    {
        int32_t sy = sy0 + stepy * y;
        int syi = sy >> 16;
        const fb_data *srow;
        canvas_px *drow;
        int32_t sx = sx0;

        if (syi < 0) syi = 0;
        if (syi >= sh) syi = sh - 1;
        if (dy + y < 0 || dy + y >= dst->h)
            continue;

        srow = sp + (size_t)syi * sstride;
        drow = canvas_at(dst, dx, dy + y);
        if (!drow)
            continue;

        for (x = 0; x < dw; x++, sx += stepx)
        {
            int sxi = sx >> 16;
            if (sxi < 0) sxi = 0;
            if (sxi >= sw) sxi = sw - 1;
            if (dx + x < 0 || dx + x >= dst->w)
                continue;
            drow[x] = (canvas_px)srow[sxi];
        }
    }
}

/* Which picture the backdrop currently holds.
 *
 * The tag has to be polled rather than drawn once, because the art is not
 * there yet when the skin first renders - buffering loads it a moment
 * later, and a backdrop drawn from an album art handle of -1 is a backdrop
 * that stays empty for the whole track. Polling a 480x800 scale-and-blur
 * every refresh would be absurd, so it is polled and skipped: the work
 * happens on the frame where the handle changes, and that frame asks for a
 * full update so the viewports above re-clear onto the new backdrop. */
static int backdrop_handle = -1;
static int mirror_handle = -1;

void skin_art_fx_reset(void)
{
    backdrop_handle = -1;
    mirror_handle = -1;
}

/* Whether this effect has anything to do this pass.
 *
 * A full update clears every viewport first, so whatever was drawn last
 * time is gone and has to be drawn again - there is no caching to be had
 * there, and the first version's attempt at it drew the picture exactly
 * once and then watched the next clear erase it.
 *
 * Between full updates there is nothing to redraw *except* the moment the
 * art arrives: it is loaded a beat after the skin first renders, so the
 * first full update of a track has no picture to work with. That one case
 * is what the handle is remembered for. */
static bool fx_should_draw(bool full, int handle, int *last)
{
    if (full)
    {
        *last = handle;
        return true;
    }
    if (handle != *last)
    {
        *last = handle;
        return true;
    }
    return false;
}

bool skin_art_backdrop(struct gui_wps *gwps, struct viewport *vp,
                       int x, int y, int w, int h,
                       int radius, int veil_pct, bool full)
{
    struct canvas_surface fb;
    struct canvas_rect r;
    struct bitmap *bmp = art_bitmap(gwps);
    int handle = art_handle(gwps);
    canvas_px *scratch;
    size_t scratch_px;

    if (!bmp || !target_surface(gwps->display, vp, &fb))
        return false;

    if (!fx_should_draw(full, handle, &backdrop_handle))
        return true;
    if (w <= 0) w = fb.w - x;
    if (h <= 0) h = fb.h - y;

    r.x = x; r.y = y; r.w = w; r.h = h;

    scale_cover(&fb, x, y, w, h, bmp);

    if (radius > 0)
    {
        scratch = canvas_scratch(w, h, &scratch_px);
        if (scratch)
            canvas_blur(&fb, &r, radius, scratch, scratch_px);
    }

    /* The veil. A cover thrown across the background at full strength is a
     * background you cannot read text on, and the answer is not to dim the
     * text. veil_pct is how much of the viewport's own background colour
     * goes over the top, so the theme's ground is what the picture fades
     * into and a theme keeps its palette. */
    if (veil_pct > 0)
    {
        /* Blended by hand rather than with canvas_fill_a(), which is not
         * the function its name suggests: it fills the rectangle solid and
         * then writes `alpha` into the surface's *alpha plane*, which is
         * for building a layer to composite later. On a surface with no
         * alpha plane - the framebuffer - that is a solid fill and the
         * picture underneath is simply gone. It was, too: the backdrop
         * came out flat black at every veil setting and perfect at zero. */
        unsigned a = (unsigned)veil_pct * 255 / 100;
        canvas_px veil = (canvas_px)vp->bg_pattern;
        int px, py;

        if (a > 255)
            a = 255;

        for (py = 0; py < h; py++)
        {
            canvas_px *row;
            if (y + py < 0 || y + py >= fb.h)
                continue;
            row = canvas_at(&fb, x, y + py);
            if (!row)
                continue;
            for (px = 0; px < w; px++)
            {
                if (x + px < 0 || x + px >= fb.w)
                    continue;
                row[px] = canvas_blend_px(row[px], veil, a);
            }
        }
    }

    return true;
}

bool skin_art_mirror(struct gui_wps *gwps, struct viewport *vp,
                     int x, int y, int w, int h, int top, int bottom,
                     bool full)
{
    struct canvas_surface fb, art;
    struct canvas_rect srect;
    struct bitmap *bmp = art_bitmap(gwps);

    if (!bmp || !target_surface(gwps->display, vp, &fb))
        return false;

    if (!fx_should_draw(full, art_handle(gwps), &mirror_handle))
        return true;

    if (w <= 0) w = bmp->width;
    if (h <= 0) h = bmp->height / 3;

    canvas_surface_init(&art, (canvas_px *)bmp->data, NULL,
                        bmp->width, bmp->height,
                        STRIDE_MAIN(bmp->width, bmp->height));

    /* The bottom `h` rows of the art are what a reflection of height h can
     * show; taking the whole picture and squashing it would be a second,
     * smaller picture rather than a reflection. */
    srect.x = 0;
    srect.w = MIN(w, bmp->width);
    srect.h = MIN(h, bmp->height);
    srect.y = bmp->height - srect.h;

    canvas_reflect(&fb, x, y, &art, &srect, srect.h,
                   (unsigned)top, (unsigned)bottom);
    return true;
}

#endif /* HAVE_ALBUMART && HAVE_COMPOSITOR */
