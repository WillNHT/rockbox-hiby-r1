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
#ifndef _SCREENSAVER_H_
#define _SCREENSAVER_H_

#include <stdbool.h>
#include "queue.h"

/* Screensavers are skins: files in /.rockbox/wps with the .ss extension,
 * written like a WPS, their pictures in the folder of the same name. One
 * is chosen for battery and one for charging (Settings > Video >
 * Screensaver), and it starts after that long without input, from
 * whatever list or screen is up. Any key or touch ends it; the input that
 * ends it does nothing else. */

enum saver_keep_on
{
    SAVER_KEEP_NEVER = 0,   /* the backlight timers apply as usual */
    SAVER_KEEP_CHARGING,    /* always on while charging */
    SAVER_KEEP_ALWAYS,
};

/* Posted to the button queue when the idle time has passed; any screen
 * that hands unknown events to default_event_handler() shows it. */
#define SYS_SCREENSAVER MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 6)

void screensaver_init(void);
/* Some input happened: start counting again. */
void screensaver_note_input(void);
/* Every button read, before anything acts on it. True when it belongs to
 * the screensaver (it ends it, or it is the rest of the press that did)
 * and must go no further. */
bool screensaver_filter_input(long button);
/* Show the screensaver now (preview: even if the timer is off). Returns
 * whether one was shown. */
bool screensaver_run(bool preview);
bool screensaver_active(void);
/* The skin to load (a name in WPS_DIR, no extension), or NULL. */
const char *screensaver_skin_name(void);
/* The built-in one, for when the file is missing. */
char *screensaver_default_skin(int screen);

#endif
