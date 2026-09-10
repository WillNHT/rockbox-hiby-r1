/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - the Rockbox side of the engine.
 *
 * Everything that knows about Rockbox lives here: turning a resolved
 * binding into the same button code a physical key would have produced,
 * replaying a tap that fell through, the click track, and the hardware
 * escape hatch. The engine itself (stick.c) knows none of it.
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

#include "config.h"

#ifdef HAVE_TOUCHSCREEN

#include <string.h>

#include "action.h"
#include "button.h"
#include "file.h"
#include "kernel.h"
#include "misc.h"
#include "rbpaths.h"
#include "settings.h"
#include "splash.h"
#include "stick.h"
#include "stick_glue.h"
#include "touchscreen.h"

#define LOGF_ENABLE
#include "logf.h"

/* The engine can only emit what a physical key emits, so it needs a table
 * of this target's key codes. Where no such table exists the feature
 * compiles but stays permanently off, which keeps other touch targets
 * exactly as they were. */
#if (CONFIG_KEYPAD == HIBY_R1_PAD)

struct binding_button
{
    int button;      /* the code to look up                       */
    int prebutton;   /* what last->button must be for a prereq    */
};

/* Deliberately the codes from keymap-hibyr1.c, not actions. What a binding
 * means is then whatever the current screen makes of that key, which is
 * spec 4's "meaning depends on the current screen" for free, and it keeps
 * working when the user remaps their keys.
 *
 * Scroll bindings emit a bare press, never BUTTON_REPEAT. That is what
 * keeps list acceleration out of the path so the stick alone owns the
 * repeat rate. */
static const struct binding_button binding_buttons[STICK_BIND_COUNT] =
{
    [STICK_BIND_NONE]        = { BUTTON_NONE,                 BUTTON_NONE   },
    [STICK_BIND_SCROLL_UP]   = { BUTTON_UP,                   BUTTON_NONE   },
    [STICK_BIND_SCROLL_DOWN] = { BUTTON_DOWN,                 BUTTON_NONE   },
    [STICK_BIND_SELECT]      = { BUTTON_RIGHT | BUTTON_REL,   BUTTON_RIGHT  },
    [STICK_BIND_BACK]        = { BUTTON_LEFT  | BUTTON_REL,   BUTTON_LEFT   },
    [STICK_BIND_PLAY_PAUSE]  = { BUTTON_PLAY  | BUTTON_REL,   BUTTON_PLAY   },
    [STICK_BIND_NEXT]        = { BUTTON_NEXT  | BUTTON_REL,   BUTTON_NEXT   },
    [STICK_BIND_PREV]        = { BUTTON_PREV  | BUTTON_REL,   BUTTON_PREV   },
    /* Volume only exists behind a repeat in this keymap, so these two are
     * the exception to the rule above. They are not list actions, so
     * nothing compounds. */
    [STICK_BIND_VOL_UP]      = { BUTTON_UP   | BUTTON_REPEAT, BUTTON_NONE   },
    [STICK_BIND_VOL_DOWN]    = { BUTTON_DOWN | BUTTON_REPEAT, BUTTON_NONE   },
};

/* Both volume keys together: a combination the keymap does not use, so it
 * cannot collide with anything the user might mean. */
#define STICK_KILL_COMBO   (BUTTON_UP | BUTTON_DOWN)
#define STICK_HAVE_KEYMAP

#endif /* CONFIG_KEYPAD == HIBY_R1_PAD */

#define KILL_HOLD_TICKS    (2 * HZ)
#define BOOT_SETTLE_TICKS  (10 * HZ)

/* Written when the stick is first used and removed once it has survived
 * ten seconds. Finding it at startup means the last run did not get that
 * far, so we start with the stick off and say so. */
#define STICK_MARKER_FILE  ROCKBOX_DIR "/stick_running.tmp"

static struct stick_config live_cfg;
static struct stick_state  live_state;

static bool  initialised;
static bool  session_disabled;      /* kill switch, or a crash-loop */
static bool  marker_present;
static int   cached_context = -1;
static long  first_use_tick;

/* While a fallthrough tap is being replayed through the ordinary path we
 * must not look at it again, or the replay would arm the stick afresh. */
static int   replay_pending;

/* When both volume keys went down together, or 0 if they are not held. */
static long  kill_held_since;

/* ------------------------------------------------------------------ state */

static long now_ms(void)
{
    return (long)current_tick * (1000 / HZ);
}

static void marker_write(void)
{
    int fd;

    if (marker_present)
        return;

    fd = open(STICK_MARKER_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        close(fd);
        marker_present = true;
        first_use_tick = current_tick;
    }
}

static void marker_clear(void)
{
    if (!marker_present)
        return;
    remove(STICK_MARKER_FILE);
    marker_present = false;
}

static void stick_lazy_init(void)
{
    if (initialised)
        return;
    initialised = true;

    if (file_exists(STICK_MARKER_FILE))
    {
        /* The previous run enabled the stick and never settled. Start
         * without it rather than repeating whatever went wrong. */
        logf("stick: previous run did not settle, starting disabled");
        splashf(HZ * 2, "Stick disabled after an unclean run");
        remove(STICK_MARKER_FILE);
        session_disabled = true;
    }

    stick_reset(&live_state, &live_cfg);
    cached_context = -1;
}

bool stick_enabled(void)
{
#ifndef STICK_HAVE_KEYMAP
    return false;
#else
    stick_lazy_init();

    if (session_disabled)
        return false;
    if (global_settings.touch_nav_mode == TOUCH_NAV_CLASSIC)
        return false;

    return true;
#endif
}

void stick_settings_apply(void)
{
    stick_config_invalidate();
    cached_context = -1;        /* force a rebuild on the next event */
    stick_reset(&live_state, &live_cfg);
}

const struct stick_state *stick_get_state(void)   { return &live_state; }
const struct stick_config *stick_get_config(void) { return &live_cfg; }

/* ------------------------------------------------------------------- cues */

static void play_cue(int cue)
{
    if (cue == STICK_CUE_NONE)
        return;
    if (!global_settings.keyclick)
        return;

    /* One short click for every cue point is the default (spec 7). The
     * mixer drops a click rather than queueing it, so this can never add
     * latency to the gesture. */
    system_sound_play(SOUND_KEYCLICK);
}

/* ---------------------------------------------------------- the kill switch */

void stick_check_killswitch(int button)
{
#ifdef STICK_HAVE_KEYMAP
    int held = button & ~(BUTTON_REL | BUTTON_REPEAT);

    /* Deliberately no lazy init here. This runs on every action poll,
     * including early in the boot before the card is mounted and before
     * there is a screen to splash on; the marker check belongs on the
     * touch path, where both are guaranteed to be up. */

    /* Once the stick has run for a while without taking the device down
     * with it, drop the crash marker. */
    if (marker_present && TIME_AFTER(current_tick,
                                     first_use_tick + BOOT_SETTLE_TICKS))
        marker_clear();

    if (session_disabled)
        return;

    if ((held & STICK_KILL_COMBO) == STICK_KILL_COMBO)
    {
        if (kill_held_since == 0)
            kill_held_since = current_tick;
        else if (TIME_AFTER(current_tick, kill_held_since + KILL_HOLD_TICKS))
        {
            session_disabled = true;
            kill_held_since = 0;
            marker_clear();
            stick_reset(&live_state, &live_cfg);
            if (global_settings.keyclick)
                system_sound_play(SOUND_KEYCLICK);
            splashf(HZ * 2, "Stick off for this session");
            logf("stick: disabled by the hardware kill switch");
        }
    }
    else
    {
        kill_held_since = 0;
    }
#else
    (void)button;
#endif
}

/* --------------------------------------------------------------- emitting */

#ifdef STICK_HAVE_KEYMAP

static int binding_to_button(int binding, int *prebutton)
{
    if (binding <= STICK_BIND_NONE || binding >= STICK_BIND_COUNT)
    {
        *prebutton = BUTTON_NONE;
        return BUTTON_NONE;
    }
    *prebutton = binding_buttons[binding].prebutton;
    return binding_buttons[binding].button;
}

/* Extra emissions that will not fit into this call go through the button
 * queue, where they drain on their own. */
static void post_binding(int binding)
{
    int pre;
    int btn = binding_to_button(binding, &pre);

    if (btn == BUTTON_NONE)
        return;

    if (pre != BUTTON_NONE)
        button_queue_post(pre, 0);
    button_queue_post(btn, 0);
}

static int dial_binding(int kind, int step)
{
    switch (kind)
    {
    case STICK_DIAL_VOLUME:
        return step > 0 ? STICK_BIND_VOL_UP : STICK_BIND_VOL_DOWN;
    case STICK_DIAL_SCROLL:
        /* Clockwise walks down the list, the way a wheel does. */
        return step > 0 ? STICK_BIND_SCROLL_DOWN : STICK_BIND_SCROLL_UP;
    default:
        return STICK_BIND_NONE;
    }
}

/* Hands a fallthrough tap back to ordinary touch handling by replaying it
 * through the button queue, at the press point rather than the release
 * point, so the hit test lands where the user actually touched. */
static void replay_tap(int x, int y)
{
    intptr_t data = ((intptr_t)(x & 0xffff) << 16) | (y & 0xffff);

    replay_pending = 2;
    button_queue_post(BUTTON_TOUCHSCREEN, data);
    button_queue_post(BUTTON_TOUCHSCREEN | BUTTON_REL, data);
}

#endif /* STICK_HAVE_KEYMAP */

/* ------------------------------------------------------------ event entry */

int stick_handle_touch(const struct touchevent *ev, int context,
                       int *button, int *prebutton)
{
#ifndef STICK_HAVE_KEYMAP
    (void)ev; (void)context; (void)button; (void)prebutton;
    return STICK_RESULT_PASS;
#else
    struct stick_output out;
    int type;

    *button = BUTTON_NONE;
    *prebutton = BUTTON_NONE;

    /* Our own replayed tap must go straight through. */
    if (replay_pending > 0)
    {
        if (ev->type == TOUCHEVENT_RELEASE)
            replay_pending--;
        else if (ev->type == TOUCHEVENT_PRESS)
            replay_pending--;
        return STICK_RESULT_PASS;
    }

    if (context != cached_context)
    {
        stick_build_config(&live_cfg, context);
        cached_context = context;
        /* Keep any live gesture pointed at the config it started with by
         * only re-seeding when nothing is in flight. */
        if (stick_phase(&live_state) == STICK_PHASE_IDLE)
            stick_reset(&live_state, &live_cfg);
    }

    switch (ev->type)
    {
    case TOUCHEVENT_PRESS:   type = STICK_TOUCH_PRESS;   break;
    case TOUCHEVENT_CONTACT: type = STICK_TOUCH_CONTACT; break;
    case TOUCHEVENT_RELEASE: type = STICK_TOUCH_RELEASE; break;
    default:                 type = STICK_TOUCH_IDLE;    break;
    }

    if (type == STICK_TOUCH_PRESS)
        marker_write();

    stick_process(&live_state, type, ev->x, ev->y, now_ms(), &out);

    play_cue(out.cue);

    switch (out.action)
    {
    case STICK_ACT_FIRE:
        *button = binding_to_button(out.binding, prebutton);
        if (*button == BUTTON_NONE)
            return STICK_RESULT_CONSUMED;
        return STICK_RESULT_BUTTON;

    case STICK_ACT_DIAL:
    {
        int binding = dial_binding(live_cfg.dial, out.dial_steps);
        int n = out.dial_steps < 0 ? -out.dial_steps : out.dial_steps;
        int i;

        if (binding == STICK_BIND_NONE || n == 0)
            return STICK_RESULT_CONSUMED;

        /* The first step goes out inline; a fast spin's remainder drains
         * through the queue. */
        for (i = 1; i < n; i++)
            post_binding(binding);

        *button = binding_to_button(binding, prebutton);
        return *button == BUTTON_NONE ? STICK_RESULT_CONSUMED
                                      : STICK_RESULT_BUTTON;
    }

    case STICK_ACT_TAP:
        replay_tap(out.tap_x, out.tap_y);
        return STICK_RESULT_CONSUMED;

    case STICK_ACT_PASS:
        return STICK_RESULT_PASS;

    case STICK_ACT_DROP:
    case STICK_ACT_NONE:
    default:
        return STICK_RESULT_CONSUMED;
    }
#endif
}

#endif /* HAVE_TOUCHSCREEN */
