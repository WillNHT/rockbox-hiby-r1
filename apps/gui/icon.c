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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inttypes.h"
#include "config.h"
#include "core_alloc.h"
#include "icon.h"
#include "screen_access.h"
#include "icons.h"
#include "settings.h"
#include "rbpaths.h"
#include "bmp.h"
#include "filetypes.h"
#include "language.h"
#include "misc.h"

#include "bitmaps/default_icons.h"
#if defined(HAVE_REMOTE_LCD) && (NB_SCREENS > 1)
#include "bitmaps/remote_default_icons.h"
#endif

/* We dont actually do anything with these pointers,
   but they need to be grouped like this to save code
   so storing them as void* is ok. (stops compile warning) */
static const struct bitmap *inbuilt_iconset[NB_SCREENS] =
{
    &bm_default_icons,
#if defined(HAVE_REMOTE_LCD) && (NB_SCREENS > 1)
    &bm_remote_default_icons,
#endif
};

enum Iconset {
    Iconset_user,
    Iconset_viewers,
    Iconset_Count
};

static struct iconset {
    struct bitmap bmp;
    bool loaded;
    int handle;
} iconsets[Iconset_Count][NB_SCREENS];

#define ICON_HEIGHT(screen) (!iconsets[Iconset_user][screen].loaded ?       \
                             (*(inbuilt_iconset[screen])) : iconsets[Iconset_user][screen].bmp).height \
                            / Icon_Last_Themeable

#define ICON_WIDTH(screen)  (!iconsets[Iconset_user][screen].loaded ?       \
                             (*(inbuilt_iconset[screen])) : iconsets[Iconset_user][screen].bmp).width

/* x,y in letters, not pixles */
void screen_put_icon(struct screen * display,
                       int x, int y, enum themable_icons icon)
{
    screen_put_icon_with_offset(display, x, y, 0, 0, icon);
}

void screen_put_icon_with_offset(struct screen * display,
                       int x, int y, int off_x, int off_y,
                       enum themable_icons icon)
{
    const int screen = display->screen_type;
    const int icon_width = ICON_WIDTH(screen);
    const int icon_height = ICON_HEIGHT(screen);
    int xpos, ypos;
    int width, height;
    display->getstringsize((unsigned char *)"M", &width, &height);
    xpos = x*icon_width + off_x;
    ypos = y*height + off_y;

    if ( height > icon_height )/* center the cursor */
        ypos += (height - icon_height) / 2;
    screen_put_iconxy(display, xpos, ypos, icon);
}

/* x,y in pixels */
void screen_put_iconxy(struct screen * display,
                       int xpos, int ypos, enum themable_icons icon)
{
    const int screen = display->screen_type;
    const int width = ICON_WIDTH(screen);
    const int height = ICON_HEIGHT(screen);
    const int is_rtl = lang_is_rtl();
    const struct bitmap *iconset;

    if (icon == Icon_Book)
    {
        /* An open book, as the icon set draws things: solid shapes with
         * the detail cut out of them, not outlines. Two filled pages that
         * dip into the spine, a cover joining them underneath, and two
         * lines of text knocked out of each. Drawn rather than themed -
         * see the note in icon.h - in the list's own foreground colour. */
        int t  = MAX(2, height / 10);       /* the set's stroke           */
        int bh = height - 2 * (height / 8);
        int bw = MIN(width, height + height / 8);
        int bx = xpos + (width - bw) / 2, by = ypos + height / 8;
        int ph = bh - t;                    /* page height, over the cover */
        int pw = (bw - t) / 2;              /* one page                   */
        int rx, i;

        if (is_rtl)
            bx = display->getwidth() - bx - bw;
        rx = bx + pw + t;

        if (bw < 8 || bh < 8)
            return;

        display->fillrect(bx, by, pw, ph);
        display->fillrect(rx, by, pw, ph);
        display->fillrect(bx, by + ph, bw, t);

        display->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        display->fillrect(bx + pw - t, by, t, t);
        display->fillrect(rx, by, t, t);
        display->hline(bx + t, bx + pw - 1, by + ph - 1);
        display->hline(rx, rx + pw - t - 1, by + ph - 1);
        display->drawpixel(bx, by);
        display->drawpixel(bx + bw - 1, by);
        for (i = 1; i <= 2; i++)
        {
            int ly = by + (ph * i) / 3 - t / 2;
            display->fillrect(bx + t, ly, pw - 2 * t, MAX(1, t - 1));
            display->fillrect(rx + t, ly, pw - 2 * t, MAX(1, t - 1));
        }
        display->set_drawmode(DRMODE_SOLID);
        return;
    }

    if (icon == Icon_Heart)
    {
        /* A solid heart: two round lobes over a point, a row at a time. */
        int r  = (MIN(width, height) - height / 8) / 4;
        int cx = xpos + width / 2, cy = ypos + (height - 7 * r / 2) / 2 + r;
        int dx = 0, dy;

        if (is_rtl)
            cx = display->getwidth() - cx;
        if (r < 2)
            return;

        for (dy = -r; dy < 0; dy++)
        {
            while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
                dx++;
            display->hline(cx - r - dx, cx - r + dx, cy + dy);
            display->hline(cx + r - dx, cx + r + dx, cy + dy);
        }
        for (dy = 0; dy <= 5 * r / 2; dy++)
        {
            dx = 2 * r - (4 * dy + 2) / 5;
            display->hline(cx - dx, cx + dx, cy + dy);
        }
        return;
    }

    if (icon == Icon_Battery)
    {
        /* A cell on its side: a case of the set's stroke, the charge
         * inside it clear of the case, and the terminal on the end. */
        int t  = MAX(2, height / 10);
        int bh = (height - 2 * (height / 8)) * 2 / 3;
        int bw = MIN(width, height) - t;
        int bx = xpos + (width - bw - t) / 2, by = ypos + (height - bh) / 2;

        if (is_rtl)
            bx = display->getwidth() - bx - bw - t;
        if (bh < 4 * t || bw < 6 * t)
            return;

        display->fillrect(bx, by, bw, t);
        display->fillrect(bx, by + bh - t, bw, t);
        display->fillrect(bx, by, t, bh);
        display->fillrect(bx + bw - t, by, t, bh);
        display->fillrect(bx + bw, by + bh / 4, t, bh - 2 * (bh / 4));
        display->fillrect(bx + 2 * t, by + 2 * t,
                          (bw - 4 * t) * 2 / 3, bh - 4 * t);
        return;
    }

    if (icon == Icon_Usb)
    {
        /* A plug, end up: the grip, and the shell over it with its two
         * holes knocked out. */
        int t  = MAX(2, height / 10);
        int h  = height - 2 * (height / 8);
        int bw = MIN(width, height) / 2 / 2 * 2, sw = bw - 2 * t;
        int sh = h * 2 / 5;
        int cx = xpos + width / 2, y0 = ypos + (height - h) / 2;

        if (is_rtl)
            cx = display->getwidth() - cx;
        if (sw < 3 * t)
            return;

        display->fillrect(cx - sw / 2, y0, sw, sh);
        display->fillrect(cx - bw / 2, y0 + sh, bw, h - sh);
        display->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        display->fillrect(cx - sw / 2 + t, y0 + t, (sw - 3 * t) / 2, sh - 2 * t);
        display->fillrect(cx + sw / 2 - t - (sw - 3 * t) / 2, y0 + t,
                          (sw - 3 * t) / 2, sh - 2 * t);
        display->set_drawmode(DRMODE_SOLID);
        return;
    }

    if (icon == Icon_Bluetooth)
    {
        /* The rune: a spine, and from each end of it a stroke out to the
         * right and back across to the far side. */
        int t  = MAX(2, height / 10);       /* the set's stroke */
        int h  = (height - 2 * (height / 8)) / 4 * 4;
        int w  = h / 3;
        int cx = xpos + (width - t) / 2, y0 = ypos + (height - h) / 2;
        int i;

        if (is_rtl)
            cx = display->getwidth() - cx - t;
        if (h < 8)
            return;

        display->fillrect(cx, y0, t, h + 1);
        for (i = 0; i <= t; i++)     /* a slanted stroke reads thinner */
        {
            display->drawline(cx + i, y0, cx + w + i, y0 + h / 4);
            display->drawline(cx + w + i, y0 + h / 4,
                              cx - w + i, y0 + 3 * h / 4);
            display->drawline(cx + i, y0 + h, cx + w + i, y0 + 3 * h / 4);
            display->drawline(cx + w + i, y0 + 3 * h / 4,
                              cx - w + i, y0 + h / 4);
        }
        return;
    }

    if (icon == Icon_Wifi)
    {
        /* A dot and three arcs fanning up from it, a pixel at a time:
         * each arc is the band of its radius inside the quarter above. */
        int t  = MAX(2, height / 10);
        int r  = MIN(height - 2 * (height / 8), width * 2 / 3) / 3 * 3;
        int cx = xpos + width / 2, cy = ypos + (height + r) / 2;
        int dx, dy, k;

        if (is_rtl)
            cx = display->getwidth() - cx;
        if (r < 9)
            return;

        for (dy = -r; dy <= t / 2; dy++)
            for (dx = -r; dx <= r; dx++)
            {
                int d2 = dx * dx + dy * dy;
                bool on = 4 * d2 <= (t + 1) * (t + 1);

                for (k = 1; k <= 3 && !on && abs(dx) <= -dy; k++)
                    on = d2 <= (k * r / 3) * (k * r / 3) &&
                         d2 > (k * r / 3 - t) * (k * r / 3 - t);
                if (on)
                    display->drawpixel(cx + dx, cy + dy);
            }
        return;
    }

    if (icon <= Icon_NOICON)
    {
        if (is_rtl)
            xpos = display->getwidth() - xpos - width;
        screen_clear_area(display, xpos, ypos, width, height);
        return;
    }
    else if (icon >= Icon_Last_Themeable)
    {
        iconset = &iconsets[Iconset_viewers][screen].bmp;
        icon -= Icon_Last_Themeable;
        if (!iconsets[Iconset_viewers][screen].loaded ||
           (global_status.viewer_icon_count * height > iconset->height) ||
           (icon * height + height > iconset->height))
        {
            screen_put_iconxy(display, xpos, ypos, Icon_Questionmark);
            return;
        }
    }
    else if (iconsets[Iconset_user][screen].loaded)
    {
        iconset = &iconsets[Iconset_user][screen].bmp;
    }
    else
    {
        iconset = inbuilt_iconset[screen];
    }

    if (is_rtl)
        xpos = display->getwidth() - xpos - width;


    display->bmp_part(iconset, 0, height * icon, xpos, ypos, width, height);
}

void screen_put_cursorxy(struct screen * display, int x, int y, bool on)
{
    screen_put_icon(display, x, y, on?Icon_Cursor:0);
}

static int buflib_move_callback(int handle, void* current, void* new)
{
    (void)handle;
    (void)new;
    int i;
    FOR_NB_SCREENS(j)
    {
        for (i=0; i<Iconset_Count; i++)
        {
            struct iconset *set = &iconsets[i][j];
            if (set->bmp.data == current)
            {
                set->bmp.data = new;
                return BUFLIB_CB_OK;
            }
        }
    }
    return BUFLIB_CB_OK;
}

static void load_icons(const char* filename, enum Iconset iconset,
                        enum screen_type screen)
{
    static struct buflib_callbacks buflib_ops = {buflib_move_callback, NULL, NULL};
    const int bmpformat = (FORMAT_ANY|FORMAT_DITHER|FORMAT_TRANSPARENT);
    struct iconset *ic = &iconsets[iconset][screen];
    ssize_t buf_reqd;

    ic->loaded = false;
    ic->handle = CLB_ALOC_ERR;
    if (filename[0] && filename[0] != '-')
    {
        char fname[MAX_PATH];
        snprintf(fname, sizeof(fname), ICON_DIR "/%s.bmp", filename);
        ic->handle = core_load_bmp(fname, &ic->bmp, bmpformat, &buf_reqd, &buflib_ops);
        if (ic->handle != CLB_ALOC_ERR)
        {
            ic->bmp.data = core_get_data(ic->handle);
            ic->loaded = true;
        }
    }
}

void icons_init(void)
{
    int i;
    FOR_NB_SCREENS(j)
    {
        for (i=0; i<Iconset_Count; i++)
        {
            struct iconset* set = &iconsets[i][j];
            if (set->loaded && set->handle > 0)
            {
                set->handle = core_free(set->handle);
                set->loaded = false;
            }
        }
    }

    if (global_settings.show_icons)
    {
        load_icons(global_settings.icon_file, Iconset_user, SCREEN_MAIN);

        if (global_settings.viewers_icon_file[0] != '-' &&
            global_settings.viewers_icon_file[0] != '\0')
        {
            load_icons(global_settings.viewers_icon_file,
                    Iconset_viewers, SCREEN_MAIN);
            read_viewer_theme_file();
        }
#if defined(HAVE_REMOTE_LCD) && (NB_SCREENS > 1)
        load_icons(global_settings.remote_icon_file,
                Iconset_user, SCREEN_REMOTE);

        if (global_settings.remote_viewers_icon_file[0] != '-' &&
            global_settings.remote_viewers_icon_file[0] != '\0')
        {
            load_icons(global_settings.remote_viewers_icon_file,
                    Iconset_viewers, SCREEN_REMOTE);
        }
#endif
    }
}

int get_icon_width(enum screen_type screen_type)
{
    return ICON_WIDTH(screen_type);
}

int get_icon_height(enum screen_type screen_type)
{
    return ICON_HEIGHT(screen_type);
}

#if (LCD_DEPTH > 1) || defined(HAVE_REMOTE_LCD) && (LCD_REMOTE_DEPTH > 1)
int get_icon_format(enum screen_type screen)
{
    const struct bitmap *iconset;

    if (iconsets[Iconset_user][screen].loaded)
        iconset = &iconsets[Iconset_user][screen].bmp;
    else
        iconset = inbuilt_iconset[screen];

    return iconset->format;
}
#endif
