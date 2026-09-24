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
#ifndef __LCD_TRANSITION_H__
#define __LCD_TRANSITION_H__

#include "config.h"

/* Screen transitions. See lcd-transition.c. The values are stored in
 * settings, so only ever append. */
enum lcd_transition
{
    LCD_TRANSITION_NONE = 0,
    LCD_TRANSITION_FADE,
    LCD_TRANSITION_SLIDE,   /* the new screen slides over the old one */
    LCD_TRANSITION_PUSH,    /* the new screen pushes the old one out */
    LCD_TRANSITION_CASCADE, /* bands of the screen arrive one after another */
    LCD_TRANSITION_WIPE,    /* a soft edge sweeps across                   */
    LCD_TRANSITION_DISSOLVE,/* the new screen grains in pixel by pixel     */
    LCD_TRANSITION_BLINDS,  /* slats open across the whole screen at once  */
    LCD_TRANSITION_COUNT
};

#ifdef HAVE_LCD_TRANSITIONS
#include <stdbool.h>

/* The screen is about to change. What is on it now is kept, presents are
 * held back while the next screen is drawn, and the change is animated the
 * next time the calling thread waits (for a key, or in sleep()).
 *
 * dir is +1 for going forward/deeper, -1 for going back. A second arm
 * before the first has run keeps the first snapshot and takes the new
 * type, direction and duration. */
void lcd_transition_arm(enum lcd_transition type, int dir, int duration_ms);

/* Drop an armed transition; held presents go out at the next update. */
void lcd_transition_cancel(void);

/* The owner thread is about to wait: run an armed transition now. */
void lcd_transition_idle(void);

/* Called by lcd_update*(): true if the present is to be held back. */
bool lcd_transition_hold(void);

#else
static inline void lcd_transition_arm(enum lcd_transition type, int dir,
                                      int duration_ms)
{ (void)type; (void)dir; (void)duration_ms; }
static inline void lcd_transition_cancel(void) { }
static inline void lcd_transition_idle(void) { }
#endif

#endif /* __LCD_TRANSITION_H__ */
