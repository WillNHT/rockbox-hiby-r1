/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Arranging and renaming the main menu, from inside the menus.
 *
 * The order was always editable and never editable *here*: "root menu
 * order" in config.cfg, and a plugin to write it. Neither is a place
 * anybody looks. The names were not editable at all.
 *
 * Both are display-layer only, and deliberately so. The order is a
 * permutation of root_menu.c's menu_table and the names are a string per
 * entry; the items keep their GO_TO_ return values, their icons, and the
 * keys config.cfg stores them under. Renaming "Files" changes the label and
 * nothing else - it is still `files` on disk, still the file browser, and
 * still there after a firmware update that has never heard of the rename.
 *
 * The interaction is one list rather than a list plus a menu of verbs,
 * because the thing being edited is the order and the order is the list:
 *
 *   select        pick the item up. Scroll, and it comes with you.
 *                 Select again to drop it.
 *   context       the things that are not moving: show / hide, rename,
 *                 reset the name.
 *   back          done - the arrangement is applied and settings saved.
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
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "action.h"
#include "keyboard.h"
#include "lang.h"
#include "list.h"
#include "menu.h"
#include "misc.h"
#include "root_menu.h"
#include "settings.h"
#include "splash.h"
#include "string-extra.h"

#include "exported_menus.h"

/* The arrangement being edited. `order` is table indices, the first
 * `visible` of them being the menu as it will be. */
static struct
{
    unsigned char order[ROOT_MENU_MAX_ITEMS];
    int  visible;
    int  count;
    int  grabbed;       /* row being dragged, or -1 */
    int  last_sel;      /* where the cursor was, to know which way it went */
} layout;

/* The verbs that are not "move". Two lists rather than one, because the
 * first entry is the only one that changes and a menu that says "Show" over
 * something already showing is worse than two tables. */
MENUITEM_STRINGLIST(ctx_visible, ID2P(LANG_MAIN_MENU_LAYOUT), NULL,
                    ID2P(LANG_MAIN_MENU_HIDE),
                    ID2P(LANG_MAIN_MENU_RENAME),
                    ID2P(LANG_MAIN_MENU_RESET_NAME));

MENUITEM_STRINGLIST(ctx_hidden, ID2P(LANG_MAIN_MENU_LAYOUT), NULL,
                    ID2P(LANG_MAIN_MENU_SHOW),
                    ID2P(LANG_MAIN_MENU_RENAME),
                    ID2P(LANG_MAIN_MENU_RESET_NAME));

static const char *layout_get_name(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    int t;
    (void)data;

    if (selected_item < 0 || selected_item >= layout.count)
        return "";

    t = layout.order[selected_item];

    /* Three states worth telling apart at a glance, and no icons to spend
     * on them: the one in your hand, the ones in the menu, and the ones
     * that are not in the menu. */
    if (selected_item == layout.grabbed)
        snprintf(buffer, buffer_len, "> %s <", root_menu_name_of(t));
    else if (selected_item >= layout.visible)
        snprintf(buffer, buffer_len, "%s  (%s)", root_menu_name_of(t),
                 str(LANG_MAIN_MENU_HIDDEN));
    else
        snprintf(buffer, buffer_len, "%s", root_menu_name_of(t));

    return buffer;
}

static void swap_rows(int a, int b)
{
    unsigned char t = layout.order[a];
    layout.order[a] = layout.order[b];
    layout.order[b] = t;
}

/* Whether hiding this row is allowed. Settings is the way back into the
 * settings, so hiding it is a door that locks from the outside. */
static bool may_hide(int row)
{
    return !root_menu_is_settings(layout.order[row]);
}

static void toggle_hidden(int row)
{
    if (row < layout.visible)
    {
        if (!may_hide(row))
        {
            splash(HZ, ID2P(LANG_MAIN_MENU_KEEP_SETTINGS));
            return;
        }
        /* Out of the menu: it walks to the front of the hidden block, so
         * the visible ones keep the order they were in. */
        while (row < layout.visible - 1)
        {
            swap_rows(row, row + 1);
            row++;
        }
        layout.visible--;
    }
    else
    {
        /* Back into the menu, at the end of it. */
        while (row > layout.visible)
        {
            swap_rows(row, row - 1);
            row--;
        }
        layout.visible++;
    }
}

static void rename_row(int row)
{
    char buf[ROOT_MENU_NAME_MAX];
    int t = layout.order[row];

    strlcpy(buf, root_menu_name_of(t), sizeof(buf));

    if (kbd_input(buf, sizeof(buf), NULL) < 0)
        return;

    /* An empty answer is "use the built-in name", which is also what Reset
     * Name does - the same intention arrived at two ways. */
    root_menu_set_name(t, buf);
}

static void context_menu(int row)
{
    bool hidden = row >= layout.visible;
    int sel = do_menu(hidden ? &ctx_hidden : &ctx_visible, NULL, NULL, false);

    switch (sel)
    {
        case 0:
            toggle_hidden(row);
            break;
        case 1:
            rename_row(row);
            break;
        case 2:
            root_menu_set_name(layout.order[row], NULL);
            break;
        default:
            break;
    }
}

static int layout_action_callback(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    /* The cursor has already moved by the time this runs, which is exactly
     * what a drag wants: the item follows it into the row it just left. */
    if (layout.grabbed >= 0 && sel != layout.last_sel &&
        sel >= 0 && sel < layout.count)
    {
        int step = (sel > layout.last_sel) ? 1 : -1;
        int i;

        for (i = layout.last_sel; i != sel; i += step)
            swap_rows(i, i + step);

        layout.grabbed = sel;

        /* Dragging across the line between the menu and the hidden block is
         * how an item is shown or hidden by hand, so the line moves with it
         * rather than the move being refused. Settings is the exception: it
         * pushes the line along in front of it instead of crossing. */
        if (layout.grabbed >= layout.visible)
            layout.visible = may_hide(layout.grabbed) ? layout.grabbed
                                                      : layout.grabbed + 1;
        else if (layout.grabbed == layout.visible - 1)
            layout.visible = layout.grabbed + 1;
    }

    layout.last_sel = sel;

    switch (action)
    {
        case ACTION_STD_OK:
            layout.grabbed = (layout.grabbed >= 0) ? -1 : sel;
            return ACTION_REDRAW;

        case ACTION_STD_CONTEXT:
            if (layout.grabbed < 0)
                context_menu(sel);
            return ACTION_REDRAW;

        case ACTION_STD_CANCEL:
            if (layout.grabbed >= 0)
            {
                /* Back puts the item down rather than leaving the screen, so
                 * a drag can be abandoned without leaving with it. */
                layout.grabbed = -1;
                return ACTION_REDRAW;
            }
            break;

        default:
            break;
    }
    return action;
}

static int main_menu_layout_screen(void)
{
    struct simplelist_info info;
    int i;
    bool renamed = false;

    root_menu_get_layout(layout.order, &layout.visible);
    layout.count = root_menu_item_count();
    layout.grabbed = -1;
    layout.last_sel = 0;

    simplelist_info_init(&info, (char *)str(LANG_MAIN_MENU_LAYOUT),
                         layout.count, NULL);
    info.get_name = layout_get_name;
    info.action_callback = layout_action_callback;

    simplelist_show_list(&info);

    /* Applied on the way out rather than as the user drags, because a menu
     * rebuilt underneath the screen that is editing it is a way to find out
     * which of Rockbox's pointers into it were stale. */
    root_menu_set_layout(layout.order, layout.visible);

    for (i = 0; i < layout.count && !renamed; i++)
        renamed = root_menu_has_custom_name(i);

    global_settings.root_menu_customized = true;
    global_settings.root_menu_renamed = renamed;

    settings_save();
    return 0;
}

MENUITEM_FUNCTION(main_menu_layout_item, 0, ID2P(LANG_MAIN_MENU_LAYOUT),
                  main_menu_layout_screen, NULL, Icon_Submenu_Entered);
