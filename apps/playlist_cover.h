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
#ifndef _PLAYLIST_COVER_H_
#define _PLAYLIST_COVER_H_

#include <stdbool.h>
#include <stddef.h>

/* The cover of a saved playlist (.m3u / .m3u8), always shown square.
 *
 * Found next to the playlist, with the same name: "Road Trip.m3u8" takes
 * "Road Trip.jpg", ".jpeg", ".png" - or ".gif" / ".webp", which stay
 * unshown until animated covers land (issue #19). Without one, the first
 * track in the playlist stands in, and its own art is used.
 *
 * Dynamic playlists (the database, the current queue) have no cover.
 *
 * Writes the image or track path to buf. False when there is neither. */
bool playlist_cover_find(const char *playlist, char *buf, size_t size);

/* The playlist the current playback came from, or false if it was not a
 * saved playlist file. */
bool playlist_cover_current(char *buf, size_t size);

#endif /* _PLAYLIST_COVER_H_ */
