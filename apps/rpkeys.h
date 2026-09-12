/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket global keys - the physical buttons mean one thing everywhere.
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
#ifndef _RPKEYS_H_
#define _RPKEYS_H_

#include <stdbool.h>
#include "config.h"

#if (CONFIG_KEYPAD != HIBY_R1_PAD)

/* Every other target keeps its keymap exactly as it was. */
static inline bool rpkeys_handle(int button) { (void)button; return false; }
static inline bool rpkeys_locked(void) { return false; }
static inline void rpkeys_chirp_up(void) { }
static inline void rpkeys_chirp_down(void) { }
static inline bool rpkeys_countdown_active(void) { return false; }
static inline void rpkeys_chirp_step(int s, int t, bool r)
{ (void)s; (void)t; (void)r; }

#else

/* Offered every physical button, before the keymap sees it. Returns true
 * when it has taken ownership, in which case the caller must not look the
 * button up in any context: that is the whole point - these five keys mean
 * the same thing on every screen.
 *
 * Buttons the stick synthesises never reach here. They arrive further down
 * the action path, so the stick can still produce POWER combinations even
 * though a real POWER press no longer reaches the keymap. */
bool rpkeys_handle(int button);

/* True while input is locked. Everything except the unlock hold is
 * swallowed. */
bool rpkeys_locked(void);

/* True while the full-screen lock countdown owns the display. The skin
 * engine stands down for the length of it; two things repainting the same
 * screen on different timers is a strobe, not a race worth winning. */
bool rpkeys_countdown_active(void);

/* One note of an arming run: step of total, rising or falling. */
void rpkeys_chirp_step(int step, int total, bool rising);

/* A rising chirp for something arming - the volume dial, an unlock - and a
 * falling one for something shutting: the lock. Two sounds the user can
 * tell apart with the device in a pocket. */
void rpkeys_chirp_up(void);
void rpkeys_chirp_down(void);

#endif /* CONFIG_KEYPAD */

#endif /* _RPKEYS_H_ */
