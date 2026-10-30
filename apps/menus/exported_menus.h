/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 Jonathan Gordon
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
#ifndef _EXPORTED_MENUS_H
#define _EXPORTED_MENUS_H

#include "menu.h"
/* not needed for plugins */
#ifndef PLUGIN 

extern const struct menu_item_ex 
        display_menu,               /* display_menu.c   */
        playback_settings,          /* playback_menu.c  */
#ifdef HAVE_RECORDING
        recording_settings,         /* recording_menu.c */
        recording_settings_menu,
        peak_meter_menu,            /* also used from within recording_menu */
#endif
        replaygain_settings_menu,
        advanced_playback_menu,
        sound_settings,             /* sound_menu.c     */
        audio_effects_menu,
        library_menu,               /* settings_menu.c  */
        controls_menu,
        system_menu,
        settings_advanced_menu,
        device_sounds_menu,
        bookmark_settings_menu,
        altmenu_menu,
        wps_set_context_plugin,
        manage_settings,            /* main_menu.c      */
        info_menu,
#if CONFIG_RTC
        timedate_item,
#endif
        advanced_input_menu,        /* display_menu.c   */
        advanced_lists_menu,
        advanced_display_menu,
#ifdef HAVE_TOUCHSCREEN
        stick_menu,
#endif
        ab_settings_menu,           /* ab_ui.c          */
        pradio_settings_menu,       /* pradio.c         */
        playlist_settings,          /* playlist_menu.c  */
        viewer_settings_menu,       /* playlist_menu.c  */
        equalizer_menu,             /* eq_menu.c        */
#ifdef AUDIOHW_HAVE_EQ
        audiohw_eq_tone_controls,   /* audiohw_eq_menu.c */
#endif
        radio_settings_menu,        /* radio_menu.c     */
#ifdef HAVE_HIBY_BLUETOOTH
        hiby_bluetooth_root_item,   /* hiby_bluetooth_menu.c */
#endif
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
        hiby_wifi_root_item,        /* hiby_bluetooth_menu.c */
#endif
        main_menu_layout_item,      /* main_menu_layout.c */
        main_menu_title_item,       /* main_menu_layout.c */
#ifdef HAVE_VIDEO
        video_settings_menu,        /* video_menu.c     */
        saver_menu,
#endif
#ifdef HAVE_LCD_TRANSITIONS
        transitions_menu,           /* theme_menu.c     */
#endif
#ifdef HAVE_LCD_COLOR
        colors_settings,
        emoji_item,
#endif
#ifdef HAVE_BACKDROP_IMAGE
        clear_main_bd,
#endif
        browse_themes,
        browse_wps,
        browse_fonts,
        browse_fallback_font_item,
        browse_sbs,
        theme_show_icons,
        bars_menu;

struct browse_folder_info {
    const char* dir;
    int show_options;
};
int browse_folder(void *param); /* in theme_menu.c as it is mostly used there */

#endif /* ! PLUGIN */
#endif /*_EXPORTED_MENUS_H */
