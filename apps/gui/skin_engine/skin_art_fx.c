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
 * wiped by the next viewport that sits on top of it. So %Cb does not paint:
 * it composes the blur, the cover and the reflection into a buffer that
 * becomes the LCD backdrop, the one surface a viewport clear restores
 * rather than wipes. See the backdrop section below.
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
#include "core_alloc.h"
#include "misc.h"

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
 * Only %Cm uses this now, and only when there is no album-art backdrop to
 * compose into. Inside a %VB viewport vp->buffer is the skin's backdrop
 * buffer, which is where the skin engine has already pointed it.
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
static int mirror_handle = -1;

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

/* ------------------------------------------------------------ backdrop
 *
 * The blurred cover is not painted onto the panel. It is rendered once per
 * picture into a buffer of its own, and that buffer is handed to the LCD
 * driver as its *backdrop* - the same mechanism a theme's backdrop .bmp
 * uses, and the only real transparency Rockbox has.
 *
 * With a backdrop set, two things change everywhere at once, with no skin
 * or viewport knowing about it:
 *
 *   - clear_viewport() copies the backdrop instead of filling a colour, so
 *     every viewport's rectangle *is* the picture underneath it;
 *   - text drawn in the ordinary solid mode takes its background pixels
 *     from the backdrop, so the glyph lands on the cover and not on a box.
 *
 * The buffer holds the whole picture layer, not only the blur: the sharp
 * cover (%Cd, at the place %Cl put it) and its reflection (%Cm) are
 * composed into it as well. Anything that moves over the cover - a sheen,
 * a %Vt viewport - clears back to the cover rather than to a blurred copy
 * of it. %Cd still draws on the panel as well; %Cm does not while the
 * backdrop is live, because the clear that precedes it already put the
 * reflection there.
 *
 * Why the skin's own %X/%VB backdrop machinery is not used: those buffers
 * are loaded from a file at parse time and shared between skins by name.
 * This one changes with every track and belongs to one skin.
 *
 * Memory: the buffer is reserved when the skin is *loaded* (see
 * skin_art_fx_reserve()), never while rendering. The audio buffer holds
 * every free byte (core_alloc_maximum), so an allocation mid-render makes
 * it shrink - which stops playback, resets the buffering layer, and leaves
 * any album-art bitmap pointer taken beforehand pointing at nothing. That
 * was a segfault in scale_cover() the moment the first cover arrived.
 */

static int      bd_hid = -1;          /* buflib handle of the buffer       */
static fb_data *bd_buf;
static const struct wps_data *bd_owner;
static int      bd_x, bd_y, bd_w, bd_h, bd_radius, bd_veil;
static unsigned bd_ground;
static int      bd_rendered_for = -1; /* art handle the buffer shows       */
static bool     bd_active;            /* the LCD is showing bd_buf         */

/* Where %Cd put the cover on the panel, in absolute coordinates: the
 * viewport origin is only known while rendering, so it is noted then. */
static bool     bd_cover;
static int      bd_cover_vx, bd_cover_vy;

/* %Cm, composed into the buffer while it is active. */
static bool     bd_mirror;
static int      bd_mx, bd_my, bd_mw, bd_mh, bd_mtop, bd_mbottom;

static int bd_move(int handle, void *current, void *new)
{
    (void)handle;
    if (bd_buf == current)
        bd_buf = new;
    /* The driver holds a raw pointer; it has to follow the block. */
    if (bd_active)
        screens[SCREEN_MAIN].backdrop_show((char *)bd_buf);
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks bd_ops = { bd_move, NULL, NULL };

bool skin_art_fx_reserve(void)
{
    if (bd_hid > 0)
        return true;
    bd_hid = core_alloc_ex(LCD_BACKDROP_BYTES, &bd_ops);
    if (bd_hid <= 0)
    {
        bd_hid = -1;
        return false;
    }
    bd_buf = core_get_data(bd_hid);
    return true;
}

static void bd_set_active(bool on)
{
    if (on && !bd_buf)
        on = false;
    if (bd_active == on)
        return;
    bd_active = on;
    screens[SCREEN_MAIN].backdrop_show(on ? (char *)bd_buf : NULL);
}

void skin_art_fx_reset(const struct wps_data *data)
{
    if (data != bd_owner)
        return;
    mirror_handle = -1;
    bd_owner = NULL;
    bd_rendered_for = -1;
    bd_cover = false;
    bd_mirror = false;
    bd_set_active(false);
}

void skin_art_fx_leave(void)
{
    bd_set_active(false);
}

/* The cover as %Cd draws it: cropped to the %Cl box and aligned in it.
 * Mirrors draw_album_art() in skin_display.c. */
static void bd_draw_cover(struct canvas_surface *surf,
                          const struct skin_albumart *aa,
                          const struct bitmap *bmp)
{
    int x = bd_cover_vx + aa->x;
    int y = bd_cover_vy + aa->y;
    int w = bmp->width, h = bmp->height;
    int sstride = STRIDE_MAIN(bmp->width, bmp->height);
    const fb_data *sp = (const fb_data *)bmp->data;
    int row, col;

    if (aa->width > 0)
    {
        w = MIN(bmp->width, aa->width);
        if (aa->xalign & WPS_ALBUMART_ALIGN_RIGHT)
            x += aa->width - w;
        else if (aa->xalign & WPS_ALBUMART_ALIGN_CENTER)
            x += (aa->width - w) / 2;
    }
    if (aa->height > 0)
    {
        h = MIN(bmp->height, aa->height);
        if (aa->yalign & WPS_ALBUMART_ALIGN_BOTTOM)
            y += aa->height - h;
        else if (aa->yalign & WPS_ALBUMART_ALIGN_CENTER)
            y += (aa->height - h) / 2;
    }

    for (row = 0; row < h; row++)
    {
        canvas_px *d;
        const fb_data *s;

        if (y + row < 0 || y + row >= surf->h)
            continue;
        d = canvas_at(surf, 0, y + row);
        s = sp + (size_t)row * sstride;
        for (col = 0; col < w; col++)
        {
            if (x + col < 0 || x + col >= surf->w)
                continue;
            d[x + col] = (canvas_px)s[col];
        }
    }
}

static bool bd_render(struct gui_wps *gwps, int handle)
{
    struct canvas_surface surf;
    struct canvas_rect r;
    struct bitmap *bmp;
    canvas_px *scratch = NULL;
    size_t scratch_px = 0;
    int w, h, px, py;

    if (!bd_buf)
        return false;

    w = bd_w > 0 ? bd_w : LCD_WIDTH  - bd_x;
    h = bd_h > 0 ? bd_h : LCD_HEIGHT - bd_y;
    if (bd_radius > 0)
        scratch = canvas_scratch(w, h, &scratch_px);

    /* Every pointer below is taken after the last call that could have
     * touched buflib, and nothing between here and the end allocates. */
    bmp = art_bitmap(gwps);
    if (!bmp)
        return false;
    bd_buf = core_get_data(bd_hid);

    canvas_surface_init(&surf, (canvas_px *)bd_buf, NULL,
                        LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH);

    /* The ground everywhere, then the picture where the tag asked for it,
     * so a backdrop rectangle smaller than the panel still leaves a clean
     * edge rather than whatever the buffer held last. */
    r.x = 0; r.y = 0; r.w = LCD_WIDTH; r.h = LCD_HEIGHT;
    canvas_fill(&surf, &r, (canvas_px)bd_ground);

    r.x = bd_x; r.y = bd_y; r.w = w; r.h = h;
    scale_cover(&surf, bd_x, bd_y, w, h, bmp);

    if (scratch)
        canvas_blur(&surf, &r, bd_radius, scratch, scratch_px);

    if (bd_veil > 0)
    {
        unsigned a = (unsigned)bd_veil * 255 / 100;
        if (a > 255)
            a = 255;

        for (py = 0; py < h; py++)
        {
            canvas_px *row;
            if (bd_y + py < 0 || bd_y + py >= LCD_HEIGHT)
                continue;
            row = canvas_at(&surf, 0, bd_y + py);
            for (px = 0; px < w; px++)
            {
                int ax = bd_x + px;
                if (ax < 0 || ax >= LCD_WIDTH)
                    continue;
                row[ax] = canvas_blend_px(row[ax], (canvas_px)bd_ground, a);
            }
        }
    }

    if (bd_cover)
    {
        struct skin_albumart *aa =
            SKINOFFSETTOPTR(get_skin_buffer(gwps->data), gwps->data->albumart);
        if (aa)
            bd_draw_cover(&surf, aa, bmp);
    }

    if (bd_mirror)
    {
        struct canvas_surface art;
        struct canvas_rect srect;

        canvas_surface_init(&art, (canvas_px *)bmp->data, NULL,
                            bmp->width, bmp->height,
                            STRIDE_MAIN(bmp->width, bmp->height));
        srect.x = 0;
        srect.w = MIN(bd_mw > 0 ? bd_mw : bmp->width, bmp->width);
        srect.h = MIN(bd_mh > 0 ? bd_mh : bmp->height / 3, bmp->height);
        srect.y = bmp->height - srect.h;
        canvas_reflect(&surf, bd_mx, bd_my, &art, &srect, srect.h,
                       (unsigned)bd_mtop, (unsigned)bd_mbottom);
    }

    bd_rendered_for = handle;
    return true;
}

fb_data *skin_art_backdrop_buffer(void)
{
    return bd_active ? bd_buf : NULL;
}

void skin_art_fx_prepare(struct gui_wps *gwps)
{
    int handle;

    if (!gwps || !bd_owner || gwps->data != bd_owner)
    {
        /* Another skin is rendering. If it is the statusbar drawn over the
         * owner's screen, the backdrop stays; anywhere else it goes, or a
         * list would clear its rows onto the last cover. */
        if (get_current_activity() != ACTIVITY_WPS)
            bd_set_active(false);
        return;
    }

    handle = art_handle(gwps);
    if (handle < 0)
    {
        bd_set_active(false);
        return;
    }

    if (handle != bd_rendered_for)
    {
        if (!bd_render(gwps, handle))
        {
            bd_set_active(false);
            return;
        }
        /* The picture changed underneath every viewport, and only a full
         * update makes all of them clear onto it again. */
        skin_request_full_update(WPS);
    }

    bd_set_active(true);
}

/* Something the backdrop is composed from changed: render it again at the
 * start of the next pass, which is a full one so every viewport re-clears. */
static void bd_invalidate(void)
{
    bd_rendered_for = -1;
    skin_request_full_update(WPS);
}

bool skin_art_backdrop(struct gui_wps *gwps, struct viewport *vp,
                       int x, int y, int w, int h,
                       int radius, int veil_pct, bool full)
{
    (void)full;

    /* The tag only says what the backdrop should be. The work is done in
     * skin_art_fx_prepare(), before the first viewport of each pass,
     * because a backdrop set halfway down the viewport list is a backdrop
     * the viewports above it never cleared onto. */
    if (bd_owner != gwps->data || bd_x != x || bd_y != y ||
        bd_w != w || bd_h != h || bd_radius != radius ||
        bd_veil != veil_pct || bd_ground != vp->bg_pattern)
    {
        if (bd_owner != gwps->data)
        {
            bd_cover = false;
            bd_mirror = false;
        }
        bd_owner  = gwps->data;
        bd_x = x; bd_y = y; bd_w = w; bd_h = h;
        bd_radius = radius;
        bd_veil   = veil_pct;
        bd_ground = vp->bg_pattern;
        bd_invalidate();
    }

    return art_handle(gwps) >= 0;
}

void skin_art_note_cover(struct gui_wps *gwps, struct viewport *vp)
{
    if (!gwps || gwps->data != bd_owner)
        return;
    if (!bd_cover || bd_cover_vx != vp->x || bd_cover_vy != vp->y)
    {
        bd_cover = true;
        bd_cover_vx = vp->x;
        bd_cover_vy = vp->y;
        bd_invalidate();
    }
}

bool skin_art_mirror(struct gui_wps *gwps, struct viewport *vp,
                     int x, int y, int w, int h, int top, int bottom,
                     bool full)
{
    struct canvas_surface fb, art;
    struct canvas_rect srect;
    struct bitmap *bmp;

    /* While this skin's backdrop is live the reflection is part of it. */
    if (gwps->data == bd_owner)
    {
        if (!bd_mirror || bd_mx != x || bd_my != y || bd_mw != w ||
            bd_mh != h || bd_mtop != top || bd_mbottom != bottom)
        {
            bd_mirror = true;
            bd_mx = x; bd_my = y; bd_mw = w; bd_mh = h;
            bd_mtop = top; bd_mbottom = bottom;
            bd_invalidate();
        }
        if (bd_active)
            return true;
    }

    bmp = art_bitmap(gwps);
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
