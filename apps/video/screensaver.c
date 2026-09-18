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

#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "power.h"
#include "action.h"
#include "appevents.h"
#include "misc.h"
#include "settings.h"
#include "viewport.h"
#include "screen_access.h"
#include "rbpaths.h"
#include "file.h"
#include "string-extra.h"
#include "skin_engine/skin_engine.h"
#include "skin_engine/wps_internals.h"
#include "statusbar-skinned.h"
#include "wps.h"
#include "audio.h"
#include "video_art.h"
#include "screensaver.h"

static long last_input;
static bool posted;
static bool active;
static bool input_seen;     /* a press while it was up: it ends */
static bool swallow;        /* the rest of that press is ours too */
static char skin_name[MAX_FILENAME + 1];

static bool on_charger(void)
{
#if CONFIG_CHARGING
    return charger_inserted();
#else
    return false;
#endif
}

static const char *chosen(void)
{
    const char *name = on_charger() ?
        (const char *)global_settings.saver_charging :
        (const char *)global_settings.saver_battery;
    if (!name[0] || name[0] == '-')
        return NULL;
    return name;
}

/* In the tick: nothing but counting. */
static void saver_tick(void)
{
    int timeout = global_settings.saver_timeout;
    if (posted || active || timeout <= 0 || !global_settings.video_enabled)
        return;
    if (TIME_AFTER(current_tick, last_input + timeout * HZ))
    {
        posted = true;
        button_queue_post(SYS_SCREENSAVER, 0);
    }
}

void screensaver_init(void)
{
    last_input = current_tick;
    tick_add_task(saver_tick);
}

void screensaver_note_input(void)
{
    last_input = current_tick;
    posted = false;
}

bool screensaver_filter_input(long button)
{
    if (button == BUTTON_NONE || IS_SYSEVENT(button) ||
        button == BUTTON_REDRAW)
        return false;
    screensaver_note_input();
    if (active)
    {
        input_seen = true;
        swallow = !(button & BUTTON_REL);
        return true;
    }
    if (swallow)
    {
        if ((button & BUTTON_REL) || !button_status())
            swallow = false;
        return true;
    }
    return false;
}

bool screensaver_active(void)
{
    return active;
}

const char *screensaver_skin_name(void)
{
    return skin_name[0] ? skin_name : NULL;
}

char *screensaver_default_skin(int screen)
{
    (void)screen;
    /* A clock and what is playing, for when the chosen file is missing. */
    static char skin[] =
        "%wd\n"
        "%X(d)\n"
        "%V(0,300,-,90,1)\n"
        "%ac%cH:%cM\n"
        "%V(20,420,-20,40,1)\n"
        "%ac%s%?it<%it|%fn>\n"
        "%V(20,470,-20,40,1)\n"
        "%ac%s%?ia<%ia|>\n";
    return skin;
}

/* The skin tags read the playing track from the WPS's state, which only
 * follows the music while the WPS is up. The screensaver can start from
 * anywhere, so it keeps that state itself while it runs. */
static void follow_track(void)
{
    struct wps_state *st = get_wps_state();
    if (audio_status() & AUDIO_STATUS_PLAY)
    {
        st->id3 = audio_current_track();
        st->nid3 = audio_next_track();
    }
    else
    {
        st->id3 = NULL;
        st->nid3 = NULL;
    }
}

static void track_changed(unsigned short id, void *param)
{
    (void)id;
    (void)param;
    follow_track();
    skin_request_full_update(SCREENSAVER_SKIN);
}

/* Is the skin engine's screensaver skin the one we want? */
static void load_skin(const char *name)
{
    if (!strcmp(skin_name, name))
        return;
    strlcpy(skin_name, name, sizeof(skin_name));
    skin_unload_screensaver();
}

static bool can_start(void)
{
    switch (get_current_activity())
    {
        case ACTIVITY_PLUGIN:
        case ACTIVITY_USBSCREEN:
        case ACTIVITY_RECORDING:
        case ACTIVITY_FM:
            return false;
        default:
            return true;
    }
}

static bool keep_on(void)
{
    switch (global_settings.saver_keep_on)
    {
        case SAVER_KEEP_ALWAYS:
            return true;
        case SAVER_KEEP_CHARGING:
            return on_charger();
        default:
            return false;
    }
}

static void apply_power(bool *battery_mode)
{
    bool battery = !on_charger();
    if (battery == *battery_mode)
        return;
    *battery_mode = battery;
    skin_animation_fps_cap(battery ? global_settings.saver_battery_fps : 0);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    backlight_set_brightness(battery ? global_settings.saver_battery_brightness
                                     : global_settings.brightness);
#endif
}

bool screensaver_run(bool preview)
{
    const char *name;
    long keep_tick = 0;
    bool battery_mode;
    bool was_lit = true;
    bool quit = false;
    bool full = true;

    if (active || !global_settings.video_enabled || !can_start())
        return false;
    name = chosen();
    if (!name)
    {
        if (!preview)
            return false;
        name = "-";
    }

    active = true;
    input_seen = false;
    load_skin(name);
    follow_track();
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
    add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_changed);
    add_event(PLAYBACK_EVENT_NEXTTRACKID3_AVAILABLE, track_changed);
    skin_request_full_update(SCREENSAVER_SKIN);
    push_current_activity(ACTIVITY_SCREENSAVER);
    button_clear_queue();

    FOR_NB_SCREENS(i)
    {
        struct gui_wps *gwps = skin_get_gwps(SCREENSAVER_SKIN, i);
        viewportmanager_theme_enable(i, skin_has_sbs(gwps), NULL);
        gwps->display->scroll_stop();
#ifdef HAVE_BACKDROP_IMAGE
        /* Its own backdrop, or none - not the theme's under the lists. */
        skin_backdrop_show(gwps->data->backdrop_id);
#endif
        gwps->display->clear_display();
    }

    /* The first comparison always differs. */
    battery_mode = on_charger();
    apply_power(&battery_mode);

    while (!quit)
    {
        int action;
        bool lit = is_backlight_lit();

        if (keep_on() && TIME_AFTER(current_tick, keep_tick))
        {
            backlight_on();
            keep_tick = current_tick + 2 * HZ;
        }
        apply_power(&battery_mode);

        if (lit)
        {
            if (full)
                skin_request_full_update(SCREENSAVER_SKIN);
            FOR_NB_SCREENS(i)
                skin_update(SCREENSAVER_SKIN, i, SKIN_REFRESH_NON_STATIC);
            full = false;
        }
        else
            full = true;    /* whatever is drawn when it comes back */

        action = skin_wait_for_action(SCREENSAVER_SKIN,
                                      CONTEXT_STD | ALLOW_SOFTLOCK,
                                      lit ? HZ / 2 : HZ);

        /* A press that only woke a dark panel still ends it. */
        if ((lit && !was_lit) || input_seen)
            quit = true;
        was_lit = lit;

        switch (action)
        {
            case ACTION_NONE:
            case SYS_SCREENSAVER:
                break;
            case ACTION_REDRAW:
                full = true;
                break;
            default:
                if (IS_SYSEVENT(action))
                {
                    if (default_event_handler(action) == SYS_USB_CONNECTED)
                        quit = true;
                    full = true;
                    break;
                }
                quit = true;
                break;
        }
    }

    remove_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
    remove_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_changed);
    remove_event(PLAYBACK_EVENT_NEXTTRACKID3_AVAILABLE, track_changed);
    skin_animation_fps_cap(0);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    backlight_set_brightness(global_settings.brightness);
#endif
    FOR_NB_SCREENS(i)
    {
        struct gui_wps *gwps = skin_get_gwps(SCREENSAVER_SKIN, i);
        gwps->display->scroll_stop();
#ifdef HAVE_BACKDROP_IMAGE
        skin_backdrop_show(sb_get_backdrop(i));
#endif
        viewportmanager_theme_undo(i, skin_has_sbs(gwps));
    }
    /* Clips the saver opened are not needed behind it. */
    video_art_release();
    pop_current_activity_without_refresh();
    active = false;
    screensaver_note_input();

    /* Whatever was underneath paints itself again. */
    skin_request_full_update(WPS);
    skin_request_full_update(CUSTOM_STATUSBAR);
    send_event(GUI_EVENT_THEME_CHANGED, NULL);
    button_clear_queue();
    button_queue_post(BUTTON_REDRAW, 0);
    return true;
}
