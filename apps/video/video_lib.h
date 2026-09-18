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
#ifndef _VIDEO_LIB_H_
#define _VIDEO_LIB_H_

#include <stdbool.h>
#include <stddef.h>
#include "rbvideo.h"

/* Where librbvideo.so is looked for. */
#define VIDEO_LIB_PATH ROCKBOX_DIR "/lib/librbvideo.so"

/* The decoder, loaded the first time it is asked for; NULL when it is not
 * installed or is the wrong version. Cheap after the first call. */
const struct rbv_api *video_lib(void);

/* The operating system's name for a Rockbox path, which is what the
 * decoder opens. NULL when there is none. */
const char *video_os_path(const char *path, char *buf, size_t size);

/* Why video_lib() returned NULL, for a splash. */
const char *video_lib_error(void);

/* Is `path` something the decoder might play (by its extension)? */
bool video_is_video_file(const char *path);
bool video_is_image_anim(const char *path);     /* .gif, .webp, .apng */

/* Does this file hold a picture the video player should show? Only asks
 * the decoder for names that could be video, so a song costs nothing. */
bool video_has_picture(const char *path);

/* A clip that belongs to a track (<track>.video.mp4, .canvas.mp4) rather
 * than music of its own: kept out of the browser and the database. Works
 * on any platform, it only looks at the name. */
bool video_is_sidecar(const char *name);

#endif
