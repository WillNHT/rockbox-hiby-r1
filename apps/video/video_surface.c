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

#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "screen_access.h"
#include "strmemccpy.h"
#include "video_lib.h"
#include "video_surface.h"

/* A picture nobody has asked for in this long stops being decoded. */
#define IDLE_MS 600

void video_surface_close(struct video_surface *s)
{
    if (s->v)
        s->api->close(s->v);
    s->v = NULL;
    s->path[0] = '\0';
    s->drawn_serial = 0;
    s->failed = false;
}

bool video_surface_open(struct video_surface *s, const char *path,
                        int w, int h, int fit, int flags, int max_fps)
{
    struct rbv_config cfg;
    char osbuf[MAX_PATH];
    const char *ospath;

    if (s->path[0] && !strcmp(s->path, path) && s->w == w && s->h == h &&
        s->fit == fit && s->flags == flags && s->max_fps == max_fps)
        return s->v != NULL;

    video_surface_close(s);
    strmemccpy(s->path, path, sizeof(s->path));
    s->w = w;
    s->h = h;
    s->fit = fit;
    s->flags = flags;
    s->max_fps = max_fps;
    s->loop_ms = 0;
    s->loop_tick = current_tick;

    s->api = video_lib();
    if (!s->api || w <= 0 || h <= 0)
    {
        s->failed = true;
        return false;
    }

    ospath = video_os_path(path, osbuf, sizeof(osbuf));
    if (!ospath)
    {
        s->failed = true;
        return false;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.size = sizeof(cfg);
    cfg.out_w = w;
    cfg.out_h = h;
    cfg.fit = fit;
    cfg.flags = flags;
    cfg.max_fps = max_fps;
    cfg.idle_ms = IDLE_MS;
    s->v = s->api->open(ospath, &cfg);
    s->failed = (s->v == NULL);
    return s->v != NULL;
}

void video_surface_set_clock(struct video_surface *s, long ms, bool playing)
{
    if (s->v)
        s->api->set_clock(s->v, ms, playing);
}

void video_surface_run(struct video_surface *s, bool running)
{
    long now = current_tick;
    long dt = now - s->loop_tick;
    s->loop_tick = now;
    /* A screen that was away does not fast-forward the clip. */
    if (running && dt > 0 && dt < HZ)
        s->loop_ms += dt * 1000 / HZ;
    video_surface_set_clock(s, s->loop_ms, running);
}

bool video_surface_draw(struct video_surface *s, struct screen *display,
                        int x, int y, bool force)
{
    const uint16_t *px;
    int serial;

    if (!s->v)
        return false;
    serial = s->api->lock_frame(s->v, &px, NULL);
    if (serial > 0 && px && (force || serial != s->drawn_serial))
    {
#if LCD_DEPTH == 16 && LCD_PIXELFORMAT == RGB565
        display->bitmap_part((const fb_data *)px, 0, 0,
                             STRIDE_MAIN(s->w, s->h), x, y, s->w, s->h);
#endif
        s->drawn_serial = serial;
    }
    s->api->unlock_frame(s->v);
    if (serial < 0)
    {
        /* Broken file: keep the path, so it is not opened again. */
        s->api->close(s->v);
        s->v = NULL;
        s->drawn_serial = 0;
        s->failed = true;
        return false;
    }
    return s->drawn_serial > 0;
}

bool video_decode_still(const char *path, int w, int h, int fit,
                        fb_data *dst)
{
    struct video_surface s;
    bool ok = false;
    long until = current_tick + HZ * 3 / 2;

    memset(&s, 0, sizeof(s));
    if (!video_surface_open(&s, path, w, h, fit, 0, 0))
        return false;
    s.api->set_clock(s.v, 0, 0);
    while (TIME_BEFORE(current_tick, until))
    {
        int64_t pts;
        int r = s.api->get_frame(s.v, (uint16_t *)dst, w, &pts);
        if (r != 0)
        {
            ok = (r > 0);
            break;
        }
        sleep(HZ / 50);
    }
    video_surface_close(&s);
    return ok;
}

bool video_surface_ended(struct video_surface *s)
{
    return s->v && s->api->status(s->v) == RBV_EOF;
}
