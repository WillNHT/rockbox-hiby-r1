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

/* Lists laid out as covers.
 *
 * The list itself is untouched: it still owns the items, the selection and
 * the keys, so every screen that uses it keeps working and the stick keeps
 * its meaning - up and down move through the items, the sideways flicks
 * are yes and no. Only the drawing is replaced, through the list's
 * callback_draw_list, with one of these layouts:
 *
 *   Thumbnails  a line per item with a small cover
 *   Shelves     a large card per row: cover, name, second line
 *   Grid        two covers per row; up and down still step one item
 *   Carousel    the selected cover large in the middle, the previous and
 *               next ones peeking in above and below
 *
 * The layouts take no item-specific knowledge; a coverview_source says
 * how to draw an item's cover and what its second line is. */

#include "config.h"

#ifdef HAVE_COVER_VIEWS

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "screen_access.h"
#include "settings.h"
#include "lang.h"
#include "list.h"
#include "line.h"
#include "viewport.h"
#include "covers.h"
#include "coverview.h"
#include "button.h"

#define MAX_STATES  4
#define MAX_TILES   16
#define PAD         8

struct cv_state
{
    struct gui_synclist *list;
    const struct coverview_source *src;
    int style;
    int top;                    /* first row on screen */
    int last_sel, last_top;     /* marquees belong to these */
};

static struct cv_state states[MAX_STATES];
/* One text viewport per thing on screen, so each keeps its own marquee. */
static struct viewport tile_vp[MAX_TILES];

static const char *item_name(struct gui_synclist *list, int item,
                             char *buf, size_t size)
{
    const char *s = list->callback_get_item_name(item, list->data, buf, size);
    if (!s)
        return "";
    if (P2ID((unsigned char *)s) > VOICEONLY_DELIMITER)
        return "";
    s = (const char *)P2STR((unsigned char *)s);
    while (*s == '\t')
        s++;
    return s;
}

static void draw_cover(struct screen *d, struct cv_state *st, int item,
                       const char *name, int x, int y, int size)
{
    if (size < 8)
        return;
    if (!st->src->draw_cover ||
        !st->src->draw_cover(d, item, st->list->data, x, y, size))
        cover_draw_placeholder(d, name, x, y, size);
}

static void set_selected_style(int cursor_style, struct line_desc *ld,
                               bool selected)
{
    ld->style = STYLE_DEFAULT;
    /* NOSTYLE is an arrow in a text list; there is no arrow here, so the
     * selection is shown as a bar. -1: no bar at all. */
    if (!selected || cursor_style < 0)
        return;
    if (cursor_style == SYNCLIST_CURSOR_INVERT)
        ld->style = STYLE_INVERT;
    else if (cursor_style == SYNCLIST_CURSOR_GRADIENT)
    {
        ld->style = STYLE_GRADIENT;
        ld->text_color = global_settings.lst_color;
        ld->line_color = global_settings.lss_color;
        ld->line_end_color = global_settings.lse_color;
    }
    else
    {
        ld->style = STYLE_COLORBAR;
        ld->text_color = global_settings.lst_color;
        ld->line_color = global_settings.lss_color;
    }
}

/* Fill a selected item's whole area in the selector colour, so the cover
 * sits on the bar with the text. */
static void fill_selected(struct screen *d, struct gui_synclist *list,
                          int x, int y, int w, int h)
{
    unsigned fg = d->get_foreground();
    if (list->cursor_style == SYNCLIST_CURSOR_INVERT)
        return;
    d->set_foreground(global_settings.lss_color);
    d->fillrect(x, y, w, h);
    d->set_foreground(fg);
}

static int centred_x(struct screen *d, const char *s, int width)
{
    int sw, sh;
    d->getstringsize((const unsigned char *)s, &sw, &sh);
    return sw < width ? (width - sw) / 2 : 0;
}

/* One or two lines of text in a box of the list's viewport. */
static void draw_text(struct screen *d, struct cv_state *st, int tile,
                      struct viewport *vp, int item, const char *name,
                      int x, int y, int w, int h, bool selected,
                      int cursor_style, bool with_subtitle, bool centre)
{
    struct viewport *tv;
    struct line_desc ld = LINE_DESC_DEFINIT;
    char sub[MAX_PATH];
    const char *subtitle = NULL;
    int fh = d->getcharheight();
    int lines, ty;

    if (tile >= MAX_TILES || w <= 0 || h <= 0 || y < 0)
        return;
    if (with_subtitle && st->src->subtitle)
        subtitle = st->src->subtitle(item, st->list->data, sub, sizeof(sub));
    lines = (subtitle && subtitle[0] && h >= 2 * fh) ? 2 : 1;

    tv = &tile_vp[tile];
    *tv = *vp;
    tv->x = vp->x + x;
    tv->y = vp->y + y;
    tv->width = MIN(w, vp->width - x);
    tv->height = MIN(h, vp->height - y);
    if (tv->height <= 0 || tv->width <= 0)
        return;
    d->set_viewport(tv);

    ty = (tv->height - lines * fh) / 2;
    if (ty < 0)
        ty = 0;
    ld.height = fh;
    set_selected_style(cursor_style, &ld, selected);
    ld.scroll = selected;
    put_line(d, centre ? centred_x(d, name, tv->width) : 0, ty, &ld,
             "$t", name);
    if (lines == 2)
    {
        if (!selected)
        {
            /* the second line is quieter */
            ld.style = STYLE_DEFAULT | STYLE_COLORED;
            ld.text_color = LCD_RGBPACK(0xa0, 0xa0, 0xa0);
        }
        put_line(d, centre ? centred_x(d, subtitle, tv->width) : 0, ty + fh,
                 &ld, "$t", subtitle);
    }
    d->set_viewport(vp);
}

/* Keep the selected row on screen; returns the first row to draw. */
static int scroll_to(struct cv_state *st, int sel_row, int rows_visible,
                     int total_rows)
{
    if (rows_visible < 1)
        rows_visible = 1;
    if (sel_row < st->top)
        st->top = sel_row;
    else if (sel_row >= st->top + rows_visible)
        st->top = sel_row - rows_visible + 1;
    if (st->top > total_rows - rows_visible)
        st->top = total_rows - rows_visible;
    if (st->top < 0)
        st->top = 0;
    return st->top;
}

static void draw_rows(struct screen *d, struct cv_state *st,
                      struct viewport *vp, int row_h, int cover, bool sub)
{
    struct gui_synclist *list = st->list;
    char buf[MAX_PATH];
    int rows = vp->height / row_h;
    int top = scroll_to(st, list->selected_item, rows, list->nb_items);
    int tile = 0;

    for (int i = top; i < list->nb_items && tile < MAX_TILES; i++, tile++)
    {
        int y = (i - top) * row_h;
        if (y >= vp->height)
            break;
        bool selected = (i == list->selected_item);
        const char *name = item_name(list, i, buf, sizeof(buf));
        int cy = y + (row_h - cover) / 2;
        int tx = PAD + cover + 2 * PAD;

        if (selected)
            fill_selected(d, list, 0, y, vp->width, row_h);
        draw_cover(d, st, i, name, PAD, cy, cover);
        draw_text(d, st, tile, vp, i, name, tx, y, vp->width - tx - PAD,
                  row_h, selected, list->cursor_style, sub, false);
    }
}

static void draw_grid(struct screen *d, struct cv_state *st,
                      struct viewport *vp, int cols)
{
    struct gui_synclist *list = st->list;
    char buf[MAX_PATH];
    int fh = d->getcharheight();
    int tile_w = vp->width / cols;
    int cover = tile_w - 3 * PAD;
    int tile_h = cover + fh + 3 * PAD;
    int rows_total = (list->nb_items + cols - 1) / cols;
    int rows = vp->height / tile_h;
    int top = scroll_to(st, list->selected_item / cols, rows, rows_total);
    int tile = 0;

    for (int i = top * cols; i < list->nb_items && tile < MAX_TILES;
         i++, tile++)
    {
        int x = (i % cols) * tile_w;
        int y = (i / cols - top) * tile_h;
        if (y >= vp->height)
            break;
        bool selected = (i == list->selected_item);
        const char *name = item_name(list, i, buf, sizeof(buf));

        if (selected)
            fill_selected(d, list, x + PAD / 2, y + PAD / 2,
                          tile_w - PAD, tile_h - PAD);
        draw_cover(d, st, i, name, x + (tile_w - cover) / 2, y + PAD, cover);
        draw_text(d, st, tile, vp, i, name, x + PAD, y + PAD + cover + PAD / 2,
                  tile_w - 2 * PAD, fh + PAD, selected, list->cursor_style,
                  false, true);
    }
}

static void draw_carousel(struct screen *d, struct cv_state *st,
                          struct viewport *vp)
{
    struct gui_synclist *list = st->list;
    char buf[MAX_PATH];
    const int fh = d->getcharheight();
    const int W = vp->width, H = vp->height;
    const int text_h = 2 * fh + PAD;
    const int sel = list->selected_item;
    const int big = MIN(W - 12 * PAD, (H - text_h - 4 * PAD) * 2 / 3);
    const int small = big * 2 / 3;
    const int peek = (H - text_h - big - 4 * PAD) / 2;  /* of each neighbour */
    const int big_y = peek + PAD;
    const int text_y = big_y + big + PAD;
    const char *name;

    st->top = sel;
    if (big < 16)
        return;

    if (sel > 0)
    {
        name = item_name(list, sel - 1, buf, sizeof(buf));
        draw_cover(d, st, sel - 1, name, (W - small) / 2, peek - small, small);
    }
    if (sel + 1 < list->nb_items)
    {
        name = item_name(list, sel + 1, buf, sizeof(buf));
        draw_cover(d, st, sel + 1, name, (W - small) / 2,
                   text_y + text_h + PAD, small);
    }

    name = item_name(list, sel, buf, sizeof(buf));
    draw_cover(d, st, sel, name, (W - big) / 2, big_y, big);
    /* No bar here: the large cover already says which one it is. The name
     * still scrolls. */
    draw_text(d, st, 0, vp, sel, name, PAD, text_y, W - 2 * PAD, text_h,
              true, -1, true, true);

    /* where we are, for a long list */
    if (list->nb_items > 1)
    {
        int bar_h = H - 2 * PAD;
        int knob = MAX(PAD, bar_h / list->nb_items);
        int ky = PAD + (bar_h - knob) * sel / (list->nb_items - 1);
        unsigned fg = d->get_foreground();
        d->set_foreground(LCD_RGBPACK(0x3a, 0x3b, 0x3d));
        d->fillrect(W - 4, PAD, 2, bar_h);
        d->set_foreground(global_settings.lss_color);
        d->fillrect(W - 5, ky, 4, knob);
        d->set_foreground(fg);
    }
}

static bool draw_list(struct screen *d, struct gui_synclist *list,
                      struct viewport *vp)
{
    struct cv_state *st = list->draw_list_data;
    if (!st || st->list != list || d->screen_type != SCREEN_MAIN ||
        d->depth < 16 || list->nb_items <= 0)
        return false;

    int fh = d->getcharheight();
    int line_h = MAX(list->line_height[d->screen_type], fh + PAD);

    if (list->selected_item >= list->nb_items)
        list->selected_item = list->nb_items - 1;
    if (list->selected_item < 0)
        list->selected_item = 0;

    /* One cover decoded per redraw, and another redraw asked for while any
     * are left: the list answers the key at once and the pictures come in
     * behind it, instead of every step of a scroll waiting on a file read
     * and a JPEG decode for each new cover (#79). */
    covers_set_budget(1);
#ifdef HAVE_TOUCHSCREEN
    /* Under a moving thumb not even one: a decode is a frame the drag
     * waits on. Cached covers still draw; the rest fill in once the list
     * settles (scroll_mode goes back to 0, SCROLL_NONE). */
    if (list->scroll_mode != 0)
        covers_set_budget(0);
#endif

    /* marquees belong to the rows that were there */
    if (st->last_sel != list->selected_item || st->last_top != st->top)
    {
        for (int i = 0; i < MAX_TILES; i++)
            if (tile_vp[i].width > 0)
                d->scroll_stop_viewport(&tile_vp[i]);
    }

    switch (st->style)
    {
    case COVERVIEW_THUMBS:
    {
        int row_h = MAX(line_h, fh + 3 * PAD);
        draw_rows(d, st, vp, row_h, row_h - PAD, false);
        break;
    }
    case COVERVIEW_GRID:
        draw_grid(d, st, vp, 2);
        break;
    case COVERVIEW_CAROUSEL:
        draw_carousel(d, st, vp);
        break;
    case COVERVIEW_SHELVES:
    default:
    {
        /* 35% of the panel rather than 30%: at a 44 px face the name
         * was taller than the picture beside it, which is the wrong way
         * round for a view whose whole point is the picture. */
        int row_h = MAX(2 * fh + 3 * PAD, vp->width * 7 / 20);
        draw_rows(d, st, vp, row_h, row_h - 2 * PAD, true);
        break;
    }
    }

    d->set_viewport(vp);
    st->last_sel = list->selected_item;
    st->last_top = st->top;
    /* Mid-scroll the release or the end of the coast redraws anyway. */
    if (covers_deferred()
#ifdef HAVE_TOUCHSCREEN
        && list->scroll_mode == 0
#endif
        )
        button_queue_post(BUTTON_REDRAW, 0);
    covers_set_budget(-1);
    return true;
}

void coverview_attach(struct gui_synclist *list,
                      const struct coverview_source *src, int style)
{
    struct cv_state *st = NULL;

    coverview_detach(list);
    if (style <= COVERVIEW_CLASSIC || style >= COVERVIEW_COUNT || !src)
        return;
    /* A list on the stack can go away without detaching, so a state's list
     * pointer is never followed from here: a free state is used if there
     * is one, and otherwise the least recently attached one is taken. A
     * list whose state was taken sees st->list != itself and falls back
     * to plain rows. */
    static unsigned next_steal;
    for (int i = 0; i < MAX_STATES; i++)
        if (states[i].list == list)     /* re-initialised, not detached */
            states[i].list = NULL;
    for (int i = 0; i < MAX_STATES; i++)
    {
        if (!states[i].list)
        {
            st = &states[i];
            break;
        }
    }
    if (!st)
        st = &states[next_steal++ % MAX_STATES];

    memset(st, 0, sizeof(*st));
    st->list = list;
    st->src = src;
    st->style = style;
    st->last_sel = -1;
    list->draw_list_data = st;
    list->callback_draw_list = draw_list;
}

void coverview_detach(struct gui_synclist *list)
{
    if (list->callback_draw_list != draw_list)
        return;
    struct cv_state *st = list->draw_list_data;
    if (st)
        st->list = NULL;
    list->callback_draw_list = NULL;
    list->draw_list_data = NULL;
}

bool coverview_draw_path_cover(struct screen *d, const char *path,
                               int x, int y, int size)
{
    return cover_draw(d, path, x, y, size);
}

#endif /* HAVE_COVER_VIEWS */
