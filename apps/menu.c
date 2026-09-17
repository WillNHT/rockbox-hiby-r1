/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Robert E. Hak
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
/*
2005 Kevin Ferrare :
 - Multi screen support
 - Rewrote/removed a lot of code now useless with the new gui API
*/
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"

#include "appevents.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "menu.h"
#include "button.h"
#include "kernel.h"
#include "debug.h"
#include "usb.h"
#include "panic.h"
#include "settings.h"
#include "settings_list.h"
#include "option_select.h"
#include "screens.h"
#include "talk.h"
#include "lang.h"
#include "misc.h"
#include "action.h"
#include "menus/exported_menus.h"
#include "string.h"
#include "root_menu.h"
#include "audio.h"
#include "viewport.h"
#include "quickscreen.h"
#include "shortcuts.h"
#include "statusbar-skinned.h"

#include "icons.h"

/* gui api */
#include "list.h"
#include "transition.h"

#define MAX_MENUS 8
/* used to allow for dynamic menus */
#define MAX_MENU_SUBITEMS 64
static struct menu_item_ex *current_submenus_menu;
static int current_subitems[MAX_MENU_SUBITEMS];
static int current_subitems_count = 0;
static int talk_menu_item(int selected_item, void *data);
static char *title_buf;
static size_t title_buf_sz;

struct menu_data_t
{
    const struct menu_item_ex *menu;
    int selected;
};

static int empty_menu_callback(int action, const struct menu_item_ex *this_item, struct gui_synclist *this_list)
{
    return action;
    (void)this_item;
    (void)this_list;
}

static void get_menu_callback(const struct menu_item_ex *m,
                        menu_callback_type *menu_callback)
{
    if (m->flags&(MENU_HAS_DESC|MENU_DYNAMIC_DESC))
        *menu_callback= m->callback_and_desc->menu_callback;
    else
        *menu_callback = m->menu_callback;

    if (!*menu_callback)
        *menu_callback = &empty_menu_callback;
}

static bool query_audio_status(int *old_audio_status)
{
    bool redraw_list = false;
    /* query audio status to see if it changed */
    int new_audio_status = audio_status();
    if (*old_audio_status != new_audio_status)
    {  /* force a redraw if anything changed the audio status from outside */
        *old_audio_status = new_audio_status;
        redraw_list = true;
    }
    return redraw_list;
}

static int get_menu_selection(int selected_item, const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_MENU || type == MT_RETURN_ID)
        && (selected_item<current_subitems_count))
        return current_subitems[selected_item];
    return selected_item;
}
static int find_menu_selection(int selected)
{
    int i;
    for (i=0; i< current_subitems_count; i++)
        if (current_subitems[i] == selected)
            return i;
    return 0;
}
/* Every row of every menu goes through here, so this is the one place the
 * lowercase has to be applied - and the one place it is safe, because the
 * name a menu *stores* is untouched: config.cfg keys, voice ids and the
 * strings other screens read are all still the originals. */
static const char* menu_item_name(int selected_item,
                                  void * data,
                                  char *buffer,
                                  size_t buffer_len);

static const char* get_menu_item_name(int selected_item,
                                      void * data,
                                      char *buffer,
                                      size_t buffer_len)
{
    static char lowered[MAX_PATH];
    const char *name = menu_item_name(selected_item, data, buffer, buffer_len);

    /* What comes back may still be a lang id rather than a string - the
     * list resolves it with P2STR itself - so resolve it here before
     * treating it as one. Reading an id as text is a segfault. */
    name = P2STR((unsigned char *)name);

    return display_lower(name, lowered, sizeof(lowered));
}

static const char* menu_item_name(int selected_item,
                                  void * data,
                                  char *buffer,
                                  size_t buffer_len)
{
    const char *name;
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    /* only MT_MENU or MT_RETURN_ID is allowed in here */
    if (type == MT_RETURN_ID)
    {
        if (menu->flags&MENU_DYNAMIC_DESC)
        {
            name = menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);
        }
        else
            name = menu->strings[selected_item];

        if (P2ID((unsigned char *)name) > VOICEONLY_DELIMITER)
            name = "";

        return name;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    /* A main menu item the user has renamed. Only the root menu asks, and
     * only for the label: the item still returns the same GO_TO_ value,
     * keeps its icon, and is still written to config.cfg under its own key.
     * Checked here rather than by rewriting the item because the items are
     * const and shared - the same &menu_ appears in more than one place. */
    if ((const struct menu_item_ex *)data == &root_menu_)
    {
        const char *custom = root_menu_custom_name(menu);
        if (custom)
            return custom;
    }

    if ((menu->flags&MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);

    type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
    {
        const struct settings_list *v = find_setting(menu->variable);
        if (v)
            return str(v->lang_id);
        else return "Not Done yet!";
    }
    return P2STR(menu->callback_and_desc->desc);
}

static enum themable_icons  menu_get_icon(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int menu_icon = Icon_NOICON;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
    {
        return Icon_Menu_functioncall;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if (menu->flags&MENU_HAS_DESC)
        menu_icon = menu->callback_and_desc->icon_id;
    else if (menu->flags&MENU_DYNAMIC_DESC)
        menu_icon = menu->menu_get_name_and_icon->icon_id;

    if (menu_icon == Icon_NOICON)
    {
        unsigned int flags = (menu->flags&MENU_TYPE_MASK);
        if(flags == MT_MENU)
            menu_icon = Icon_Submenu;
        else if (flags == MT_SETTING || flags == MT_SETTING_W_TEXT)
             menu_icon = Icon_Menu_setting;
        else if (flags == MT_FUNCTION_CALL || flags == MT_RETURN_VALUE)
             menu_icon = Icon_Menu_functioncall;
    }
    return menu_icon;
}

/* Where you are, not only what you are looking at.
 *
 * The settings tree is deep and every level of it looks the same: a title
 * reading "Scrolling" says nothing about which of the several Scrolling
 * menus this is, or how to describe where you got to. do_menu() already
 * keeps the ancestors on a stack so the back key works - this walks it.
 *
 * Gated on the same Show Path setting the file browser uses. Asking twice
 * for the same thing in two places is a split nobody remembers they made.
 */
static const struct menu_data_t *crumb_stack;
static const int *crumb_depth;

static const char *crumb_name(const struct menu_item_ex *menu,
                              char *buf, size_t sz)
{
    if (!menu)
        return NULL;
    if (menu->flags & MENU_HAS_DESC)
        return P2STR(menu->callback_and_desc->desc);
    if (menu->flags & MENU_DYNAMIC_DESC)
        return menu->menu_get_name_and_icon->list_get_name(-1,
                   menu->menu_get_name_and_icon->list_get_name_data, buf, sz);
    return NULL;
}

static char *build_breadcrumb(char *title)
{
    static char crumbs[MAX_PATH];
    char tmp[64];
    size_t len = 0;
    int i;

    if (global_settings.show_path_in_browser != SHOW_PATH_FULL ||
        !crumb_stack || !crumb_depth || *crumb_depth <= 0)
        return title;

    for (i = 0; i < *crumb_depth && i < MAX_MENUS; i++)
    {
        const char *name = crumb_name(crumb_stack[i].menu, tmp, sizeof tmp);
        int used;

        if (!name || !*name)
            continue;
        used = snprintf(crumbs + len, sizeof(crumbs) - len, "%s / ", name);
        if (used < 0 || (size_t)used >= sizeof(crumbs) - len)
            return title;       /* too deep to say: the plain title is honest */
        len += used;
    }

    if (len == 0)
        return title;

    snprintf(crumbs + len, sizeof(crumbs) - len, "%s", title ? title : "");
    return crumbs;
}

static char* init_title(const struct menu_item_ex *menu, int *icon)
{
    char *title;

    if (menu->flags&MENU_HAS_DESC)
    {
        *icon = menu->callback_and_desc->icon_id;
        title = P2STR(menu->callback_and_desc->desc);
    }
    else if (menu->flags&MENU_DYNAMIC_DESC)
    {
        *icon = menu->menu_get_name_and_icon->icon_id;
        title = menu->menu_get_name_and_icon->
                      list_get_name(-1, menu->menu_get_name_and_icon->
                                    list_get_name_data, title_buf, title_buf_sz);
    }
    else
    {
        *icon = Icon_NOICON;
        title = "";
    }

    if (*icon == Icon_NOICON)
        *icon = Icon_Submenu_Entered;

    return title;
}

static int init_menu_lists(const struct menu_item_ex *menu,
                     struct gui_synclist *lists, int selected, bool callback,
                     struct viewport parent[NB_SCREENS])
{
    if (!menu || !lists)
    {
        panicf("init_menu_lists, NULL pointer");
        return 0;
    }

    int i;
    int start_action = ACTION_ENTER_MENUITEM;
    int count = MIN(MENU_GET_COUNT(menu->flags), MAX_MENU_SUBITEMS);
    int type = (menu->flags&MENU_TYPE_MASK);
    menu_callback_type menu_callback = &empty_menu_callback;
    int icon;
    char * title;
    current_subitems_count = 0;

    if (type == MT_RETURN_ID)
        get_menu_callback(menu, &menu_callback);

    for (i=0; i<count; i++)
    {
        if (type != MT_RETURN_ID)
            get_menu_callback(menu->submenus[i],&menu_callback);

        if (menu_callback(ACTION_REQUEST_MENUITEM,
            type==MT_RETURN_ID ? (void*)(intptr_t)i: menu->submenus[i], lists)
                != ACTION_EXIT_MENUITEM)
        {
            current_subitems[current_subitems_count] = i;
            current_subitems_count++;
        }
    }

    current_submenus_menu = (struct menu_item_ex *)menu;

    gui_synclist_init(lists,get_menu_item_name,(void*)menu,false,1, parent);
    title = build_breadcrumb(init_title(menu, &icon));
    {
        static char lowered_title[MAX_PATH];
        title = (char *)display_lower(title, lowered_title,
                                      sizeof(lowered_title));
    }
    gui_synclist_set_title(lists, title, icon);
    gui_synclist_set_icon_callback(lists, global_settings.show_icons?menu_get_icon:NULL);
    if(global_settings.talk_menu)
        gui_synclist_set_voice_callback(lists, talk_menu_item);
    gui_synclist_set_nb_items(lists,current_subitems_count);
    gui_synclist_select_item(lists, find_menu_selection(selected));

    get_menu_callback(menu,&menu_callback);
    if (callback)
        start_action = menu_callback(start_action, menu, lists);

    return start_action;
}

static int talk_menu_item(int selected_item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int id = -1;
    int type;
    unsigned char *str;
    int sel = get_menu_selection(selected_item, menu);

        if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
        {
            type = menu->submenus[sel]->flags&MENU_TYPE_MASK;
            if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
                talk_setting(menu->submenus[sel]->variable);
            else
            {
                if (menu->submenus[sel]->flags&(MENU_DYNAMIC_DESC))
                {
                    int (*list_speak_item)(int selected_item, void * data)
                        = menu->submenus[sel]->menu_get_name_and_icon->
                        list_speak_item;
                    if(list_speak_item)
                        list_speak_item(sel, menu->submenus[sel]->
                                        menu_get_name_and_icon->
                                        list_get_name_data);
                    else
                    {
                        char buffer[80];
                        str = menu->submenus[sel]->menu_get_name_and_icon->
                            list_get_name(sel, menu->submenus[sel]->
                                    menu_get_name_and_icon->
                                    list_get_name_data, buffer, sizeof(buffer));
                        id = P2ID(str);
                    }
                }
                else
                    id = P2ID(menu->submenus[sel]->callback_and_desc->desc);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        else if(((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID))
        {
            if ((menu->flags&MENU_DYNAMIC_DESC) == 0)
            {
                unsigned char *s = (unsigned char *)menu->strings[sel];
                /* string list, try to talk it if ID2P was used */
                id = P2ID(s);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        return 0;
}

void do_setting_screen(const struct settings_list *setting, const char * title,
                        struct viewport parent[NB_SCREENS])
{
    char padded_title[MAX_PATH];
    /* Pad the title string by repeating it. This is needed
       so the scroll settings title can actually be used to
       test the setting */
    if (setting->flags&F_PADTITLE)
    {
        int i = 0, len;
        title = P2STR((unsigned char*)title);
        len = strlen(title);
        while (i < MAX_PATH-1)
        {
            int padlen = MIN(len, MAX_PATH-1-i);
            memcpy(&padded_title[i], title, padlen);
            i += padlen;
            if (i<MAX_PATH-1)
                padded_title[i++] = ' ';
        }
        padded_title[i] = '\0';
        title = padded_title;
    }

    option_screen((struct settings_list *)setting, parent,
                  setting->flags&F_TEMPVAR, (char*)title);
}


void do_setting_from_menu(const struct menu_item_ex *temp,
                          struct viewport parent[NB_SCREENS])
{
    char *title;
    if (!temp)
    {
        panicf("do_setting_from_menu, NULL pointer");
        return;
    }
    const struct settings_list *setting = find_setting(temp->variable);

    if ((temp->flags&MENU_TYPE_MASK) == MT_SETTING_W_TEXT)
        title = temp->callback_and_desc->desc;
    else
        title = ID2P(setting->lang_id);

    do_setting_screen(setting, title, parent);
}

/* display a menu */
int do_menu(const struct menu_item_ex *start_menu, int *start_selected,
            struct viewport parent[NB_SCREENS], bool hide_theme)
{
    int selected = start_selected? *start_selected : 0;
    int ret = 0;
    int action;
    int start_action;
    int icon;
    char buf[80], *title;
    struct gui_synclist lists;
    const struct menu_item_ex *temp = NULL;
    const struct menu_item_ex *menu = start_menu;

    bool in_stringlist, done = false;
    bool redraw_lists;

    int old_audio_status = audio_status();

#ifdef HAVE_TOUCHSCREEN
    /* plugins possibly have grid mode active. force global settings in lists */
    enum touchscreen_mode tsm = touchscreen_get_mode();
    enum touchscreen_mode old_global_mode = global_settings.touch_mode;
    touchscreen_set_mode(global_settings.touch_mode);
#endif

    title_buf = buf;
    title_buf_sz = sizeof buf;
    title = init_title(menu, &icon);
    /* No crumbs yet - the stack below is empty at this point anyway. */
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(title, icon, i);
        viewportmanager_theme_enable(i, !hide_theme, NULL);
    }
    struct menu_data_t mstack[MAX_MENUS]; /* menu, selected */
    int stack_top = 0;

    /* Hand the stack to the title builder for the length of this menu, and
     * put back whatever an outer do_menu() had - a context menu opened from
     * inside a settings menu runs its own nested copy of all this. */
    const struct menu_data_t *outer_stack = crumb_stack;
    const int *outer_depth = crumb_depth;
    crumb_stack = mstack;
    crumb_depth = &stack_top;

    struct viewport *vps = NULL;
    menu_callback_type menu_callback = &empty_menu_callback;

    /* if hide_theme is true, assume parent has been fixed before passed into
     * this function, e.g. with viewport_set_defaults(parent, screen)
     * start_action allows an action to be processed
     * by menu logic by bypassing get_action on the initial run */
    start_action = init_menu_lists(menu, &lists, selected, true, parent);
    vps = *(lists.parent);
    in_stringlist = ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID);
    /* load the callback, and only reload it if menu changes */
    get_menu_callback(menu, &menu_callback);

    gui_synclist_draw(&lists);
    gui_synclist_speak_item(&lists);

    while (!done)
    {
        keyclick_set_callback(gui_synclist_keyclick_callback, &lists);

        if (UNLIKELY(start_action != ACTION_ENTER_MENUITEM))
        {
            action = start_action;
            start_action = ACTION_ENTER_MENUITEM;
        }
        else
            action = get_action(CONTEXT_MAINMENU|ALLOW_SOFTLOCK,
                                list_do_action_timeout(&lists, HZ));
            /* HZ so the status bar redraws corectly */

        /* query audio status to see if it changed */
        redraw_lists = query_audio_status(&old_audio_status);

#ifdef HAVE_TOUCHSCREEN
        /* need to translate touch actions *first* so the menu callback has
         * a chance to intercept before it hits the list's do_button. */
        if (action == ACTION_TOUCHSCREEN)
            action = gui_synclist_do_touchscreen(&lists);
#endif

        int new_action = menu_callback(action, menu, &lists);
        if (new_action == ACTION_EXIT_AFTER_THIS_MENUITEM)
            ret = MENU_SELECTED_EXIT; /* exit after return from selection */
        else if (new_action == ACTION_REDRAW)
            redraw_lists = true;
        else
            action = new_action;

        if (LIKELY(gui_synclist_do_button(&lists, &action)))
            continue;
#ifdef HAVE_QUICKSCREEN
        else if (action == ACTION_STD_QUICKSCREEN)
        {
            if (global_settings.shortcuts_replaces_qs ||
                quick_screen_quick(action) == QUICKSCREEN_GOTO_SHORTCUTS_MENU)
            {
                int last_screen = global_status.last_screen;
                global_status.last_screen = GO_TO_SHORTCUTMENU;
                int shortcut_ret = do_shortcut_menu(NULL);
                if (shortcut_ret == GO_TO_PREVIOUS)
                    global_status.last_screen = last_screen;
                else
                {
                    ret = shortcut_ret;
                    done = true;
                }
            }
            if (!done)
                init_menu_lists(menu, &lists, lists.selected_item, false, vps);
            redraw_lists = true;
        }
#endif
#ifdef HAVE_RECORDING
        else if (action == ACTION_STD_REC)
        {
            ret = GO_TO_RECSCREEN;
            done = true;
        }
#endif
        else if (action == ACTION_TREE_WPS)
        {
            ret = GO_TO_PREVIOUS_MUSIC;
            done = true;
        }
        else if (action == ACTION_TREE_STOP)
        {
            redraw_lists = list_stop_handler();
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            if (menu == &root_menu_)
            {
                ret = GO_TO_ROOTITEM_CONTEXT;
                done = true;
            }
            else if (!in_stringlist)
            {
                int type = (menu->flags&MENU_TYPE_MASK);
                selected = get_menu_selection(gui_synclist_get_sel_pos(&lists),menu);
                if (type == MT_MENU)
                {
                    temp = menu->submenus[selected];
                    type = (temp->flags&MENU_TYPE_MASK);
                }
                else
                    type = -1;

                if (type == MT_SETTING_W_TEXT || type == MT_SETTING)
                {
                    const struct menu_item_ex *context_menu;
                    const struct settings_list *setting =
                            find_setting(temp->variable);

                    MENUITEM_STRINGLIST(settings_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_RESET_SETTING),
#ifndef HAVE_QUICKSCREEN
                                       );
                    context_menu = &settings_op_menu;
#else
                                        ID2P(LANG_TOP_QS_ITEM),
                                        ID2P(LANG_LEFT_QS_ITEM),
                                        ID2P(LANG_BOTTOM_QS_ITEM),
                                        ID2P(LANG_RIGHT_QS_ITEM),
                                        ID2P(LANG_ADD_CURRENT_TO_FAVES),
                                        ID2P(LANG_ADD_TO_FAVES));

                    /* re-use the strings and desc from the settings_op_menu */
                    static const struct menu_item_ex non_quickscreen_op_menu =
                    {MT_RETURN_ID|MENU_HAS_DESC|MENU_ITEM_COUNT(1),
                    { .strings = settings_op_menu_},
                    {.callback_and_desc = &settings_op_menu__}};

                    if (is_setting_quickscreenable(setting))
                        context_menu = &settings_op_menu;
                    else
                    {
                        context_menu = &non_quickscreen_op_menu;
                    }
#endif
                    int msel = do_menu(context_menu, NULL, NULL, false);

                    switch (msel)
                    {
                        case GO_TO_PREVIOUS:
                            break;
                        case 0: /* reset setting */
                            reset_setting(setting, setting->setting);
                            settings_save();
                            settings_apply(false);
                            break;
#ifdef HAVE_QUICKSCREEN
                        case 1: /* set as top QS item */
                            global_settings.qs_items[QUICKSCREEN_TOP] = setting;
                            break;
                        case 2: /* set as left QS item */
                            global_settings.qs_items[QUICKSCREEN_LEFT] = setting;
                            break;
                        case 3: /* set as bottom QS item */
                            global_settings.qs_items[QUICKSCREEN_BOTTOM] = setting;
                            break;
                        case 4: /* set as right QS item */
                            global_settings.qs_items[QUICKSCREEN_RIGHT] = setting;
                            break;
                        case 5: /* Add current value of setting to faves.
                                   Same limitation on which can be
                                   added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING_APPLY, (void*)setting);
                            break;
                        case 6: /* Add to faves. Same limitation on which can be
                                  added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING, (void*)setting);
                            break;
#endif
                    } /* switch(do_menu()) */
                    if (menu->flags & MENU_EXITAFTERTHISMENU)
                        done = true; /* in case onplay menu contains setting */
                    redraw_lists = true;
                }
            } /* else if (!in_stringlist) */
        }
        else if (action == ACTION_STD_MENU)
        {
            if (menu != &root_menu_)
                ret = GO_TO_ROOT;
            else
                ret = GO_TO_PREVIOUS;
            done = true;
        }
        else if (action == ACTION_STD_CANCEL)
        {
            /* might be leaving list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);

            bool exiting_menu = false;
            in_stringlist = false;

            menu_callback(ACTION_EXIT_MENUITEM, menu, &lists);

            if (menu->flags&MENU_EXITAFTERTHISMENU)
                done = true;
            else if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
                exiting_menu = true;

            if (stack_top > 0)
            {
                gui_transition_menu(-1);
                stack_top--;
                menu = mstack[stack_top].menu;
                int msel = mstack[stack_top].selected;
                if (!exiting_menu && (menu->flags&MENU_EXITAFTERTHISMENU))
                    done = true;
                else
                    init_menu_lists(menu, &lists, msel, false, vps);
                redraw_lists = true;
                /* new menu, so reload the callback */
                get_menu_callback(menu, &menu_callback);
            }
            else if (menu != &root_menu_)
            {
                ret = GO_TO_PREVIOUS;
                done = true;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            /* entering an item that may not be a list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
            redraw_lists = true;

            int type = (menu->flags&MENU_TYPE_MASK);
            selected = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            if (type == MT_MENU)
                temp = menu->submenus[selected];
            else if (!in_stringlist)
                type = -1;

            if (!in_stringlist && temp)
            {
                type = (temp->flags&MENU_TYPE_MASK);
                get_menu_callback(temp, &menu_callback);
                action = menu_callback(ACTION_ENTER_MENUITEM, temp, &lists);
                if (action == ACTION_EXIT_MENUITEM)
                    break;
            }
            switch (type)
            {
                case MT_MENU:
                    if (stack_top < MAX_MENUS)
                    {
                        gui_transition_menu(1);
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, true, vps);
                    }
                    break;
                case MT_FUNCTION_CALL_W_PARAM:
                case MT_FUNCTION_CALL:
                {
                    int return_value;
                    /* Most of these open a screen of their own. One that
                     * does not leaves the frame as it was, and an unchanged
                     * frame is not animated. */
                    gui_transition_menu(1);
                    if (type == MT_FUNCTION_CALL_W_PARAM)
                    {
                        return_value = temp->function_param->function_w_param(
                                    temp->function_param->param);
                    }
                    else
                    {
                        return_value = temp->function->function();
                    }
                    gui_transition_menu(-1);
                    if (!(menu->flags&MENU_EXITAFTERTHISMENU) ||
                            (temp->flags&MENU_EXITAFTERTHISMENU))
                    {
                        /* Reload menu but don't run the calback again FS#8117 */
                        init_menu_lists(menu, &lists, selected, false, vps);
                    }
                    if (temp->flags&MENU_FUNC_CHECK_RETVAL)
                    {
                        if (return_value != 0)
                        {
                            done = true;
                            ret =  return_value;
                        }
                    }
                    break;
                }
                case MT_SETTING:
                case MT_SETTING_W_TEXT:
                {
                    gui_transition_menu(1);
                    do_setting_from_menu(temp, vps);
                    gui_transition_menu(-1);
                    init_menu_lists(menu, &lists, selected, false, vps);
                    redraw_lists = true;

                    break;
                }
                case MT_RETURN_ID:
                    if (in_stringlist)
                    {
                        done = true;
                        ret =  selected;
                    }
                    else if (stack_top < MAX_MENUS)
                    {
                        gui_transition_menu(1);
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu,&lists,0,false, vps);
                        in_stringlist = true;
                    }
                    break;
                case MT_RETURN_VALUE:
                    ret = temp->value;
                    done = true;
                    break;

                default:
                    ret = GO_TO_PREVIOUS;
                    done = true;
                    break;
            }
            if (type != MT_MENU)
            {
                menu_callback(ACTION_EXIT_MENUITEM, temp, &lists);
            }
            if (current_submenus_menu != menu)
                init_menu_lists(menu,&lists,selected,true,vps);
            /* callback was changed, so reload the menu's callback */
            get_menu_callback(menu, &menu_callback);
            if ((menu->flags&MENU_EXITAFTERTHISMENU) &&
                !(temp->flags&MENU_EXITAFTERTHISMENU))
            {
                done = true;
                break;
            }
        }
        else
        {
            if (action == SYS_USB_CONNECTED)
                gui_synclist_scroll_stop(&lists);

            switch(default_event_handler(action))
            {
                case SYS_USB_CONNECTED:
                    ret = MENU_ATTACHED_USB;
                    done = true;
                    break;
                case SYS_CALL_HUNG_UP:
                case BUTTON_MULTIMEDIA_PLAYPAUSE:
                /* remove splash from playlist_resume() */
                    redraw_lists = true;
                    break;
            }
        }

        if (redraw_lists && !done)
        {
            if (menu_callback(ACTION_REDRAW, menu, &lists) != ACTION_REDRAW)
                continue;


            gui_synclist_set_title(&lists, lists.title, lists.title_icon);
            gui_synclist_draw(&lists);
            gui_synclist_speak_item(&lists);
        }
    }

    if (start_selected)
    {
        /* make sure the start_selected variable is set to
           the selected item from the menu do_menu() was called from */
        if (stack_top > 0)
        {
            menu = mstack[0].menu;
            init_menu_lists(menu,&lists,mstack[0].selected,true, vps);
        }
        *start_selected = get_menu_selection(
                            gui_synclist_get_sel_pos(&lists), menu);
    }

    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(lists.title, lists.title_icon, i);
        viewportmanager_theme_undo(i, false);
        skinlist_set_cfg(i, NULL); /* Bugfix dangling reference in skin_draw() */
    }
#ifdef HAVE_TOUCHSCREEN
    /* This is needed because this function runs the settings menu and we do
     * not want to switch back to the old mode if the user intentionally went
     * to a different one. This is a very hacky way to do this... */
    if(!(global_settings.touch_mode != (int)old_global_mode &&
         tsm == old_global_mode))
        touchscreen_set_mode(tsm);
#endif
    crumb_stack = outer_stack;
    crumb_depth = outer_depth;
    return ret;
}
