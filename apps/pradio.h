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
#ifndef _PRADIO_H_
#define _PRADIO_H_

#include <stdbool.h>
#include <stddef.h>

/* Pseudo-radio: long recordings played from the middle, the way a radio
 * you tune into is already halfway through whatever it is playing. Not
 * the FM tuner - this device has none. */

/* The station list. Returns a GO_TO_* value. */
int  pradio_screen(void);

/* Whether a track belongs to the radio folder. */
bool pradio_is_station_track(const char *path);
/* Whether a radio track is what is playing right now. */
bool pradio_playing(void);
/* User-initiated prev/next while tuned in: retunes to another station
 * instead of skipping a track. Returns false (does nothing) when the radio
 * isn't what's playing, so the caller falls back to its normal skip. */
bool pradio_skip(void);
/* Pause/unpause bookkeeping: a station keeps playing while you are away, so
 * resuming after a long pause drops in further along instead of where it
 * stopped. No-op unless the radio is what's playing. */
void pradio_pause(bool paused);
/* The station folder a radio track belongs to, trailing slash included, at
 * whatever depth the file is. False when the path isn't a station track. */
bool pradio_station_dir(const char *path, char *buf, size_t size);
/* The station's folder name on its own - what a radio skin shows instead of
 * an album. False when the path isn't a station track. */
bool pradio_station_name(const char *path, char *buf, size_t size);
/* The set going off. Call while the station is still playing. */
void pradio_leaving(void);
/* Called from the WPS loop: every hour or two, something drifts past on
 * the band. Cheap and a no-op unless a station is playing. */
void pradio_ambience_tick(void);

#endif /* _PRADIO_H_ */
