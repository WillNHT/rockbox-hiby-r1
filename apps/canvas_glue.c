/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Canvas - the Rockbox side.
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
#include "config.h"

#ifdef HAVE_COMPOSITOR

#include "lcd.h"
#include "core_alloc.h"
#include "canvas_glue.h"

/* The main framebuffer, addressed directly. get_address_fn resolves
 * against the current viewport rather than against the buffer it is
 * handed, so it cannot be used to reach the screen from here. */
extern struct frame_buffer_t lcd_framebuffer_default;

#define MAX_OVERLAYS 4

struct canvas_overlay
{
    bool in_use;
    int w, h;                       /* claimed size */
    struct canvas_surface surf;     /* what the caller draws into */
    struct canvas_surface backing;  /* what was underneath */
    struct canvas_rect covered;     /* where, on screen; empty if nowhere */
    size_t surf_off;   /* offsets, not pointers: buflib may move the pool */
    size_t alpha_off;
    size_t back_off;
};

static int pool_handle = -1;
static size_t pool_size, pool_used;
static struct canvas_overlay overlays[MAX_OVERLAYS];
static struct canvas_rect pending;      /* what to push at present() */
static struct canvas_surface screen;
static size_t scratch_off, scratch_len; /* tail of the pool, reused */

/* ------------------------------------------------------------------ */

static void screen_surface(void)
{
    canvas_surface_init(&screen, lcd_framebuffer_default.fb_ptr, NULL,
                        LCD_WIDTH, LCD_HEIGHT,
                        (int)LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride));
}

bool canvas_glue_init(void)
{
    if (pool_handle >= 0)
        return true;

    pool_size = (size_t)CANVAS_POOL_MAX_KIB * 1024;
    pool_handle = core_alloc(pool_size);

    if (pool_handle < 0)
    {
        /* Documented failure mode: no pool, no compositor, and every
         * caller carries on painting straight at the framebuffer. */
        pool_size = 0;
        return false;
    }

    pool_used = 0;
    scratch_off = scratch_len = 0;
    memset(overlays, 0, sizeof(overlays));
    pending.x = pending.y = pending.w = pending.h = 0;
    screen_surface();

    return true;
}

void canvas_glue_shutdown(void)
{
    if (pool_handle >= 0)
        pool_handle = core_free(pool_handle);

    pool_handle = -1;
    pool_size = pool_used = 0;
    scratch_off = scratch_len = 0;
    memset(overlays, 0, sizeof(overlays));
    pending.w = pending.h = 0;
}

bool canvas_available(void)
{
    return pool_handle >= 0;
}

size_t canvas_pool_used(void) { return pool_used; }
size_t canvas_pool_size(void) { return pool_size; }

/* Bump allocation, and deliberately only bump: an overlay is claimed once
 * and kept, and a free list would buy nothing but a way to fragment
 * 384 KiB. Releasing an overlay frees the slot, not the bytes; the pool is
 * only reclaimed wholesale at shutdown. */
#define POOL_NONE ((size_t)-1)

static size_t pool_take(size_t bytes)
{
    size_t off;

    bytes = (bytes + 3u) & ~3u;

    if (pool_handle < 0 || pool_used + bytes + scratch_len > pool_size)
        return POOL_NONE;

    off = pool_used;
    pool_used += bytes;

    return off;
}

/* buflib is free to move an allocation with no move callback, so nothing
 * here may hold a pointer across a call. Surfaces are rebound from the
 * current base every time they are used, which costs one handle lookup. */
static void bind_overlay(struct canvas_overlay *ov)
{
    char *base = core_get_data(pool_handle);

    if (ov->surf_off != POOL_NONE)
        canvas_surface_init(&ov->surf, (canvas_px *)(base + ov->surf_off),
                            (uint8_t *)(base + ov->alpha_off),
                            ov->w, ov->h, ov->w);
    else
        canvas_surface_init(&ov->surf, NULL, NULL, 0, 0, 1);
    canvas_surface_init(&ov->backing, (canvas_px *)(base + ov->back_off),
                        NULL, ov->w, ov->h, ov->w);
}

/* ------------------------------------------------------------------ */

static struct canvas_overlay *overlay_claim(int w, int h, bool with_surface);

struct canvas_overlay *canvas_overlay_claim(int w, int h)
{
    return overlay_claim(w, h, true);
}

struct canvas_overlay *canvas_overlay_claim_backing(int w, int h)
{
    return overlay_claim(w, h, false);
}

static struct canvas_overlay *overlay_claim(int w, int h, bool with_surface)
{
    struct canvas_overlay *ov = NULL;
    size_t px = (size_t)w * h;
    int i;

    if (w <= 0 || h <= 0)
        return NULL;

    /* Allocated on first use rather than at boot: with no overlay claimed
     * the pool is 384 KiB of nothing, and boot-to-audio is one of the few
     * things this fork is not allowed to spend. */
    if (!canvas_available() && !canvas_glue_init())
        return NULL;

    for (i = 0; i < MAX_OVERLAYS; i++)
        if (!overlays[i].in_use)
        {
            ov = &overlays[i];
            break;
        }

    if (!ov)
        return NULL;

    /* colour + alpha for the overlay itself, colour only for the backdrop */
    if (with_surface)
    {
        ov->surf_off  = pool_take(px * sizeof(canvas_px));
        ov->alpha_off = pool_take(px);

        if (ov->surf_off == POOL_NONE || ov->alpha_off == POOL_NONE)
            return NULL;
    }
    else
    {
        ov->surf_off = ov->alpha_off = POOL_NONE;
    }

    ov->back_off = pool_take(px * sizeof(canvas_px));
    if (ov->back_off == POOL_NONE)
        return NULL;

    ov->in_use = true;
    ov->w = w;
    ov->h = h;
    ov->covered.x = ov->covered.y = ov->covered.w = ov->covered.h = 0;

    bind_overlay(ov);

    if (with_surface)
        memset(ov->surf.alpha, 0, px);

    return ov;
}

void canvas_overlay_release(struct canvas_overlay *ov)
{
    if (!ov || !ov->in_use)
        return;

    canvas_overlay_restore(ov);
    ov->in_use = false;
}

struct canvas_surface *canvas_overlay_surface(struct canvas_overlay *ov)
{
    if (!ov || !ov->in_use)
        return NULL;

    bind_overlay(ov);
    return &ov->surf;
}

static void add_pending(const struct canvas_rect *r)
{
    struct canvas_rect c = *r;
    struct canvas_rect bound = { 0, 0, LCD_WIDTH, LCD_HEIGHT };

    if (!canvas_rect_intersect(&c, &bound))
        return;

    canvas_rect_union(&pending, &c);
}

bool canvas_overlay_restore(struct canvas_overlay *ov)
{
    struct canvas_rect src;

    if (!ov || !ov->in_use || canvas_rect_empty(&ov->covered))
        return false;

    screen_surface();
    bind_overlay(ov);

    src.x = 0;
    src.y = 0;
    src.w = ov->covered.w;
    src.h = ov->covered.h;

    canvas_blit(&screen, ov->covered.x, ov->covered.y, &ov->backing, &src);
    add_pending(&ov->covered);

    ov->covered.w = ov->covered.h = 0;
    return true;
}

void canvas_overlay_invalidate(struct canvas_overlay *ov)
{
    if (ov)
        ov->covered.w = ov->covered.h = 0;
}

bool canvas_overlay_show(struct canvas_overlay *ov, int x, int y,
                         int w, int h, unsigned opacity)
{
    struct canvas_rect r, src;

    if (!ov || !ov->in_use || w <= 0 || h <= 0 || w > ov->w || h > ov->h)
        return false;

    screen_surface();
    bind_overlay(ov);

    /* Whatever we were covering goes back first. This is the entire
     * mechanism: the vacated pixels are restored by the layer that
     * vacated them, so nothing downstream has to be asked to repaint and
     * a shape that moves cannot smear. */
    canvas_overlay_restore(ov);

    r.x = x; r.y = y; r.w = w; r.h = h;
    {
        struct canvas_rect bound = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
        if (!canvas_rect_intersect(&r, &bound))
            return false;
    }

    /* save the backdrop, then blend over it */
    src.x = r.x - x;
    src.y = r.y - y;
    src.w = r.w;
    src.h = r.h;

    {
        struct canvas_rect screen_src = { r.x, r.y, r.w, r.h };
        canvas_blit(&ov->backing, src.x, src.y, &screen, &screen_src);
    }

    canvas_blend(&screen, r.x, r.y, &ov->surf, &src, opacity);

    ov->covered = r;
    add_pending(&r);

    return true;
}

bool canvas_overlay_capture(struct canvas_overlay *ov, int x, int y,
                            int w, int h)
{
    struct canvas_rect r, src;

    if (!ov || !ov->in_use || w <= 0 || h <= 0 || w > ov->w || h > ov->h)
        return false;

    screen_surface();
    bind_overlay(ov);

    /* What we were covering goes back before anything else happens. The
     * caller is about to paint somewhere else, and the pixels it painted
     * over last time are ours to restore - that is the whole reason a
     * moving overlay drawn this way cannot leave a trail. */
    canvas_overlay_restore(ov);

    r.x = x; r.y = y; r.w = w; r.h = h;
    {
        struct canvas_rect bound = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
        if (!canvas_rect_intersect(&r, &bound))
            return false;
    }

    src.x = r.x - x;
    src.y = r.y - y;
    src.w = r.w;
    src.h = r.h;

    {
        struct canvas_rect screen_src = { r.x, r.y, r.w, r.h };
        canvas_blit(&ov->backing, src.x, src.y, &screen, &screen_src);
    }

    ov->covered = r;
    add_pending(&r);

    return true;
}

bool canvas_present(void)
{
    if (!canvas_available() || canvas_rect_empty(&pending))
        return false;

    lcd_update_rect(pending.x, pending.y, pending.w, pending.h);
    pending.w = pending.h = 0;

    return true;
}

canvas_px *canvas_scratch(int w, int h, size_t *px_out)
{
    size_t need = canvas_blur_scratch_px(w, h);
    size_t bytes = ((need * sizeof(canvas_px)) + 3u) & ~3u;
    char *base;

    if (pool_handle < 0)
        return NULL;

    /* The scratch lives at the tail of the pool and is handed out again
     * every time. A blur is a within-frame operation; nothing may hold on
     * to this across one. */
    if (bytes > pool_size || pool_used + bytes > pool_size)
        return NULL;

    scratch_len = bytes;
    scratch_off = pool_size - bytes;

    base = core_get_data(pool_handle);

    if (px_out)
        *px_out = need;

    return (canvas_px *)(base + scratch_off);
}

#endif /* HAVE_COMPOSITOR */
