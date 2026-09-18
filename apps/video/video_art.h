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
#ifndef _VIDEO_ART_H_
#define _VIDEO_ART_H_

#include <stdbool.h>
#include <stddef.h>

/* The moving picture a WPS shows for the playing track (%Cv): its music
 * video, when there is one and the user asked for it, else its animated
 * cover (canvas), else nothing.
 *
 * A music video has no sound of its own. It follows the track's clock,
 * shifted by its sidecar's offset and the output latency, so switching
 * between it and the cover changes only what is drawn - see
 * docs/mods/video.md. */

enum video_art_kind
{
    VIDEO_ART_NONE = 0,
    VIDEO_ART_CANVAS,
    VIDEO_ART_MUSIC,
};

struct screen;

/* What %Cv would show for the playing track right now: NONE also when
 * the music video is outside its part of the track. */
enum video_art_kind video_art_kind(void);

/* Whether the playing track has a music video (for the context menu), and
 * switching between it and the cover. */
bool video_art_track_has_music_video(void);
void video_art_toggle_music_video(void);

/* %Cv: draws the picture into w x h at x,y of the current viewport. full:
 * redraw even when there is no new frame. Returns whether a picture is
 * on screen. */
bool video_art_draw(struct screen *display, int x, int y, int w, int h,
                    int fit, bool full);

/* %Cf: a clip that loops on its own clock, while it is being drawn. */
bool video_clip_draw(struct screen *display, const char *path,
                     int x, int y, int w, int h, int fit, bool full);

/* Close the streams (the WPS is gone, or settings changed). */
void video_art_release(void);

/* Finds "<base><suffix>.<video ext>" for a file path: the sidecar lookup
 * shared with the screensaver and the covers. */
bool video_find_sidecar(const char *path, const char *suffix,
                        bool images, char *buf, size_t size);

#endif
