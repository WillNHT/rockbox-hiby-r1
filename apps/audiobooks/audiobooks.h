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
#ifndef _AUDIOBOOKS_H_
#define _AUDIOBOOKS_H_

#include <stdbool.h>

/* The Audiobooks library: the main menu entry, what it remembers of each
 * book, and what changes while a book plays. */

void audiobooks_init(void);
/* The screens' own start-up part (apps/audiobooks/ab_ui.c). */
void audiobooks_ui_init(void);

/* The main menu screen. Returns a GO_TO_* value. */
int  audiobooks_screen(void);

/* Rescans at startup when the folder changed (if enabled). */
void audiobooks_startup_scan(void);

/* Whether a track belongs to the audiobooks folder. */
bool audiobooks_is_book(const char *path);

/* Called by the WPS right before it unpauses: rewinds a book that was
 * paused long enough. */
void audiobooks_before_resume(void);

/* --- used by the library screens -------------------------------------- */

/* Starts book (an index into the library) where it was left, or from the
 * beginning. Returns true when playback started. */
bool audiobooks_play(int book, bool start_over);
/* Starts book at file, ms into it. */
bool audiobooks_play_at(int book, int file, unsigned long elapsed);

/* Lets the user pick the WPS used while a book plays. */
int  audiobooks_choose_wps(void);

/* Sleep timer: minutes, 0 = off, -1 = at the end of the chapter. */
void audiobooks_set_sleep(int minutes);
bool audiobooks_sleep_at_chapter_end(void);

/* Serialises library access between the screens and the playback
 * watcher. Recursive. */
void ab_lock(void);
void ab_unlock(void);

#endif /* _AUDIOBOOKS_H_ */
