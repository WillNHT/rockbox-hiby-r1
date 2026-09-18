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
#ifndef _VIDEO_SURFACE_H_
#define _VIDEO_SURFACE_H_

#include <stdbool.h>
#include "config.h"
#include "rbvideo.h"

struct screen;

/* One moving picture of a fixed size: a stream in librbvideo and what was
 * last drawn of it. The caller says what time it is and where to draw. */
struct video_surface
{
    const struct rbv_api *api;
    struct rbv *v;
    char path[MAX_PATH];
    int w, h, fit, flags, max_fps;
    int drawn_serial;
    bool failed;            /* this path would not open: do not retry */

    /* the loop clock, for clips that follow nothing */
    long loop_ms;
    long loop_tick;
};

/* Opens path at w x h (see enum rbv_fit / rbv_flags), closing whatever
 * the surface had unless it is the same thing. False when it cannot. */
bool video_surface_open(struct video_surface *s, const char *path,
                        int w, int h, int fit, int flags, int max_fps);
void video_surface_close(struct video_surface *s);
static inline bool video_surface_is_open(const struct video_surface *s)
{
    return s->v != NULL;
}

/* Where the clip should be. */
void video_surface_set_clock(struct video_surface *s, long ms, bool playing);
/* Or: let it run on its own clock, which stops while !running. */
void video_surface_run(struct video_surface *s, bool running);

/* Draws the newest frame at x,y of the display's current viewport.
 * With !force only a frame not drawn before is drawn. Returns true when
 * the surface has a picture on screen (drawn now or before). */
bool video_surface_draw(struct video_surface *s, struct screen *display,
                        int x, int y, bool force);

/* The stream ran out (and does not loop). */
bool video_surface_ended(struct video_surface *s);

/* The first frame of a clip or an animated image, w x h, fit as asked,
 * into dst (w pixels a row). For covers. Waits up to a second and a
 * half; false when there is no decoder or no frame. */
bool video_decode_still(const char *path, int w, int h, int fit,
                        fb_data *dst);

#endif
