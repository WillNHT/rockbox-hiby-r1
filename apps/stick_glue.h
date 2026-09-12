/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - the Rockbox side of the engine.
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
#ifndef _STICK_GLUE_H_
#define _STICK_GLUE_H_

#include "config.h"

#ifdef HAVE_TOUCHSCREEN

#include <stdbool.h>
#include "stick.h"

struct touchevent;

enum stick_result
{
    STICK_RESULT_PASS = 0,   /* not ours: ordinary touch handling      */
    STICK_RESULT_CONSUMED,   /* the stick owns it; produce no action   */
    STICK_RESULT_BUTTON,     /* a synthetic button is ready to look up */
};

/* True when the engine is in the input path at all. False unless the
 * touchscreen mode is TOUCHSCREEN_STICK, and false after the kill switch
 * or a crash-loop fallback - in which case nothing below is reached. */
bool stick_enabled(void);

/* True while a stick gesture or its coast is driving the list. Lists read
 * this to suppress wraparound: a drag is paper under a thumb, and paper
 * does not jump from the bottom back to the top. Rockbox already makes the
 * same exception for repeating keys. */
bool stick_is_scrolling(void);

/* Rebuild the engine config from global_settings. Cheap; call it whenever
 * the settings might have changed. */
void stick_settings_apply(void);

/* Feed one touch event. On STICK_RESULT_BUTTON, *button holds the code to
 * look up and *prebutton the value last->button must carry so the keymap's
 * prerequisite matching works. */
int stick_handle_touch(const struct touchevent *ev, int context,
                       int *button, int *prebutton);

/* Hardware-only escape hatch. Holding both volume keys together for two
 * seconds turns the stick off for the rest of the session, so a
 * misconfigured stick can always be escaped without the touchscreen. */
void stick_check_killswitch(int button);

/* Drives anything that outlives the thumb - today, the kinetic coast after
 * a flicked scroll. Call it once per action poll; it is a no-op unless a
 * coast is running. Any buttons it owes go out through the button queue,
 * because there is no touch event in hand to answer with. */
void stick_tick(int context);

/* Paints the live gesture, if the overlay is on. Must be called at the top
 * of the action poll, after whatever the last gesture fired has finished
 * redrawing the screen - anything drawn earlier than that is painted over
 * immediately. */
void stick_draw_overlay(void);

/* Repaints the gesture after a list has redrawn its rows over it. The list
 * calls this at the end of its own draw; nothing else should. */
void stick_redraw_overlay(void);

/* Read-only view for the lab plugin and, later, the presentation layer. */
const struct stick_state *stick_get_state(void);
const struct stick_config *stick_get_config(void);

/* Build a config for one context without touching the live engine. Used by
 * the lab plugin so it can run the real thing in isolation. */
void stick_build_config(struct stick_config *cfg, int context);

/* Throw away the cached config so the next build re-reads the settings and
 * stick.cfg. Called when the settings menu is left. */
void stick_config_invalidate(void);

#endif /* HAVE_TOUCHSCREEN */
#endif /* _STICK_GLUE_H_ */
