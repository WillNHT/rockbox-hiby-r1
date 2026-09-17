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
#ifndef _GUI_TRANSITION_H_
#define _GUI_TRANSITION_H_

/* The screen is about to change; animate it with the user's settings.
 * dir: +1 going in/forward, -1 coming back. */

#include "config.h"
#include "lcd-transition.h"

#ifdef HAVE_LCD_TRANSITIONS
#include "settings.h"

static inline void gui_transition_menu(int dir)
{
    lcd_transition_arm(global_settings.menu_transition, dir,
                       global_settings.menu_transition_ms);
}

static inline void gui_transition_wps(int dir)
{
    lcd_transition_arm(global_settings.wps_transition, dir,
                       global_settings.wps_transition_ms);
}
#else
static inline void gui_transition_menu(int dir) { (void)dir; }
static inline void gui_transition_wps(int dir) { (void)dir; }
#endif

#endif /* _GUI_TRANSITION_H_ */
