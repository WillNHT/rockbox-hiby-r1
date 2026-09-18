/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2008 by Jonathan Gordon
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

#include <stdio.h>
#include "config.h"
#include "system.h"
#include "icons.h"
#include "font.h"
#include "kernel.h"
#include "misc.h"
#include "sound.h"
#include "action.h"
#include "settings.h"
#include "settings_list.h"
#include "lang.h"
#include "playlist.h"
#include "playlist_viewer.h"
#include "powermgmt.h"
#include "viewport.h"
#include "audio.h"
#include "quickscreen.h"
#include "talk.h"
#include "list.h"
#include "option_select.h"
#include "debug.h"
#include "shortcuts.h"
#include "appevents.h"

 /* 1 top, 1 bottom, 2 on either side, 1 for the icons
  * if enough space, top and bottom have 2 lines */
#define MIN_LINES 5
#define MAX_NEEDED_LINES 10
 /* pixels between the 2 center items minimum or between text and icons,
  * and between text and parent boundaries */
#define MARGIN 10
#define CENTER_ICONAREA_SIZE (MARGIN+8*2)

/* The quickscreen is four directions and no more - it is meant to be worked
 * blind, by muscle memory, while the player is in a hand. Four slots is not
 * enough room for everything worth reaching quickly, so a slot may instead
 * open a page of four more. Two levels, four ways each: still nothing to
 * read, still one press per level, but sixteen things within reach.
 *
 * An entry is exactly one of:
 *   cfgname   - a setting, cycled in place, as the quickscreen always did
 *   user_slot - whichever setting the user put in this slot in the settings
 *   page      - a nested page
 *   act       - something done on the spot, with val() showing its state
 *   leave     - something that needs the whole screen, so we close first
 */
struct qs_page;

struct qs_entry
{
    int lang_id;                    /* label; settings bring their own */
    const char *cfgname;
    const struct qs_page *page;
    void (*act)(void);
    const char *(*val)(char *buf, size_t len);
    int leave;                      /* a QUICKSCREEN_* to return */
    bool user_slot;
};

struct qs_page
{
    int lang_id;                    /* 0 at the root, which needs no title */
    struct qs_entry items[QUICKSCREEN_ITEM_COUNT];
};

static void qs_act_sleeptimer(void)
{
    toggle_sleeptimer();
}

static const char *qs_val_sleeptimer(char *buf, size_t len)
{
    /* get_sleep_timer() is in seconds, format_sleeptimer() wants minutes. */
    int secs = get_sleep_timer();
    return format_sleeptimer(buf, len, (secs + 10) / 60, NULL);
}

static const struct qs_page qs_page_playback = {
    .lang_id = LANG_PLAYBACK,
    .items = {
        [QUICKSCREEN_TOP]    = { .cfgname = "shuffle" },
        [QUICKSCREEN_LEFT]   = { .cfgname = "repeat" },
        [QUICKSCREEN_RIGHT]  = { .lang_id = LANG_SLEEP_TIMER,
                                 .act = qs_act_sleeptimer,
                                 .val = qs_val_sleeptimer },
        [QUICKSCREEN_BOTTOM] = { .lang_id = LANG_VIEW_DYNAMIC_PLAYLIST,
                                 .leave = QUICKSCREEN_GOTO_QUEUE },
    },
};

static const struct qs_page qs_page_sound = {
    .lang_id = LANG_SOUND_SETTINGS,
    .items = {
        [QUICKSCREEN_TOP]    = { .cfgname = "volume" },
        [QUICKSCREEN_LEFT]   = { .cfgname = "bass" },
        [QUICKSCREEN_RIGHT]  = { .cfgname = "treble" },
        [QUICKSCREEN_BOTTOM] = { .cfgname = "balance" },
    },
};

static const struct qs_page qs_page_display = {
    .lang_id = LANG_DISPLAY,
    .items = {
        [QUICKSCREEN_TOP]    = { .cfgname = "brightness" },
#ifdef HAVE_BACKLIGHT_DIM_IDLE
        [QUICKSCREEN_BOTTOM] = { .cfgname = "dim level" },
        [QUICKSCREEN_RIGHT]  = { .cfgname = "backlight off timeout" },
#endif
        [QUICKSCREEN_LEFT]   = { .cfgname = "backlight timeout" },
    },
};

/* The four slots the user configures in Settings. They kept working as they
 * always did; they just live one level down now. */
static const struct qs_page qs_page_custom = {
    .lang_id = LANG_SETTINGS,
    .items = {
        [QUICKSCREEN_TOP]    = { .user_slot = true },
        [QUICKSCREEN_LEFT]   = { .user_slot = true },
        [QUICKSCREEN_RIGHT]  = { .user_slot = true },
        [QUICKSCREEN_BOTTOM] = { .user_slot = true },
    },
};

static const struct qs_page qs_page_root = {
    .items = {
        [QUICKSCREEN_TOP]    = { .lang_id = LANG_PLAYBACK,
                                 .page = &qs_page_playback },
        [QUICKSCREEN_LEFT]   = { .lang_id = LANG_SOUND_SETTINGS,
                                 .page = &qs_page_sound },
        [QUICKSCREEN_RIGHT]  = { .lang_id = LANG_DISPLAY,
                                 .page = &qs_page_display },
        [QUICKSCREEN_BOTTOM] = { .lang_id = LANG_SETTINGS,
                                 .page = &qs_page_custom },
    },
};

#define QS_MAX_DEPTH 4

struct gui_quickscreen
{
    const struct qs_page *page;
    const struct qs_entry *entries[QUICKSCREEN_ITEM_COUNT];
    /* the setting an entry resolved to, NULL if it is not a setting */
    const struct settings_list *items[QUICKSCREEN_ITEM_COUNT];
    const struct qs_page *stack[QS_MAX_DEPTH];
    int depth;
};

static void quickscreen_load_page(struct gui_quickscreen *qs,
                                  const struct qs_page *page)
{
    qs->page = page;
    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; i++)
    {
        const struct qs_entry *e = &page->items[i];
        const struct settings_list *s = NULL;

        if (e->user_slot)
            s = global_settings.qs_items[i];
        else if (e->cfgname)
            s = find_setting_by_cfgname(e->cfgname);

        if (s && !is_setting_quickscreenable(s))
            s = NULL;

        qs->entries[i] = e;
        qs->items[i] = s;
    }
}

/* An empty slot draws nothing and swallows its direction. */
static bool quickscreen_item_used(const struct gui_quickscreen *qs, int i)
{
    const struct qs_entry *e = qs->entries[i];
    return qs->items[i] || e->page || e->act || e->leave;
}

static const unsigned char *quickscreen_title(const struct gui_quickscreen *qs,
                                              int i)
{
    if (qs->items[i])
        return P2STR(ID2P(qs->items[i]->lang_id));
    if (qs->entries[i]->lang_id)
        return P2STR(ID2P(qs->entries[i]->lang_id));
    return NULL;
}

static const unsigned char *quickscreen_value(const struct gui_quickscreen *qs,
                                              int i, char *buf, int len)
{
    const struct qs_entry *e = qs->entries[i];

    if (qs->items[i])
        return (const unsigned char *)
               option_get_valuestring(qs->items[i], buf, len,
                                      option_value_as_int(qs->items[i]));
    if (e->val)
        return (const unsigned char *)e->val(buf, (size_t)len);
    if (e->page)
        return (const unsigned char *)"...";
    return NULL;
}

static bool redraw;

static void quickscreen_update_callback(unsigned short id,
                                        void *data, void *userdata)
{
    (void)id;
    (void)data;
    (void)userdata;

    redraw = true;
}

static void quickscreen_fix_viewports(struct gui_quickscreen *qs,
                                        struct screen *display,
                                        struct viewport *parent,
                                        struct viewport
                                                  vps[QUICKSCREEN_ITEM_COUNT],
                                        struct viewport *vp_icons)
{
    int char_height, width, pad = 0;
    int left_width = 0, right_width = 0, vert_lines;
    const unsigned char *s;
    int nb_lines = viewport_get_nb_lines(parent);

    /* nb_lines only returns the number of fully visible lines, small screens
        or really large fonts could cause problems with the calculation below.
     */
    if (nb_lines == 0)
        nb_lines++;

    char_height = parent->height/nb_lines;

    /* center the icons VP first */
    *vp_icons = *parent;
    vp_icons->width = CENTER_ICONAREA_SIZE; /* abosulte smallest allowed */
    vp_icons->x = parent->x;
    vp_icons->x += (parent->width-CENTER_ICONAREA_SIZE)/2;

    vps[QUICKSCREEN_BOTTOM] = *parent;
    vps[QUICKSCREEN_TOP] = *parent;
    /* depending on the space the top/buttom items use 1 or 2 lines */
    if (nb_lines < MIN_LINES)
        vert_lines = 1;
    else
        vert_lines = 2;
    vps[QUICKSCREEN_TOP].y = parent->y;
    vps[QUICKSCREEN_TOP].height = vps[QUICKSCREEN_BOTTOM].height
            = vert_lines*char_height;
    vps[QUICKSCREEN_BOTTOM].y
            = parent->y + parent->height - vps[QUICKSCREEN_BOTTOM].height;

    /* enough space vertically, so put a nice margin */
    if (nb_lines >= MAX_NEEDED_LINES)
    {
        vps[QUICKSCREEN_TOP].y += MARGIN;
        vps[QUICKSCREEN_BOTTOM].y -= MARGIN;
    }

    vp_icons->y = vps[QUICKSCREEN_TOP].y
            + vps[QUICKSCREEN_TOP].height;
    vp_icons->height = vps[QUICKSCREEN_BOTTOM].y - vp_icons->y;

    /* adjust the left/right items widths to fit the screen nicely */
    s = quickscreen_title(qs, QUICKSCREEN_LEFT);
    if (s)
        left_width = display->getstringsize(s, NULL, NULL);
    s = quickscreen_title(qs, QUICKSCREEN_RIGHT);
    if (s)
        right_width = display->getstringsize(s, NULL, NULL);

    width = MAX(left_width, right_width);
    if (width*2 + vp_icons->width > parent->width)
    {   /* crop text viewports */
        width = (parent->width - vp_icons->width)/2;
    }
    else
    {   /* add more gap in icons vp */
        int excess = parent->width - vp_icons->width - width*2;
        if (excess > MARGIN*4)
        {
            pad = MARGIN;
            excess -= MARGIN*2;
        }
        vp_icons->x -= excess/2;
        vp_icons->width += excess;
    }

    vps[QUICKSCREEN_LEFT] = *parent;
    vps[QUICKSCREEN_LEFT].x = parent->x + pad;
    vps[QUICKSCREEN_LEFT].width = width;

    vps[QUICKSCREEN_RIGHT] = *parent;
    vps[QUICKSCREEN_RIGHT].x = parent->x + parent->width - width - pad;
    vps[QUICKSCREEN_RIGHT].width = width;

    vps[QUICKSCREEN_LEFT].height = vps[QUICKSCREEN_RIGHT].height
            = 2*char_height;

    vps[QUICKSCREEN_LEFT].y = vps[QUICKSCREEN_RIGHT].y
            = parent->y + (parent->height/2) - char_height;

    /* shrink the icons vp by a few pixels if there is room so the arrows
       aren't drawn right next to the text */
    if (vp_icons->width > CENTER_ICONAREA_SIZE*2)
    {
        vp_icons->width -= CENTER_ICONAREA_SIZE*2/3;
        vp_icons->x += CENTER_ICONAREA_SIZE*2/6;
    }
    if (vp_icons->height > CENTER_ICONAREA_SIZE*2)
    {
        vp_icons->height -= CENTER_ICONAREA_SIZE*2/3;
        vp_icons->y += CENTER_ICONAREA_SIZE*2/6;
    }

    /* text alignment */
    vps[QUICKSCREEN_LEFT].flags &= ~VP_FLAG_ALIGNMENT_MASK; /* left-aligned */
    vps[QUICKSCREEN_TOP].flags    |= VP_FLAG_ALIGN_CENTER;  /* centered */
    vps[QUICKSCREEN_BOTTOM].flags |= VP_FLAG_ALIGN_CENTER;  /* centered */
    vps[QUICKSCREEN_RIGHT].flags  &= ~VP_FLAG_ALIGNMENT_MASK;/* right aligned*/
    vps[QUICKSCREEN_RIGHT].flags  |= VP_FLAG_ALIGN_RIGHT;
}

/* sin(i * 11.25 degrees) * 256, one turn in 32 steps; cos comes off the same
 * table a quarter turn along. Same trick the stick overlay uses: at these
 * radii a 32-gon is a circle, and it costs no divides. */
#define WHEEL_STEPS 32
static const int16_t wheel_sin[WHEEL_STEPS] =
{
       0,   50,   98,  142,  181,  213,  237,  251,
     256,  251,  237,  213,  181,  142,   98,   50,
       0,  -50,  -98, -142, -181, -213, -237, -251,
    -256, -251, -237, -213, -181, -142,  -98,  -50,
};
#define WHEEL_COS(i) (wheel_sin[((i) + 8) & (WHEEL_STEPS - 1)])

static void wheel_point(int ox, int oy, int i, int rad, int *px, int *py)
{
    *px = ox + (rad * wheel_sin[i & (WHEEL_STEPS - 1)]) / 256;
    *py = oy - (rad * WHEEL_COS(i)) / 256;
}

static void wheel_ring(struct screen *display, int ox, int oy, int rad)
{
    int i, x0, y0, x1, y1;

    wheel_point(ox, oy, 0, rad, &x0, &y0);
    for (i = 1; i <= WHEEL_STEPS; i++)
    {
        wheel_point(ox, oy, i, rad, &x1, &y1);
        display->drawline(x0, y0, x1, y1);
        x0 = x1;
        y0 = y1;
    }
}

/* The four ways drawn as a click wheel: an outer ring, a hub, and the four
 * quadrant cuts between them, with the direction arrows sitting in the band.
 * The labels stay where they are - the wheel goes in the space between them,
 * which is exactly the gap the arrows had to themselves before. */
static bool gui_quickscreen_draw_wheel(const struct gui_quickscreen *qs,
                                       struct screen *display,
                                       struct viewport *vp_icons)
{
    const int ox = vp_icons->width / 2;
    const int oy = vp_icons->height / 2;
    int r_out = MIN(vp_icons->width, vp_icons->height) / 2 - 1;
    int r_hub, r_band;

    /* Too small to read as a wheel - the caller falls back to the arrows. */
    if (r_out < CENTER_ICONAREA_SIZE / 2)
        return false;

    r_hub = (r_out * 2) / 5;   /* the click wheel's centre button */
    r_band = (r_out + r_hub) / 2;

    wheel_ring(display, ox, oy, r_out);
    wheel_ring(display, ox, oy, r_hub);

    /* Quadrant cuts at the diagonals, so each way owns a quarter. */
    for (int i = 4; i < WHEEL_STEPS; i += 8)
    {
        int x0, y0, x1, y1;
        wheel_point(ox, oy, i, r_hub, &x0, &y0);
        wheel_point(ox, oy, i, r_out, &x1, &y1);
        display->drawline(x0, y0, x1, y1);
    }

    if (quickscreen_item_used(qs, QUICKSCREEN_TOP))
        display->mono_bitmap(bitmap_icons_7x8[Icon_UpArrow],
                             ox - 4, oy - r_band - 4, 7, 8);
    if (quickscreen_item_used(qs, QUICKSCREEN_BOTTOM))
        display->mono_bitmap(bitmap_icons_7x8[Icon_DownArrow],
                             ox - 4, oy + r_band - 4, 7, 8);
    if (quickscreen_item_used(qs, QUICKSCREEN_LEFT))
        display->mono_bitmap(bitmap_icons_7x8[Icon_FastBackward],
                             ox - r_band - 4, oy - 4, 7, 8);
    if (quickscreen_item_used(qs, QUICKSCREEN_RIGHT))
        display->mono_bitmap(bitmap_icons_7x8[Icon_FastForward],
                             ox + r_band - 4, oy - 4, 7, 8);

    /* The page name rides the hub, the way the centre button carries what
     * pressing it would do. It only goes in if it fits inside the circle;
     * clipped text in there reads as damage, not as a label. */
    if (qs->page->lang_id)
    {
        const unsigned char *page_title = P2STR(ID2P(qs->page->lang_id));
        int w, h;
        display->getstringsize(page_title, &w, &h);
        if (w <= r_hub * 2 - 2 && h <= r_hub * 2 - 2)
            display->putsxy(ox - w/2, oy - h/2, page_title);
    }

    return true;
}

static void gui_quickscreen_draw(const struct gui_quickscreen *qs,
                                 struct screen *display,
                                 struct viewport *parent,
                                 struct viewport vps[QUICKSCREEN_ITEM_COUNT],
                                 struct viewport *vp_icons)
{
    int i;
    char buf[MAX_PATH];
    unsigned const char *title, *value;
    struct viewport *last_vp = display->set_viewport(parent);
    display->clear_viewport();

    for (i = 0; i < QUICKSCREEN_ITEM_COUNT; i++)
    {
        struct viewport *vp = &vps[i];
        if (!quickscreen_item_used(qs, i))
            continue;
        display->set_viewport(vp);

        title = quickscreen_title(qs, i);
        value = quickscreen_value(qs, i, buf, MAX_PATH);
        if (!title)
            continue;
        if (!value)
            value = (const unsigned char *)"";

        if (viewport_get_nb_lines(vp) < 2)
        {
            char text[MAX_PATH];
            snprintf(text, MAX_PATH, "%s: %s", title, value);
            display->puts_scroll(0, 0, text);
        }
        else
        {
            display->puts_scroll(0, 0, title);
            display->puts_scroll(0, 1, value);
        }
    }
    /* draw the icons */
    display->set_viewport(vp_icons);

    if (global_settings.quickscreen_wheel &&
        gui_quickscreen_draw_wheel(qs, display, vp_icons))
    {
        skin_render_deferred(display, parent);
        display->set_viewport(last_vp);
        return;
    }

    if (quickscreen_item_used(qs, QUICKSCREEN_TOP))
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_UpArrow],
            (vp_icons->width/2) - 4, 0, 7, 8);
    }
    if (quickscreen_item_used(qs, QUICKSCREEN_RIGHT))
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_FastForward],
            vp_icons->width - 8, (vp_icons->height/2) - 4, 7, 8);
    }
    if (quickscreen_item_used(qs, QUICKSCREEN_LEFT))
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_FastBackward],
            0, (vp_icons->height/2) - 4, 7, 8);
    }
    if (quickscreen_item_used(qs, QUICKSCREEN_BOTTOM))
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_DownArrow],
            (vp_icons->width/2) - 4, vp_icons->height - 8, 7, 8);
    }

    /* On a nested page, say which one - between the arrows, where the root
     * screen has nothing to show anyway. */
    if (qs->page->lang_id)
    {
        const unsigned char *page_title = P2STR(ID2P(qs->page->lang_id));
        int w, h;
        display->getstringsize(page_title, &w, &h);
        if (w < vp_icons->width && h < vp_icons->height)
            display->putsxy((vp_icons->width - w)/2,
                            (vp_icons->height - h)/2, page_title);
    }

    skin_render_deferred(display, parent);
    display->set_viewport(last_vp);
}

static void talk_qs_option(const struct settings_list *opt, bool enqueue)
{
    if (!global_settings.talk_menu || !opt)
        return;

    if (enqueue)
        talk_id(opt->lang_id, enqueue);
    option_talk_value(opt, option_value_as_int(opt), enqueue);
}

static void talk_qs_item(const struct gui_quickscreen *qs, int i, bool enqueue)
{
    if (!global_settings.talk_menu || !quickscreen_item_used(qs, i))
        return;

    if (qs->items[i])
    {
        talk_qs_option(qs->items[i], enqueue);
        return;
    }
    /* Pages and actions have no value to read out, only a name. */
    talk_id(qs->entries[i]->lang_id, enqueue);
}

/*
 * Does the actions associated to the given button if any
 *  - qs : the quickscreen
 *  - button : the key we are going to analyse
 * returns : true if the button corresponded to an action, false otherwise
 */
enum qs_button_result {
    QS_BUTTON_UNUSED = 0,   /* not ours, or an empty slot */
    QS_BUTTON_CHANGED,      /* a setting moved, or something ran */
    QS_BUTTON_PAGE,         /* we are on a different page now */
    QS_BUTTON_LEAVE,        /* close the screen, *leave holds why */
};

static enum qs_button_result gui_quickscreen_do_button(
                            struct gui_quickscreen *qs, int button, int *leave)
{
    int item;
    bool previous = false;
    switch(button)
    {
        case ACTION_QS_TOP:
            item = QUICKSCREEN_TOP;
            break;

        case ACTION_QS_LEFT:
            item = QUICKSCREEN_LEFT;
            previous = true;
            break;

        case ACTION_QS_DOWN:
            item = QUICKSCREEN_BOTTOM;
            previous = true;
            break;

        case ACTION_QS_RIGHT:
            item = QUICKSCREEN_RIGHT;
            break;

        default:
            return QS_BUTTON_UNUSED;
    }

    if (!quickscreen_item_used(qs, item))
        return QS_BUTTON_UNUSED;

    const struct qs_entry *e = qs->entries[item];

    if (e->leave)
    {
        *leave = e->leave;
        return QS_BUTTON_LEAVE;
    }

    if (e->page)
    {
        if (qs->depth + 1 >= QS_MAX_DEPTH)
            return QS_BUTTON_UNUSED;
        qs->stack[qs->depth++] = qs->page;
        quickscreen_load_page(qs, e->page);
        return QS_BUTTON_PAGE;
    }

    if (e->act)
    {
        e->act();
        talk_qs_item(qs, item, false);
        return QS_BUTTON_CHANGED;
    }

    option_select_next_val(qs->items[item], previous, true);
    talk_qs_option(qs->items[item], false);
    return QS_BUTTON_CHANGED;
}

/* Back out one level. Returns false at the root, where back means leave. */
static bool gui_quickscreen_go_back(struct gui_quickscreen *qs)
{
    if (qs->depth == 0)
        return false;
    quickscreen_load_page(qs, qs->stack[--qs->depth]);
    return true;
}

#ifdef HAVE_TOUCHSCREEN
static int quickscreen_touchscreen_button(void)
{
    struct gesture_event gevent;
    if (!action_gesture_get_event(&gevent))
        return ACTION_NONE;

    switch (gevent.id) {
    case GESTURE_TAP:
    case GESTURE_HOLD:
        break;
    default:
        return ACTION_NONE;
    }

    enum { left=1, right=2, top=4, bottom=8 };

    int bits = 0;

    if(gevent.x < LCD_WIDTH/3)
        bits |= left;
    else if(gevent.x > 2*LCD_WIDTH/3)
        bits |= right;

    if(gevent.y < LCD_HEIGHT/3)
        bits |= top;
    else if(gevent.y > 2*LCD_HEIGHT/3)
        bits |= bottom;

    switch(bits) {
    case top:
        return ACTION_QS_TOP;
    case bottom:
        return ACTION_QS_DOWN;
    case left:
        return ACTION_QS_LEFT;
    case right:
        return ACTION_QS_RIGHT;
    case 0:
        return ACTION_STD_CANCEL;
    default:
        /* A corner. It used to cancel along with the centre, which cost
         * nothing while cancel only ever closed the screen. Now that it
         * also backs out of a page, a tap that missed the direction it was
         * aiming for would quietly undo a level, so corners do nothing. */
        return ACTION_NONE;
    }
}
#endif

static int gui_syncquickscreen_run(struct gui_quickscreen * qs, int button_enter, bool *usb)
{
    int button;
    struct viewport parent[NB_SCREENS];
    struct viewport vps[NB_SCREENS][QUICKSCREEN_ITEM_COUNT];
    struct viewport vp_icons[NB_SCREENS];
    int ret = QUICKSCREEN_OK;
    /* To quit we need either :
     *  - a second press on the button that made us enter
     *  - an action taken while pressing the enter button,
     *    then release the enter button*/
    bool can_quit = false;

    push_current_activity(ACTIVITY_QUICKSCREEN);

    add_event_ex(GUI_EVENT_NEED_UI_UPDATE, false, quickscreen_update_callback, NULL);

    FOR_NB_SCREENS(i)
    {
        screens[i].set_viewport(NULL);
        screens[i].scroll_stop();
        viewportmanager_theme_enable(i, true, &parent[i]);
        quickscreen_fix_viewports(qs, &screens[i], &parent[i], vps[i], &vp_icons[i]);
        gui_quickscreen_draw(qs, &screens[i], &parent[i], vps[i], &vp_icons[i]);
    }
    *usb = false;
    /* Announce current selection on entering this screen. This is all
       queued up, but can be interrupted as soon as a setting is
       changed. */
    cond_talk_ids(VOICE_QUICKSCREEN);
    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; i++)
        talk_qs_item(qs, i, true);

#ifdef HAVE_TOUCHSCREEN
    action_gesture_reset();
#endif
    action_wait_for_release();
    while (true) {
        if (redraw)
        {
            redraw = false;
            FOR_NB_SCREENS(i)
                gui_quickscreen_draw(qs, &screens[i], &parent[i],
                                     vps[i], &vp_icons[i]);
        }
        button = get_action(CONTEXT_QUICKSCREEN, HZ/5);
#ifdef HAVE_TOUCHSCREEN
        if (button == ACTION_TOUCHSCREEN)
            button = quickscreen_touchscreen_button();
#endif
        if (default_event_handler(button) == SYS_USB_CONNECTED)
        {
            *usb = true;
            break;
        }
        int leave = QUICKSCREEN_OK;
        enum qs_button_result done = gui_quickscreen_do_button(qs, button,
                                                               &leave);
        if (done == QS_BUTTON_LEAVE)
        {
            ret |= leave;
            break;
        }
        else if (done == QS_BUTTON_PAGE)
        {
            /* the labels changed, so the columns have to be measured again */
            FOR_NB_SCREENS(i)
            {
                for (int j = 0; j < QUICKSCREEN_ITEM_COUNT; j++)
                    screens[i].scroll_stop_viewport(&vps[i][j]);
                quickscreen_fix_viewports(qs, &screens[i], &parent[i],
                                          vps[i], &vp_icons[i]);
            }
            if (global_settings.talk_menu)
            {
                talk_id(qs->page->lang_id, false);
                for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; i++)
                    talk_qs_item(qs, i, true);
            }
            can_quit = true;
            redraw = true;
        }
        else if (done == QS_BUTTON_CHANGED)
        {
            ret |= QUICKSCREEN_CHANGED;
            can_quit = true;
            redraw = true;
        }
        else if (button == button_enter)
            can_quit = true;
        else if (button == ACTION_QS_VOLUP) {
            adjust_volume(1);
            FOR_NB_SCREENS(i)
                skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_NON_STATIC);
        }
        else if (button == ACTION_QS_VOLDOWN) {
            adjust_volume(-1);
            FOR_NB_SCREENS(i)
                skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_NON_STATIC);
        }
        else if (button == ACTION_STD_CONTEXT || button == ACTION_QS_SHORTCUTS)
        {
            ret |= QUICKSCREEN_GOTO_SHORTCUTS_MENU;
            break;
        }
        if ((button == button_enter) && can_quit)
            break;

        if (button == ACTION_STD_CANCEL)
        {
            /* Back out of a page first; only the root exits. */
            if (!gui_quickscreen_go_back(qs))
                break;
            FOR_NB_SCREENS(i)
            {
                for (int j = 0; j < QUICKSCREEN_ITEM_COUNT; j++)
                    screens[i].scroll_stop_viewport(&vps[i][j]);
                quickscreen_fix_viewports(qs, &screens[i], &parent[i],
                                          vps[i], &vp_icons[i]);
            }
            redraw = true;
        }
    }
    /* Notify that we're exiting this screen */
    cond_talk_ids_fq(VOICE_OK);
    FOR_NB_SCREENS(i)
    {   /* stop scrolling before exiting */
        for (int j = 0; j < QUICKSCREEN_ITEM_COUNT; j++)
            screens[i].scroll_stop_viewport(&vps[i][j]);
        viewportmanager_theme_undo(i, !(ret & QUICKSCREEN_GOTO_SHORTCUTS_MENU));
    }

    if (ret & QUICKSCREEN_GOTO_SHORTCUTS_MENU) /* Eliminate flashing of parent during */
        pop_current_activity_without_refresh();   /* transition to Shortcuts */
    else
        pop_current_activity();

    remove_event_ex(GUI_EVENT_NEED_UI_UPDATE, quickscreen_update_callback, NULL);

    return ret;
}

int quick_screen_quick(int button_enter)
{
    struct gui_quickscreen qs;
    bool usb = false;

    qs.depth = 0;
    quickscreen_load_page(&qs, &qs_page_root);

    int ret = gui_syncquickscreen_run(&qs, button_enter, &usb);
    if (ret & QUICKSCREEN_CHANGED)
        settings_save();
    if (usb)
        return QUICKSCREEN_IN_USB;
    /* The queue needs the whole screen, so it opens once we are out of the
     * way. Callers never see this, they just get control back afterwards. */
    if (ret & QUICKSCREEN_GOTO_QUEUE)
    {
        if (playlist_viewer() == PLAYLIST_VIEWER_USB)
            return QUICKSCREEN_IN_USB;
        return QUICKSCREEN_OK;
    }
    return ret & QUICKSCREEN_GOTO_SHORTCUTS_MENU ? QUICKSCREEN_GOTO_SHORTCUTS_MENU :
                                                   QUICKSCREEN_OK;
}

/* stuff to make the quickscreen configurable */
bool is_setting_quickscreenable(const struct settings_list *setting)
{
    if (!setting)
        return true;

    /* to keep things simple, only settings which have a lang_id set are ok */
    if (setting->lang_id < 0 || (setting->flags & F_BANFROMQS))
        return false;

    switch (setting->flags & F_T_MASK)
    {
        case F_T_BOOL:
            return true;
        case F_T_INT:
        case F_T_UINT:
            return (setting->RESERVED != NULL);
        default:
            return false;
    }
}
