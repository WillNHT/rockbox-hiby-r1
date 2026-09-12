/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2016 Amaury Pouly
 * Copyright (C) 2019 Roman Stolyarov
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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include "lcd.h"
#include "lcd-target.h"
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"

/* Page flipping is opted into per target, in the target config header,
 * because it costs a second framebuffer plane and needs a driver that can
 * actually pan. Everything below degrades to the historical single-plane
 * behaviour when HAVE_FB_DOUBLEBUF is not defined. */
#ifdef HAVE_FB_DOUBLEBUF
#define FB_DOUBLEBUF
#endif

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, uint32_t)
#endif

/* Pixels, not bytes: the plane offset is applied to an fb_data pointer.
 * FRAMEBUFFER_SIZE is a byte count and using it here doubled the offset. */
#define FB_PLANE_PIXELS (LCD_WIDTH * LCD_HEIGHT)

static int fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
fb_data *framebuffer = NULL; /* global variable, see lcd-target.h */

#ifdef FB_DOUBLEBUF
static int fb_plane;        /* plane we are drawing into */
static int vsync_ok = 1;    /* cleared if the driver has no FBIO_WAITFORVSYNC */
int doublebuf;

/* Region of each plane that no longer matches the Rockbox framebuffer.
 * A partial update only touches one plane, so the other one accumulates
 * what it missed and catches up the next time it is the back buffer.
 * That is the whole of the stale-plane problem with naive double
 * buffering, and the reason partial updates can stay partial here. */
struct fb_dirty { int x1, y1, x2, y2; };
static struct fb_dirty plane_dirty[2];

static void dirty_add(struct fb_dirty *d, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (d->x2 <= d->x1)
    {
        d->x1 = x; d->y1 = y; d->x2 = x + w; d->y2 = y + h;
        return;
    }
    if (x < d->x1) d->x1 = x;
    if (y < d->y1) d->y1 = y;
    if (x + w > d->x2) d->x2 = x + w;
    if (y + h > d->y2) d->y2 = y + h;
}

static inline void dirty_clear(struct fb_dirty *d)
{
    d->x1 = d->y1 = d->x2 = d->y2 = 0;
}
#else
#define fb_plane 0
#endif

#ifdef HAVE_LCD_PRESENT_STATS
static struct lcd_present_stats stats;

static inline unsigned long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000ul + (unsigned long)(ts.tv_nsec / 1000);
}

static inline void stat_time(unsigned long *total, unsigned long *max,
                             unsigned long us)
{
    *total += us;
    if (us > *max)
        *max = us;
}

void lcd_get_present_stats(struct lcd_present_stats *out)
{
    *out = stats;
}

/* Page flipping on or off at runtime, so "is the flicker this?" is one
 * menu entry and a gesture rather than a rebuild. Turning it off parks
 * plane 0 on the display and writes only ever to that one. */
void lcd_set_doublebuf(bool on)
{
#ifdef FB_DOUBLEBUF
    if (finfo.smem_len < 2 * FRAMEBUFFER_SIZE ||
        vinfo.yres_virtual < vinfo.yres * 2)
        return;

    if (doublebuf == (on ? 1 : 0))
        return;

    doublebuf = on ? 1 : 0;
    stats.doublebuf = doublebuf;

    if (!doublebuf)
    {
        fb_plane = 0;
        vinfo.yoffset = 0;
        vinfo.activate = FB_ACTIVATE_NOW;
        ioctl(fd, FBIOPAN_DISPLAY, &vinfo);
    }

    /* both planes are now of unknown freshness */
    dirty_add(&plane_dirty[0], 0, 0, LCD_WIDTH, LCD_HEIGHT);
    dirty_add(&plane_dirty[1], 0, 0, LCD_WIDTH, LCD_HEIGHT);
#else
    (void)on;
#endif
}

bool lcd_doublebuf_enabled(void)
{
#ifdef FB_DOUBLEBUF
    return doublebuf != 0;
#else
    return false;
#endif
}

void lcd_reset_present_stats(void)
{
    struct lcd_present_stats geom = stats;
    memset(&stats, 0, sizeof(stats));
    /* geometry is not a counter */
    stats.doublebuf       = geom.doublebuf;
    stats.vsync           = geom.vsync;
    stats.fb_xres         = geom.fb_xres;
    stats.fb_yres         = geom.fb_yres;
    stats.fb_yres_virtual = geom.fb_yres_virtual;
    stats.fb_bpp          = geom.fb_bpp;
    stats.fb_line_length  = geom.fb_line_length;
    stats.fb_smem_len     = geom.fb_smem_len;
}
#define STAT(x) do { x; } while (0)
#else
#define STAT(x) do { } while (0)
#endif

static void redraw(void)
{
#ifdef HAVE_LCD_PRESENT_STATS
    unsigned long t0 = now_us();
#endif

#ifdef FB_DOUBLEBUF
    if (doublebuf) {
        vinfo.yoffset = fb_plane * LCD_HEIGHT;
        vinfo.activate = FB_ACTIVATE_VBL;

        /* Flip on a vertical blank so a half-drawn plane is never scanned
         * out. FB_ACTIVATE_VBL alone is advisory and plenty of drivers
         * ignore it, hence the explicit wait as well. */
        if (vsync_ok) {
            uint32_t crtc = 0;
            if (ioctl(fd, FBIO_WAITFORVSYNC, &crtc) < 0) {
                vsync_ok = 0;  /* unsupported - stop paying for the ioctl */
                STAT(stats.vsync = 0);
            } else {
                STAT(stats.vsync_waits++);
            }
        }
    }
#endif

    ioctl(fd, FBIOPAN_DISPLAY, &vinfo);

#ifdef FB_DOUBLEBUF
    if (doublebuf)
        fb_plane ^= 1;
#endif

#ifdef HAVE_LCD_PRESENT_STATS
    stat_time(&stats.present_us_total, &stats.present_us_max, now_us() - t0);
#endif
}

extern void lcd_copy_buffer_rect(fb_data *dst, const fb_data *src,
                                 int width, int height);

/* The main framebuffer, as opposed to whatever buffer the current viewport
 * happens to own. Catch-up copies must come from this one: a viewport with
 * its own buffer is smaller than the screen, so reading a region outside
 * the rectangle the caller asked for would read past the end of it. */
extern struct frame_buffer_t lcd_framebuffer_default;

/* get_address_fn resolves against the *current viewport*, not against the
 * buffer it is handed, so the main buffer has to be addressed by hand. */
static inline fb_data *main_fb_addr(int x, int y)
{
    return lcd_framebuffer_default.fb_ptr +
           (size_t)y * LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride) + x;
}

/* Copy a rectangle into one plane. src is already the top-left corner. */
static void copy_to_plane(int plane, const fb_data *src,
                          int x, int y, int width, int height)
{
    fb_data *dst = LCD_FRAMEBUF_ADDR(x, y) + plane * FB_PLANE_PIXELS;

    if (x == 0 && width == LCD_WIDTH)
    {
        /* Full width - the rows are contiguous, copy as one line */
        lcd_copy_buffer_rect(dst, src, LCD_WIDTH * height, 1);
    }
    else
    {
        lcd_copy_buffer_rect(dst, src, width, height);
    }

    STAT(stats.px_copied += (unsigned long long)width * height);
}

void lcd_init_device(void)
{
    const char * const fb_dev = "/dev/fb0";
    fd = open(fb_dev, O_RDWR | O_CLOEXEC);
    if(fd < 0)
    {
        panicf("Cannot open framebuffer: %s\n", fb_dev);
    }

    /* get fixed information */
    if(ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        panicf("Cannot read framebuffer fixed information");
    }

    if (finfo.smem_len < FRAMEBUFFER_SIZE)
        panicf("FRAMEBUFFER_SIZE too large for hardware? (%u vs %u)", FRAMEBUFFER_SIZE, finfo.smem_len);

    /* get variable information */
    if(ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        panicf("Cannot read framebuffer variable information");
    }

    /* Make sure we match our desired geometry and bitdepth */
    if (vinfo.bits_per_pixel != LCD_DEPTH || vinfo.xres != LCD_WIDTH || vinfo.yres != LCD_HEIGHT) {
        vinfo.bits_per_pixel = LCD_DEPTH;
        vinfo.xres = LCD_WIDTH;
        vinfo.yres = LCD_HEIGHT;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo)) {
            panicf("Cannot set framebuffer to %dx%dx%d",
               vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        }
    }

#ifdef FB_DOUBLEBUF
    /* Two planes need twice the mapping and a virtual resolution the driver
     * is willing to give us. Either can fail, and neither is fatal: we fall
     * back to single-plane, which is exactly what shipped before. */
    if (finfo.smem_len >= 2 * FRAMEBUFFER_SIZE)
    {
        struct fb_var_screeninfo tryinfo = vinfo;
        tryinfo.xres_virtual = vinfo.xres;
        tryinfo.yres_virtual = vinfo.yres * 2;
        tryinfo.yoffset = 0;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &tryinfo) == 0 &&
            ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            vinfo.yres_virtual >= vinfo.yres * 2)
        {
            doublebuf = 1;
        }
    }
    fb_plane = 0;
    dirty_clear(&plane_dirty[0]);
    dirty_clear(&plane_dirty[1]);
#endif

    /* line_length and smem_len can change with the mode - re-read them */
    ioctl(fd, FBIOGET_FSCREENINFO, &finfo);

    /* map framebuffer */
    framebuffer = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if((void *)framebuffer == MAP_FAILED)
    {
        panicf("Cannot map framebuffer");
    }

    memset(framebuffer, 0, finfo.smem_len);

#ifdef HAVE_LCD_PRESENT_STATS
    memset(&stats, 0, sizeof(stats));
#ifdef FB_DOUBLEBUF
    stats.doublebuf = doublebuf;
    stats.vsync = 1;
#endif
    stats.fb_xres         = vinfo.xres;
    stats.fb_yres         = vinfo.yres;
    stats.fb_yres_virtual = vinfo.yres_virtual;
    stats.fb_bpp          = vinfo.bits_per_pixel;
    stats.fb_line_length  = finfo.line_length;
    stats.fb_smem_len     = finfo.smem_len;
#endif

#ifdef HAVE_LCD_ENABLE
    lcd_set_active(true);
#endif
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void)
{
    munmap(framebuffer, finfo.smem_len);
    framebuffer = NULL;
    close(fd);
    fd = -1;
}
#endif

#ifdef HAVE_LCD_ENABLE
void lcd_enable(bool on)
{
    if (fd < 0) return;

    if (lcd_active() == on)
        return;

    lcd_set_active(on);

    if (on) {
        send_event(LCD_EVENT_ACTIVATION, NULL);
        ioctl(fd, FB_BLANK_UNBLANK);
    } else {
        memset(framebuffer, 0, finfo.smem_len);
#ifdef FB_DOUBLEBUF
        /* both planes are black now and owe the whole screen */
        dirty_add(&plane_dirty[0], 0, 0, LCD_WIDTH, LCD_HEIGHT);
        dirty_add(&plane_dirty[1], 0, 0, LCD_WIDTH, LCD_HEIGHT);
#endif
        redraw();
        ioctl(fd, FB_BLANK_POWERDOWN);
    }
}
#endif

void lcd_update(void)
{
    if (fd < 0) return;

#ifdef HAVE_LCD_ENABLE
    if (!lcd_active())
        return;
#endif

#ifdef HAVE_LCD_PRESENT_STATS
    unsigned long t0 = now_us();
#endif

    copy_to_plane(fb_plane, FBADDR(0, 0), 0, 0, LCD_WIDTH, LCD_HEIGHT);

#ifdef FB_DOUBLEBUF
    /* Both planes, not just this one.
     *
     * Leaving the other plane owing a whole screen was correct and slow:
     * the very next partial update had to replay the entire frame before
     * it could draw its rectangle, so in the ordinary alternating pattern
     * every second "partial" update was a full one. Paying the second
     * 750 KiB here instead costs the same copy once, on an update that is
     * already full-screen, and leaves partial updates genuinely partial.
     * Safe because the plane being written is the one *not* on display. */
    if (doublebuf)
        copy_to_plane(!fb_plane, FBADDR(0, 0), 0, 0, LCD_WIDTH, LCD_HEIGHT);

    dirty_clear(&plane_dirty[0]);
    dirty_clear(&plane_dirty[1]);
#endif

#ifdef HAVE_LCD_PRESENT_STATS
    stat_time(&stats.copy_us_total, &stats.copy_us_max, now_us() - t0);
    stats.frames++;
    stats.full_frames++;
#endif

    redraw();
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (fd < 0) return;

#ifdef HAVE_LCD_ENABLE
    if (!lcd_active())
        return;
#endif

    /* LCD_OPTIMIZED_UPDATE_RECT means the clipping is ours to do */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > LCD_WIDTH) width = LCD_WIDTH - x;
    if (y + height > LCD_HEIGHT) height = LCD_HEIGHT - y;
    if (width <= 0 || height <= 0)
        return;

#ifdef HAVE_LCD_PRESENT_STATS
    unsigned long t0 = now_us();
#endif

#ifdef FB_DOUBLEBUF
    if (doublebuf)
    {
        int p = fb_plane;
        struct fb_dirty d;

        /* both planes now differ from the Rockbox framebuffer here */
        dirty_add(&plane_dirty[0], x, y, width, height);
        dirty_add(&plane_dirty[1], x, y, width, height);

        d = plane_dirty[p];
        dirty_clear(&plane_dirty[p]);

        /* Everything this plane slept through, replayed out of the main
         * framebuffer before the caller rectangle goes on top. Content a
         * viewport drew into a buffer of its own is not in there and is
         * the one thing this cannot replay - rare, and it costs one frame
         * of staleness rather than permanent loss. */
        if (d.x1 != x || d.y1 != y || d.x2 != x + width || d.y2 != y + height)
        {
            int cx = d.x1, cy = d.y1, cw = d.x2 - d.x1, ch = d.y2 - d.y1;

            /* A box grown to most of the screen is cheaper as one
             * contiguous copy than as several hundred short strided ones. */
            if (cw * ch >= (LCD_WIDTH * LCD_HEIGHT * 3) / 4)
            {
                cx = 0; cy = 0; cw = LCD_WIDTH; ch = LCD_HEIGHT;
                STAT(stats.promoted_frames++);
            }

            copy_to_plane(p, main_fb_addr(cx, cy), cx, cy, cw, ch);
        }
    }
#endif

    copy_to_plane(fb_plane, FBADDR(x, y), x, y, width, height);

#ifdef HAVE_LCD_PRESENT_STATS
    stat_time(&stats.copy_us_total, &stats.copy_us_max, now_us() - t0);
    stats.frames++;
    stats.partial_frames++;
#endif

    redraw();
}
