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

    if (icon == Icon_Tuner)
    {
        int m = height / 8;
        int bx = xpos + m / 2, by = ypos + m;
        int bw = width - m, bh = height - 2 * m;
        int i;

        if (is_rtl)
            bx = display->getwidth() - bx - bw;

        if (bw < 8 || bh < 8)
            return;

        /* A table radio seen front-on: an aerial, a case, a speaker grille
         * filling the left of it and a tuning scale with its knob on the
         * right. The dial is the half that says "radio" rather than
         * "speaker", so it keeps its width even at list size. Drawn rather
         * than themed - see the note in icon.h - and in nothing but the
         * list's own foreground colour, so it reads the same way in a
         * colour icon set and in a monochrome one. */
        int cy = by + bh / 3;               /* top of the case  */
        int ch = bh - bh / 3;               /* case height      */
        int in = MAX(2, bw / 10);           /* inset from case  */
        int gw = (bw - 3 * in) / 2;         /* grille width     */
        int dx = bx + 2 * in + gw;          /* dial left edge   */

        /* the aerial, leaning out of the top right corner */
        display->vline(bx + bw - 1 - in, by, cy);
        display->hline(bx + bw - 2 - in, bx + bw - 1, by);

        /* the case */
        display->drawrect(bx, cy, bw, ch);

        /* the speaker grille */
        for (i = 1; (cy + in + i * 2) < cy + ch - in; i++)
            display->hline(bx + in, bx + in + gw - 1, cy + in + i * 2);

        /* the tuning scale, as ticks along the top of the right half */
        {
            int th = MAX(2, ch / 5);
            for (i = 0; dx + i * 3 < bx + bw - in; i++)
                display->vline(dx + i * 3, cy + in, cy + in + th);

            /* and the knob under it */
            int kw = MAX(3, ch / 3);
            display->fillrect(dx, cy + ch - in - kw, kw, kw);
        }
        return;
    }

    if (icon == Icon_Book)
    {
        int m = height / 8;
        int bx = xpos + m / 2, by = ypos + m;
        int bw = width - m, bh = height - 2 * m;
        int i;

        if (is_rtl)
            bx = display->getwidth() - bx - bw;

        if (bw < 8 || bh < 6)
            return;

        /* An open book, seen from above: a cover showing round the edges,
         * two pages with lines of text, the fold between them, and a
         * ribbon. A closed book front-on is a rectangle with a stripe,
         * which at list size reads as a box - an open one does not read
         * as anything else. Drawn rather than themed - see the note in
         * icon.h - and coloured like the strip icons beside it where the
         * panel has colour. */
        int cov  = MAX(1, bh / 10);         /* cover showing under pages */
        int ph   = bh - 2 * cov;            /* page height               */
        int mid  = bx + bw / 2;             /* the fold                  */
        int dip  = MAX(1, bh / 12);         /* pages sag into the fold   */
#ifdef HAVE_LCD_COLOR
        unsigned fg = display->get_foreground();
        const unsigned cover  = LCD_RGBPACK(0x3E, 0x78, 0xC4);
        const unsigned page   = LCD_RGBPACK(0xF4, 0xF2, 0xEE);
        const unsigned fold   = LCD_RGBPACK(0x9C, 0x9A, 0x96);
        const unsigned words  = LCD_RGBPACK(0x8A, 0x88, 0x84);
        const unsigned ribbon = LCD_RGBPACK(0xE0, 0x3C, 0x31);
#define BOOK_INK(c) display->set_foreground(c)
#else
#define BOOK_INK(c) do { } while (0)
#endif

        /* cover, a little wider and lower than the pages */
        BOOK_INK(cover);
        display->fillrect(bx, by + dip + cov, bw, bh - dip - cov);

        /* pages: full height at the outer edge, lower at the fold */
        BOOK_INK(page);
        display->fillrect(bx + cov, by, mid - bx - cov, ph);
        display->fillrect(mid + 1, by, bx + bw - cov - mid - 1, ph);
#ifdef HAVE_LCD_COLOR
        BOOK_INK(cover);
#else
        display->set_drawmode(DRMODE_COMPLEMENT);
#endif
        for (i = 0; i < dip; i++)
        {
            int span = (bw / 2 - cov) * (dip - i) / (dip + 2);
            display->hline(mid - span, mid + span, by + i);
        }
#ifndef HAVE_LCD_COLOR
        display->set_drawmode(DRMODE_SOLID);
#endif

        /* the fold */
        BOOK_INK(fold);
        display->vline(mid, by + dip, by + ph - 1);

        /* the words */
        BOOK_INK(words);
        for (i = 1; i <= 3; i++)
        {
            int ly = by + dip + ((ph - dip) * i) / 4;
            if (ly >= by + ph - 1)
                continue;
            display->hline(bx + cov + 2, mid - 3, ly);
            display->hline(mid + 3, bx + bw - cov - 3, ly);
        }

        /* the ribbon, hanging out of the bottom of the right page */
        BOOK_INK(ribbon);
        {
            int rw = MAX(2, bw / 10);
            int rx = mid + (bx + bw - mid) / 2;
            display->fillrect(rx, by + dip, rw, bh - dip);
        }
#undef BOOK_INK
#ifdef HAVE_LCD_COLOR
        display->set_foreground(fg);
#endif
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
