/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
* $Id$
*
* Copyright (C) 2007 Jonathan Gordon
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
#ifndef __ROOT_MENU_H__
#define __ROOT_MENU_H__

#include "config.h"
#include "gcc_extensions.h"

void root_menu(void) NORETURN_ATTR;
struct menu_table {
    char *string;
    const struct menu_item_ex *item;
};

struct menu_table *root_menu_get_options(int *nb_options);

enum {
    /* from old menu api, but still required*/
    MENU_ATTACHED_USB = -10,
    MENU_SELECTED_EXIT = -9,

    GO_TO_ROOTITEM_CONTEXT = -5,
    GO_TO_PREVIOUS_MUSIC = -4,
    GO_TO_PREVIOUS_BROWSER = -3,
    GO_TO_PREVIOUS = -2,
    GO_TO_ROOT = -1,
    GO_TO_FILEBROWSER = 0,
#ifdef HAVE_TAGCACHE
    GO_TO_DBBROWSER,
#endif
    GO_TO_WPS,
    GO_TO_MAINMENU,
#ifdef HAVE_RECORDING
    GO_TO_RECSCREEN,
#endif
#if CONFIG_TUNER
    GO_TO_FM,
#endif
    GO_TO_RECENTBMARKS,
    GO_TO_PLUGIN,
    /* Do Not add any items above here unless you want it to be able to 
       be the "start screen" after a boot up. The setting in settings_list.c
       will need editing if this is the case. */
    GO_TO_BROWSEPLUGINS,
    GO_TO_TIMESCREEN,
    GO_TO_PLAYLISTS_SCREEN,
    GO_TO_PLAYLIST_VIEWER,
    GO_TO_SYSTEM_SCREEN,
    GO_TO_SHORTCUTMENU,
    /* A main menu entry the user points at a folder of their own. It is
     * the file browser opened somewhere in particular, not a new screen:
     * everything the browser does - playing, the context menu, going up -
     * has to keep working, and anything that reimplemented it would be a
     * second file browser to maintain. */
    GO_TO_CUSTOMFOLDER
};
#ifndef PLUGIN
extern struct menu_item_ex root_menu_;

void root_menu_load_from_cfg(void* setting, char *value);
char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len);
void root_menu_set_default(void* setting, void* defaultval);
bool root_menu_is_changed(void* setting, void* defaultval);

/* The custom names the user has given main menu items. Display only: the
 * item, its icon, its action and the string written to config.cfg are all
 * unchanged, so renaming "Files" to something else does not make it a
 * different menu entry or break a theme that names it. */
void root_menu_names_load_from_cfg(void* setting, char *value);
char* root_menu_names_write_to_cfg(void* setting, char*buf, int buf_len);
void root_menu_names_set_default(void* setting, void* defaultval);
bool root_menu_names_are_changed(void* setting, void* defaultval);

/* The name to show for one item, or NULL for "use the built-in one". Takes
 * the menu item because that is what the list callback has to hand. */
const char *root_menu_custom_name(const struct menu_item_ex *item);

/* Reading and writing the arrangement, for the settings screen that edits
 * it. `order` is ROOT_MENU_MAX_ITEMS table indices: the first
 * *visible_count of them are the menu, in order, and the rest are the
 * items currently hidden. */
#define ROOT_MENU_MAX_ITEMS   16
#define ROOT_MENU_NAME_MAX    24
int  root_menu_item_count(void);
void root_menu_get_layout(unsigned char *order, int *visible_count);
void root_menu_set_layout(const unsigned char *order, int visible_count);
/* The built-in name of item n of the table, for the editor's own list. */
const char *root_menu_default_name(int table_index);
bool root_menu_has_custom_name(int table_index);
bool root_menu_is_settings(int table_index);
const char *root_menu_name_of(int table_index);
/* Whether this table entry is the user's custom folder, which is the one
 * row in the Main Menu editor that has a folder to set. */
bool root_menu_is_custom_folder(int table_index);
void root_menu_set_name(int table_index, const char *name);
#endif


#endif /* __ROOT_MENU_H__ */
