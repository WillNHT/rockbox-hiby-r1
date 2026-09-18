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

/* Settings > Video. Everything that moves sits behind the first item. */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "lang.h"
#include "action.h"
#include "settings.h"
#include "rbpaths.h"
#include "menu.h"
#include "tree.h"
#include "dir.h"
#include "splash.h"
#include "icons.h"
#include "string-extra.h"
#include "exported_menus.h"
#include "skin_engine/skin_engine.h"
#include "video/video_lib.h"
#include "video/video_art.h"
#include "video/screensaver.h"

static int video_callback(int action, const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_EXIT_MENUITEM)
        video_art_release();
    return action;
}

MENUITEM_SETTING(video_enable_item, &global_settings.video_enabled,
                 video_callback);
MENUITEM_SETTING(video_canvas_item, &global_settings.video_canvas,
                 video_callback);
MENUITEM_SETTING(video_music_item, &global_settings.video_music,
                 video_callback);
MENUITEM_SETTING(video_latency_item, &global_settings.video_latency, NULL);
#if defined(HIBY_LINUX)
MENUITEM_SETTING(video_bt_latency_item, &global_settings.video_bt_latency,
                 NULL);
#endif
MENUITEM_SETTING(video_fps_item, &global_settings.video_battery_fps,
                 video_callback);

/* ------------------------------------------------------ screensaver */

static bool show_saver(char *name, int attr, struct tree_context *tc)
{
    (void)tc;
    const char *dot = strrchr(name, '.');
    if (attr & ATTR_DIRECTORY)
        return false;
    return dot && !strcasecmp(dot, ".ss");
}

static int choose_saver(void *param)
{
    unsigned char *setting = param;
    char buf[MAX_PATH], selected[MAX_FILENAME + 8];
    struct browse_context browse = {
        .dirfilter = SHOW_ALL,
        .flags = BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU | BROWSE_DIRFILTER,
        .callback_show_item = show_saver,
        .title = str(LANG_SAVER),
        .icon = Icon_Wps,
        .root = WPS_DIR,
        .selected = selected,
        .buf = buf,
        .bufsize = sizeof(buf),
    };
    static const char *off = "-";
    MENUITEM_STRINGLIST(menu, ID2P(LANG_SAVER), NULL,
                        ID2P(LANG_SAVER), ID2P(LANG_OFF));

    switch (do_menu(&menu, NULL, NULL, false))
    {
        case 0:
            snprintf(selected, sizeof(selected), "%s.ss", setting);
            buf[0] = 0;
            rockbox_browse(&browse);
            if (!(browse.flags & BROWSE_SELECTED))
                return 0;
            set_file(buf, (char *)setting);
            break;
        case 1:
            strlcpy((char *)setting, off, MAX_FILENAME + 1);
            break;
        default:
            return 0;
    }
    settings_save();
    skin_unload_screensaver();
    return 0;
}

static char *saver_name(int selected_item, void *data, char *buffer,
                        size_t buffer_len)
{
    (void)selected_item;
    const unsigned char *setting = data;
    int label = (setting == global_settings.saver_battery) ?
                LANG_SAVER_BATTERY : LANG_SAVER_CHARGING;
    snprintf(buffer, buffer_len, "%s: %s", str(label),
             (setting[0] && setting[0] != '-') ? (const char *)setting
                                               : str(LANG_OFF));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT_W_PARAM(saver_battery_item, 0, choose_saver,
                                  global_settings.saver_battery,
                                  saver_name, NULL,
                                  global_settings.saver_battery,
                                  NULL, Icon_Wps);
MENUITEM_FUNCTION_DYNTEXT_W_PARAM(saver_charging_item, 0, choose_saver,
                                  global_settings.saver_charging,
                                  saver_name, NULL,
                                  global_settings.saver_charging,
                                  NULL, Icon_Wps);
MENUITEM_SETTING(saver_timeout_item, &global_settings.saver_timeout, NULL);
MENUITEM_SETTING(saver_fps_item, &global_settings.saver_battery_fps, NULL);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
MENUITEM_SETTING(saver_brightness_item,
                 &global_settings.saver_battery_brightness, NULL);
#endif
MENUITEM_SETTING(saver_keep_item, &global_settings.saver_keep_on, NULL);

static int saver_preview(void)
{
    if (!global_settings.video_enabled)
    {
        splash(HZ, ID2P(LANG_VIDEO_ENABLE));
        return 0;
    }
    screensaver_run(true);
    return 0;
}
MENUITEM_FUNCTION(saver_preview_item, 0, ID2P(LANG_SAVER_PREVIEW),
                  saver_preview, NULL, Icon_Wps);

MAKE_MENU(saver_menu, ID2P(LANG_SAVER), NULL, Icon_Wps,
          &saver_battery_item, &saver_charging_item, &saver_timeout_item,
          &saver_fps_item,
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
          &saver_brightness_item,
#endif
          &saver_keep_item, &saver_preview_item);

/* ---------------------------------------------------------- decoder */

static int decoder_info(void)
{
    const struct rbv_api *api = video_lib();
    splash(HZ * 3, api ? api->build : video_lib_error());
    return 0;
}
MENUITEM_FUNCTION(decoder_item, 0, ID2P(LANG_VIDEO_DECODER),
                  decoder_info, NULL, Icon_Questionmark);

MAKE_MENU(video_settings_menu, ID2P(LANG_VIDEO), NULL, Icon_Wps,
          &video_enable_item, &video_canvas_item, &video_music_item,
          &video_latency_item,
#if defined(HIBY_LINUX)
          &video_bt_latency_item,
#endif
          &video_fps_item, &saver_menu, &decoder_item);
