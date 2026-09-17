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
#ifndef _GUI_COVERVIEW_H_
#define _GUI_COVERVIEW_H_

/* Lists laid out as covers. See coverview.c. */

#include "config.h"
#include <stdbool.h>
#include <stddef.h>

/* The Library View setting. Stored, so only ever append. */
enum coverview_style
{
    COVERVIEW_CLASSIC = 0,  /* the plain text list */
    COVERVIEW_THUMBS,       /* one line per item, with a small cover */
    COVERVIEW_SHELVES,      /* large cards, one per row */
    COVERVIEW_GRID,         /* two covers per row */
    COVERVIEW_CAROUSEL,     /* one large cover, its neighbours above and below */
    COVERVIEW_COUNT
};

struct gui_synclist;
struct screen;

/* Where the covers and the second lines come from. Generic, so any list of
 * things with pictures can use it (albums, tracks, playlists, audiobooks -
 * artists later). */
struct coverview_source
{
    /* Draw item's cover in the size x size square at x,y; false when it
     * has none, and the view draws a placeholder. */
    bool (*draw_cover)(struct screen *d, int item, void *data,
                       int x, int y, int size);
    /* The line under the name, or NULL for none. May be NULL. */
    const char *(*subtitle)(int item, void *data, char *buf, size_t size);
};

#ifdef HAVE_COVER_VIEWS
/* Lay the list out in style, or leave it as it is for CLASSIC. The source
 * must outlive the list. Call after gui_synclist_init(). */
void coverview_attach(struct gui_synclist *list,
                      const struct coverview_source *src, int style);
void coverview_detach(struct gui_synclist *list);

/* A source's draw_cover for lists whose items have a path (tracks,
 * playlists, image files), given a function that returns item's path. */
bool coverview_draw_path_cover(struct screen *d, const char *path,
                               int x, int y, int size);
#else
static inline void coverview_attach(struct gui_synclist *list,
                                    const struct coverview_source *src,
                                    int style)
{ (void)list; (void)src; (void)style; }
static inline void coverview_detach(struct gui_synclist *list) { (void)list; }
#endif

#endif /* _GUI_COVERVIEW_H_ */
