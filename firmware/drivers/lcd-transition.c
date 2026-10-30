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

/* Screen transitions.
 *
 * Nothing that draws a screen knows it is being animated. The caller says
 * "the screen is about to change" and goes on drawing the next one exactly
 * as before; here the old frame is kept, the presents that would have shown
 * the new one half-drawn are held back, and once the thread that asked goes
 * to wait - for a key, or in sleep() - the new frame is complete and the
 * two are animated into each other.
 *
 * Waiting is the signal because a screen is never finished at any earlier
 * point we could name: a list draws its title, then its items, then its
 * scrollbar, each with its own update, and only the wait that follows says
 * that was all of it.
 *
 * The animation runs in the caller's thread and blocks it for its length.
 * A few hundred milliseconds at the moment the user asked for a new screen
 * is the transition, not a stall in front of it.
 *
 * The frames are never written anywhere. The two screens are taken
 * composed - framebuffer and surfaces - and while the run lasts every
 * rectangle the driver copies to the panel gets the frame instead of the
 * framebuffer (lcd_transition_paint(), from lcd-layers.c), with the
 * layers still painted live on top of it: the stick overlay follows the
 * thumb through the animation instead of freezing in it. */

#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "lcd.h"
#include "lcd-layers.h"
#include "lcd-transition.h"

#define NPIX (LCD_WIDTH * LCD_HEIGHT)

/* A transition armed but never run - the caller never waited - must not
 * hold the screen back for longer than this.
 *
 * It is also how long a slow screen gets to finish drawing, because the
 * give-up runs from a present halfway down it. At half a second a WPS
 * whose new cover had to be scaled and blurred first could run out of time
 * with only the picture drawn: the animation went to a frame with no text,
 * no meter and no bars, and the chrome popped in once it had finished. */
#define STALE_TICKS (HZ * 3 / 2)

/* Cascade: bands, and how much of the run the last band starts after. */
#define CASCADE_BANDS  12
#define CASCADE_SPREAD 512      /* of 1024 */

static fb_data from_px[NPIX];
static fb_data to_px[NPIX];

static bool armed;
static bool running;
static int t_p;                 /* eased progress of the frame showing */
static enum lcd_transition t_type;
static int t_dir;
static int t_ticks;
static long armed_tick;
static unsigned int owner;

extern struct frame_buffer_t lcd_framebuffer_default;

static bool geometry_ok(void)
{
    return LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride) == LCD_WIDTH;
}

void lcd_transition_arm(enum lcd_transition type, int dir, int duration_ms)
{
    if (running || type <= LCD_TRANSITION_NONE || type >= LCD_TRANSITION_COUNT
        || duration_ms <= 0 || !geometry_ok())
        return;

    if (!armed)
    {
        /* The old frame as it was on the panel, surfaces and all - an old
         * one without them made whatever a skin keeps in them vanish at
         * the start of the animation and pop back at its end. */
        lcd_layers_snapshot(from_px);
        armed = true;
    }
    t_type = type;
    t_dir = dir < 0 ? -1 : 1;
    t_ticks = (duration_ms * HZ + 999) / 1000;
    if (t_ticks < 1)
        t_ticks = 1;
    armed_tick = current_tick;
    owner = thread_self();
}

void lcd_transition_cancel(void)
{
    if (!armed)
        return;
    armed = false;
    lcd_update();
}

#if LCD_PIXELFORMAT == RGB565
/* a over b, w in 0..32 */
static inline fb_data blend(fb_data a, fb_data b, unsigned w)
{
    uint32_t x = b, y = a;
    x = (x | (x << 16)) & 0x07e0f81f;
    y = (y | (y << 16)) & 0x07e0f81f;
    x += ((y - x) * w) >> 5;
    x &= 0x07e0f81f;
    return (fb_data)(x | (x >> 16));
}
#else
static inline fb_data blend(fb_data a, fb_data b, unsigned w)
{
    return w >= 16 ? a : b;
}
#endif

/* 0..1024 in, eased 0..1024 out: a cubic ease-out, fast then settling. */
static int ease(int t)
{
    int u = 1024 - t;
    return 1024 - (int)(((long long)u * u * u) >> 20);
}

/* Pixel x of a row of `to` moved right by s (negative: left) over the
 * same row of `from`, with `to` at weight w. */
static inline fb_data shifted_px(const fb_data *to, const fb_data *from,
                                 int x, int s, unsigned w)
{
    int tx = x - s;
    if (tx < 0 || tx >= LCD_WIDTH)
        return from[x];
    return blend(to[tx], from[x], w);
}

/* Columns x0..x1 of row y of the frame at eased progress p (0..1024),
 * into d. Per row and range rather than per screen, so it can be painted
 * straight into whatever the driver is copying to the panel. */
static void frame_row(int p, int y, fb_data *d, int x0, int x1)
{
    const int W = LCD_WIDTH, H = LCD_HEIGHT;
    const fb_data *f = from_px + y * W, *t = to_px + y * W;
    int x;

    switch (t_type)
    {
    case LCD_TRANSITION_FADE:
    {
        unsigned w = (unsigned)(p * 32 / 1024);
        for (x = x0; x < x1; x++)
            d[x - x0] = blend(t[x], f[x], w);
        break;
    }
    case LCD_TRANSITION_SLIDE:
    {
        /* forward: in from the right edge */
        int s = t_dir * (W - W * p / 1024);
        for (x = x0; x < x1; x++)
            d[x - x0] = shifted_px(t, f, x, s, 32);
        break;
    }
    case LCD_TRANSITION_PUSH:
    {
        int off = W * p / 1024;         /* how far the new one is in */
        int n = W - off;                /* old pixels still showing */
        for (x = x0; x < x1; x++)
        {
            if (t_dir > 0)
                d[x - x0] = x < n ? f[x + off] : t[x - n];
            else
                d[x - x0] = x < off ? t[x + n] : f[x - off];
        }
        break;
    }
    case LCD_TRANSITION_WIPE:
    {
        /* A soft edge sweeping across, left to right going forward: the
         * new screen is drawn on behind it, nothing moves. */
        const int soft = W / 10;
        int edge = (W + soft) * p / 1024 - soft;    /* new up to here */
        for (x = x0; x < x1; x++)
        {
            int k = t_dir > 0 ? x : W - 1 - x;
            int w = k < edge ? 32 : k >= edge + soft ? 0
                                  : 32 - (k - edge) * 32 / soft;
            d[x - x0] = blend(t[x], f[x], (unsigned)w);
        }
        break;
    }
    case LCD_TRANSITION_DISSOLVE:
    {
        /* Each pixel fades on its own, starting at a time an ordered
         * dither gives it, so the new screen grains in rather than
         * washing over. */
        static const uint8_t bayer[4][4] =
            { { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
              { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
        for (x = x0; x < x1; x++)
        {
            int w = (p - bayer[y & 3][x & 3] * 48) * 32 / 256;
            d[x - x0] = blend(t[x], f[x],
                              (unsigned)(w < 0 ? 0 : MIN(w, 32)));
        }
        break;
    }
    case LCD_TRANSITION_BLINDS:
    {
        /* Slats: every band opens top down going forward, bottom up going
         * back, all at once, with its leading row half blended. */
        const int slat = H / 16;
        int open = slat * p / 1024;
        int k = t_dir > 0 ? y % slat : slat - 1 - y % slat;
        unsigned w = k < open ? 32 : k == open ? 16 : 0;
        for (x = x0; x < x1; x++)
            d[x - x0] = blend(t[x], f[x], w);
        break;
    }
    case LCD_TRANSITION_CASCADE:
    default:
    {
        /* Each band is its own short slide-and-fade, started a little after
         * the one before it, so the rows of a list - or the title, the art
         * and the text of a WPS - arrive one after another. Forward runs
         * top to bottom, back bottom to top. */
        const int band_h = (H + CASCADE_BANDS - 1) / CASCADE_BANDS;
        const int reach = W / 5;
        int b = y / band_h;
        int order = t_dir > 0 ? b : CASCADE_BANDS - 1 - b;
        int start = CASCADE_SPREAD * order / (CASCADE_BANDS - 1);
        int local = (p - start) * 1024 / (1024 - CASCADE_SPREAD);
        int s;
        unsigned w;
        if (local < 0) local = 0;
        if (local > 1024) local = 1024;
        local = ease(local);
        s = t_dir * (reach - reach * local / 1024);
        w = (unsigned)(local * 32 / 1024);
        for (x = x0; x < x1; x++)
            d[x - x0] = shifted_px(t, f, x, s, w);
        break;
    }
    }
}

bool lcd_transition_paint(fb_data *dst, int dst_stride,
                          int x, int y, int w, int h)
{
    int row;
    if (!running)
        return false;
    for (row = 0; row < h; row++)
        frame_row(t_p, y + row, dst + (size_t)row * dst_stride, x, x + w);
    return true;
}

static void run(void)
{
    long start, now;

    armed = false;
    lcd_layers_snapshot(to_px);

    if (memcmp(to_px, from_px, sizeof(to_px)))
    {
        /* Frames are timed off the clock, not counted, so a slow frame
         * shortens the run instead of stretching it. The first frame goes
         * out at once: the key press that asked for this wants an answer
         * in the same instant. */
        start = current_tick;
        now = start;
        t_p = 0;
        running = true;
        do
        {
            int t = (int)((now - start) * 1024 / t_ticks);
            if (t > 1024)
                break;
            /* The cascade eases each band itself. */
            t_p = t_type == LCD_TRANSITION_CASCADE ? t : ease(t);
            lcd_update_base();
            yield();
            now = current_tick;
        } while (now - start < t_ticks);
        running = false;
    }

    lcd_update_base();
}

void lcd_transition_idle(void)
{
    if (armed && !running && thread_self() == owner)
        run();
}

bool lcd_transition_hold(void)
{
    if (!armed || running)
        return false;
    if (TIME_AFTER(current_tick, armed_tick + STALE_TICKS))
    {
        if (thread_self() != owner)
        {
            armed = false;  /* not ours to run; just stop holding */
            return false;
        }
        run();
        /* run() has presented everything; the caller has nothing to add */
    }
    return true;
}
