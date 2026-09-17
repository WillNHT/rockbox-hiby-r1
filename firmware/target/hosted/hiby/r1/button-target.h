/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 by Marcin Bukat
 * Copyright (C) 2025 by Melissa Autumn
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
#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

#include <stdbool.h>
#include "config.h"

/* Main unit's buttons */
#define BUTTON_POWER                0x00000001
#define BUTTON_RIGHT                 0x00000004 //Next
#define BUTTON_LEFT                 0x00000008 //Play/Pause
#define BUTTON_UP               0x00000010 //Volume Up
#define BUTTON_DOWN             0x00000020 //Volume Down

#define BUTTON_TOUCH                0x00000040

//TODO: It's from bluetooth, should we define it as BUTTON_REMOTE?
#define BUTTON_PREV                 0x00000100
#define BUTTON_NEXT                 0x00000200
#define BUTTON_PLAY                 0x00000400
#define BUTTON_MAIN                ((BUTTON_POWER|BUTTON_RIGHT|BUTTON_LEFT|BUTTON_UP|BUTTON_DOWN)|(BUTTON_PREV|BUTTON_NEXT|BUTTON_PLAY))

/* Physical keys pressed on a dimmed or dark screen only wake it; the press
 * is dropped. Bluetooth remote keys are not in here: a remote is used with
 * the player in a pocket, where the screen state is beside the point. */
#define BUTTON_WAKE_SWALLOWS     (BUTTON_POWER|BUTTON_RIGHT|BUTTON_LEFT|BUTTON_UP|BUTTON_DOWN)


/* Touchscreen virtual buttons */
#define BUTTON_TOPLEFT      0x00001000
#define BUTTON_TOPMIDDLE    0x00002000
#define BUTTON_TOPRIGHT     0x00004000
#define BUTTON_MIDLEFT      0x00008000
#define BUTTON_CENTER       0x00010000
#define BUTTON_MIDRIGHT     0x00020000
#define BUTTON_BOTTOMLEFT   0x00040000
#define BUTTON_BOTTOMMIDDLE 0x00080000
#define BUTTON_BOTTOMRIGHT  0x00100000

/* Software power-off.
 *
 * This is counted in button-thread repeats, which start at 0.16 s and
 * accelerate to 0.05 s, so the stock 25 fired at about two and a half
 * seconds - well before the ten second hold in apps/rpkeys.c, which is
 * what made a deliberate lock look like a shutdown.
 *
 * The driver's path is kept, at roughly fifteen seconds, purely as a
 * backstop: if the UI thread is wedged, rpkeys never runs, and that is
 * exactly when a way to power the device off matters most. */
#define POWEROFF_BUTTON BUTTON_POWER
#define POWEROFF_COUNT 290

int button_map_with_id(int keycode, int id);

/* While input is locked a tap on a dark screen stays dark. */
void button_set_touch_wake(bool enable);

#endif /* _BUTTON_TARGET_H_ */
