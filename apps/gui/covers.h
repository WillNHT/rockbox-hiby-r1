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
#ifndef _GUI_COVERS_H_
#define _GUI_COVERS_H_

/* Cover pictures for anything that has a path: an image file, or an audio
 * file whose embedded art or folder art is the cover. Decoded on the UI
 * thread and kept in a small LRU cache. See covers.c. */

#include "config.h"

#ifdef HAVE_COVER_VIEWS
#include <stdbool.h>
#include "screen_access.h"

/* The largest square a cover is decoded to. */
#define COVER_MAX_SIZE 320

/* Once, at boot, so the cache does not come out of the audio buffer. */
bool covers_init(void);

/* Draw the cover for path centred in a size x size square at x,y of the
 * current viewport. False, with nothing drawn, when there is none. */
bool cover_draw(struct screen *d, const char *path, int x, int y, int size);
#ifdef HAVE_VIDEO
/* The same, but an animated .gif or .webp cover moves: each call draws
 * its current frame, and a still cover only when full. Returns 2 for an
 * animated cover, 1 for a still one drawn, 0 for nothing drawn. */
int cover_draw_animated(struct screen *d, const char *path,
                        int x, int y, int size, bool full);
#endif

/* A plain tile for items without a cover: a filled square with the first
 * letter of text in it. */
void cover_draw_placeholder(struct screen *d, const char *text,
                            int x, int y, int size);

/* Forget everything decoded for path (a cover file changed). NULL: all. */
void covers_forget(const char *path);
#endif

#endif /* _GUI_COVERS_H_ */
