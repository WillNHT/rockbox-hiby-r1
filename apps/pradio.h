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

/* Pseudo-radio: long recordings played from the middle, the way a radio
 * you tune into is already halfway through whatever it is playing. Not
 * the FM tuner - this device has none. */

/* The station list. Returns a GO_TO_* value. */
int  pradio_screen(void);

/* Whether a track belongs to the radio folder. */
bool pradio_is_station_track(const char *path);
/* Whether a radio track is what is playing right now. */
bool pradio_playing(void);

#endif /* _PRADIO_H_ */
