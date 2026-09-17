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
/*
 * The Audiobooks screens.
 *
 *   root       a "Continue listening" card, then Library, Authors, Series,
 *              Narrators, Recently Added, In Progress, Finished, Sleep
 *              Timer, Rescan and Settings
 *   book list  three lines a book: cover, title, author, progress and the
 *              time left
 *   book page  a card with the cover and the facts, then what can be done
 *              with the book
 *
 * Covers are the books' own pictures, scaled when a row first shows and
 * kept in a small cache while the screens are open.
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "action.h"
#include "lcd.h"
#include "font.h"
#include "screens.h"
#include "screen_access.h"
#include "list.h"
#include "line.h"
#include "menu.h"
#include "viewport.h"
#include "scrollbar.h"
#include "splash.h"
#include "yesno.h"
#include "lang.h"
#include "language.h"
#include "talk.h"
#include "misc.h"
#include "settings.h"
#include "audio.h"
#include "bookmark.h"
#include "root_menu.h"
#include "pathfuncs.h"
#include "string-extra.h"
#include "strnatcmp.h"
#include "core_alloc.h"
#include "powermgmt.h"
#include "metadata.h"
#include "cuesheet.h"
#include "rbpaths.h"
#include "pitchscreen.h"
#include "skin_engine/skin_engine.h"
#ifdef HAVE_ALBUMART
#include "bmp.h"
#include "jpeg_load.h"
#ifdef HAVE_PNG
#include "png_load.h"
#endif
#endif
#include "abdb.h"
#include "audiobooks.h"

#define LIST_LINES      3       /* lines a book row takes */
#define MAX_THUMB       128
#define MAX_CARD_COVER  256
#define THUMB_SLOTS     12

/* ====================================================================== */
/* covers                                                                 */
/* ====================================================================== */

#ifdef HAVE_ALBUMART
struct cover_slot
{
    uint32_t id;
    int      size;       /* the square asked for */
    int      w, h;       /* what the picture came out as */
    long     used;
    bool     missing;
};

static struct cover_slot slots[THUMB_SLOTS + 1]; /* the last is the card's */
static long cover_clock;
static int  cover_handle = -1;     /* THUMB_SLOTS thumbs, then the card */
static int  decode_handle = -1;
static size_t decode_size;

#define THUMB_BYTES  (MAX_THUMB * MAX_THUMB * sizeof(fb_data))
#define CARD_BYTES   (MAX_CARD_COVER * MAX_CARD_COVER * sizeof(fb_data))

/* Once, at boot: taken later, the memory would come out of the audio
 * buffer, and shrinking that restarts playback. */
static bool covers_open(void)
{
    if (cover_handle > 0)
        return true;
    for (int i = 0; i <= THUMB_SLOTS; i++)
        slots[i].id = 0;

    cover_handle = core_alloc(THUMB_SLOTS * THUMB_BYTES + CARD_BYTES);
    decode_size = CARD_BYTES + JPEG_DECODE_OVERHEAD + 256 * 1024;
    decode_handle = core_alloc(decode_size);
    if (cover_handle <= 0 || decode_handle <= 0)
    {
        if (cover_handle > 0)
            core_free(cover_handle);
        if (decode_handle > 0)
            core_free(decode_handle);
        cover_handle = decode_handle = -1;
        return false;
    }
    core_pin(cover_handle);
    core_pin(decode_handle);
    return true;
}

static bool decode_cover(const struct ab_book *b, int size, struct bitmap *bm)
{
    const char *path = ab_db_str(b, AB_S_COVER);
    if (!path[0])
        return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    const int format = FORMAT_NATIVE | FORMAT_DITHER |
                       FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    memset(bm, 0, sizeof *bm);
    bm->width = size;
    bm->height = size;
    bm->data = core_get_data(decode_handle);
    int rc = -1;
    size_t len = strlen(path);

    if (b->cover_size)
    {
#ifdef HAVE_PNG
        if (b->cover_type == AA_TYPE_PNG)
            rc = clip_png_fd(fd, b->cover_pos, b->cover_size, bm,
                             decode_size, format);
        else
#endif
#ifdef HAVE_JPEG
        if (b->cover_type != AA_TYPE_BMP)
        {
            lseek(fd, b->cover_pos, SEEK_SET);
            rc = clip_jpeg_fd(fd, b->cover_type, b->cover_size, bm,
                              decode_size, format, NULL, NULL);
        }
#endif
    }
#ifdef HAVE_PNG
    else if (len > 4 && !strcasecmp(path + len - 4, ".png"))
        rc = read_png_fd(fd, bm, decode_size, format);
#endif
#ifdef HAVE_JPEG
    else
        rc = read_jpeg_fd(fd, 0, bm, decode_size, format, NULL, NULL);
#endif
    close(fd);
    (void)len;
    return rc > 0 && bm->width > 0 && bm->height > 0 &&
           bm->width <= size && bm->height <= size;
}

/* A book's cover as a bitmap no bigger than size x size, or NULL. */
static const struct bitmap *cover_get(const struct ab_book *b, int size,
                                      bool card)
{
    static struct bitmap out;
    if (!b || cover_handle <= 0)
        return NULL;
    int max = card ? MAX_CARD_COVER : MAX_THUMB;
    if (size > max)
        size = max;
    if (size < 8)
        return NULL;

    int slot = -1;
    if (card)
    {
        slot = THUMB_SLOTS;
        if (slots[slot].id != b->id || slots[slot].size != size)
            slot = -slot - 1;
    }
    else
    {
        int victim = 0;
        for (int i = 0; i < THUMB_SLOTS; i++)
        {
            if (slots[i].id == b->id && slots[i].size == size)
            {
                slot = i;
                break;
            }
            if (slots[i].used < slots[victim].used)
                victim = i;
        }
        if (slot < 0)
            slot = -victim - 1;
    }

    if (slot < 0)
    {
        /* not cached yet */
        slot = -slot - 1;
        struct bitmap bm;
        slots[slot].id = b->id;
        slots[slot].size = size;
        slots[slot].missing = !decode_cover(b, size, &bm);
        if (!slots[slot].missing)
        {
            slots[slot].w = bm.width;
            slots[slot].h = bm.height;
            unsigned char *dst = (unsigned char *)core_get_data(cover_handle) +
                                 (card ? THUMB_SLOTS * THUMB_BYTES
                                       : slot * THUMB_BYTES);
            memcpy(dst, bm.data, (size_t)bm.width * bm.height * sizeof(fb_data));
        }
    }
    slots[slot].used = ++cover_clock;
    if (slots[slot].missing)
        return NULL;

    memset(&out, 0, sizeof out);
    out.width = slots[slot].w;
    out.height = slots[slot].h;
    out.format = FORMAT_NATIVE;
    out.data = (unsigned char *)core_get_data(cover_handle) +
               (card ? THUMB_SLOTS * THUMB_BYTES : slot * THUMB_BYTES);
    return &out;
}

/* Draws the cover centred in a size x size square; a book outline when
 * there is none. */
static void draw_cover(struct screen *d, const struct ab_book *b,
                       int x, int y, int size, bool card)
{
    const struct bitmap *bm = d->screen_type == SCREEN_MAIN && d->depth > 1 ?
                              cover_get(b, size, card) : NULL;
    if (bm)
    {
        d->bmp(bm, x + (size - bm->width) / 2, y + (size - bm->height) / 2);
        return;
    }
    /* a plain placeholder: an outlined book */
    int m = size / 6;
    d->drawrect(x + m, y + m / 2, size - 2 * m, size - m);
    d->vline(x + m + m / 2, y + m / 2, y + size - m / 2 - 1);
}
#else
static bool covers_open(void) { return false; }
static void draw_cover(struct screen *d, const struct ab_book *b,
                       int x, int y, int size, bool card)
{
    (void)d; (void)b; (void)x; (void)y; (void)size; (void)card;
}
#endif /* HAVE_ALBUMART */

/* ====================================================================== */
/* text helpers                                                           */
/* ====================================================================== */

static const char *or_unknown(const char *s)
{
    return s && s[0] ? s : (const char *)str(LANG_AB_UNKNOWN);
}

static void format_duration(char *buf, size_t size, unsigned long ms)
{
    unsigned long min = ms / 60000;
    if (min >= 60)
        snprintf(buf, size, "%luh %02lum", min / 60, min % 60);
    else
        snprintf(buf, size, "%lum", min ? min : (ms ? 1 : 0));
}

/* "42% · 3h 10m left", "Finished", "7h 02m" */
static void progress_text(const struct ab_book *b, char *buf, size_t size)
{
    const struct ab_progress *p = ab_progress_get(b->id);
    char dur[24];
    if (p && p->finished)
        strmemccpy(buf, str(LANG_AB_BOOK_FINISHED), size);
    else if (p && p->position > 0 && b->length)
    {
        unsigned long left = p->position < b->length ?
                             b->length - p->position : 0;
#ifdef HAVE_PITCHCONTROL
        /* time left at the book's own speed */
        if (p->speed > 0)
            left = (uint64_t)left * PITCH_SPEED_100 / p->speed;
#endif
        format_duration(dur, sizeof dur, left);
        snprintf(buf, size, "%d%% \xc2\xb7 %s %s",
                 ab_book_permille(b, p) / 10, dur, str(LANG_AB_LEFT));
    }
    else
    {
        format_duration(dur, sizeof dur, b->length);
        strmemccpy(buf, dur, size);
    }
}

static void title_text(const struct ab_book *b, char *buf, size_t size)
{
    const char *series = ab_db_str(b, AB_S_SERIES);
    const char *no = ab_db_str(b, AB_S_SERIES_NO);
    if (series[0] && no[0])
        snprintf(buf, size, "%s (%s #%s)",
                 or_unknown(ab_db_str(b, AB_S_TITLE)), series, no);
    else
        strmemccpy(buf, or_unknown(ab_db_str(b, AB_S_TITLE)), size);
}

static void draw_progress_bar(struct screen *d, int x, int y, int w, int h,
                              const struct ab_book *b)
{
    int pm = ab_book_permille(b, ab_progress_get(b->id));
    if (w > 4 && h > 0)
        gui_scrollbar_draw(d, x, y, w, h, 1000, 0, pm, HORIZONTAL);
}

/* ====================================================================== */
/* book lists                                                             */
/* ====================================================================== */

enum list_kind
{
    LIST_LIBRARY,
    LIST_RECENT,
    LIST_IN_PROGRESS,
    LIST_FINISHED,
    LIST_GROUP,      /* books sharing one author/series/narrator */
};

static int16_t book_order[AB_MAX_BOOKS];
static enum ab_str sort_field;

static int cmp_str_field(const struct ab_book *a, const struct ab_book *b,
                         enum ab_str f)
{
    return strnatcasecmp(ab_db_str(a, f), ab_db_str(b, f));
}

static int cmp_library(const void *pa, const void *pb)
{
    const struct ab_book *a = ab_db_book(*(const int16_t *)pa);
    const struct ab_book *b = ab_db_book(*(const int16_t *)pb);
    int r;
    if (sort_field != AB_S_PATH && (r = cmp_str_field(a, b, sort_field)))
        return r;
    if ((r = cmp_str_field(a, b, AB_S_AUTHOR)) ||
        (r = cmp_str_field(a, b, AB_S_SERIES)) ||
        (r = cmp_str_field(a, b, AB_S_SERIES_NO)) ||
        (r = cmp_str_field(a, b, AB_S_TITLE)))
        return r;
    return cmp_str_field(a, b, AB_S_PATH);
}

static int cmp_recent(const void *pa, const void *pb)
{
    const struct ab_book *a = ab_db_book(*(const int16_t *)pa);
    const struct ab_book *b = ab_db_book(*(const int16_t *)pb);
    if (a->added != b->added)
        return a->added > b->added ? -1 : 1;
    return cmp_library(pa, pb);
}

static int cmp_played(const void *pa, const void *pb)
{
    const struct ab_book *a = ab_db_book(*(const int16_t *)pa);
    const struct ab_book *b = ab_db_book(*(const int16_t *)pb);
    const struct ab_progress *x = ab_progress_get(a->id);
    const struct ab_progress *y = ab_progress_get(b->id);
    uint32_t ta = x ? x->last_played : 0, tb = y ? y->last_played : 0;
    if (ta != tb)
        return ta > tb ? -1 : 1;
    return cmp_library(pa, pb);
}

/* Fills book_order; returns the count. */
static int build_list(enum list_kind kind, enum ab_str field, const char *value)
{
    int n = 0, total = ab_db_count();
    for (int i = 0; i < total; i++)
    {
        const struct ab_book *b = ab_db_book(i);
        const struct ab_progress *p = ab_progress_get(b->id);
        bool keep = true;
        switch (kind)
        {
            case LIST_IN_PROGRESS:
                keep = p && !p->finished && p->position > 0;
                break;
            case LIST_FINISHED:
                keep = p && p->finished;
                break;
            case LIST_GROUP:
                keep = !strcasecmp(ab_db_str(b, field), value);
                break;
            default:
                break;
        }
        if (keep)
            book_order[n++] = i;
    }

    sort_field = AB_S_PATH;
    switch (kind)
    {
        case LIST_RECENT:
            qsort(book_order, n, sizeof *book_order, cmp_recent);
            break;
        case LIST_IN_PROGRESS:
        case LIST_FINISHED:
            qsort(book_order, n, sizeof *book_order, cmp_played);
            break;
        case LIST_GROUP:
            /* a series in reading order; everything else by title */
            sort_field = field == AB_S_SERIES ? AB_S_SERIES_NO : AB_S_TITLE;
            /* fall through */
        default:
            qsort(book_order, n, sizeof *book_order, cmp_library);
            break;
    }
    return n;
}

static const char *book_row_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    const struct ab_book *b = ab_db_book(book_order[item / LIST_LINES]);
    if (!b)
        return "";
    switch (item % LIST_LINES)
    {
        case 0:  title_text(b, buf, size); break;
        case 1:  strmemccpy(buf, or_unknown(ab_db_str(b, AB_S_AUTHOR)), size); break;
        default: progress_text(b, buf, size); break;
    }
    return buf;
}

static int thumb_size(int line_height)
{
    int s = LIST_LINES * line_height - 6;
    return s > MAX_THUMB ? MAX_THUMB : s;
}

static void draw_book_row(struct list_putlineinfo_t *li)
{
    struct screen *d = li->display;
    if (li->is_title)
    {
        d->put_line(li->x, li->y, li->linedes, "$t", li->dsp_text);
        return;
    }

    int item = li->line;
    int sub = item % LIST_LINES;
    const struct ab_book *b = ab_db_book(book_order[item / LIST_LINES]);
    if (!b)
        return;
    int h = li->linedes->height;
    if (h <= 0)
        h = font_get(li->vp->font)->height;
    int thumb = thumb_size(h);
    int tx = li->x + thumb + 8;
    char buf[MAX_PATH];

    struct line_desc ld = *li->linedes;
    ld.scroll = sub == 0 && li->is_selected;
    book_row_name(item, NULL, buf, sizeof buf);

    if (sub == LIST_LINES - 1)
    {
        /* the bar sits under the time left */
        int barh = MAX(3, h / 8);
        d->put_line(tx, li->y, &ld, "$t", buf);
        draw_progress_bar(d, tx, li->y + h - barh - 1,
                          li->vp->width - tx - 4, barh, b);
    }
    else
        d->put_line(tx, li->y, &ld, "$t", buf);

    /* The cover spans the book's lines; draw it with the first line that
     * is on screen. */
    int first_shown = li->list->start_item[d->screen_type];
    if (sub == 0 || item == first_shown)
        draw_cover(d, b, li->x + 2, li->y - sub * h + 3, thumb, false);
}

static int book_page(int book);

/* Returns a GO_TO_* value to leave the Audiobooks screens with, or 0. */
static int book_list_screen(const char *title, enum list_kind kind,
                            enum ab_str field, const char *value)
{
    struct gui_synclist lists;
    int count = build_list(kind, field, value);
    if (count == 0)
    {
        splashf(HZ, "%s", str(LANG_AB_EMPTY));
        return 0;
    }

    char title_buf[MAX_PATH];
    strmemccpy(title_buf, title, sizeof title_buf);
    int selected = 0;

    while (1)
    {
        gui_synclist_init(&lists, book_row_name, NULL, false, LIST_LINES, NULL);
        lists.callback_draw_item = draw_book_row;
        gui_synclist_set_title(&lists, title_buf, Icon_Book);
        gui_synclist_set_icon_callback(&lists, NULL);
        gui_synclist_set_nb_items(&lists, count * LIST_LINES);
        gui_synclist_select_item(&lists, selected * LIST_LINES);
        gui_synclist_draw(&lists);
        gui_synclist_speak_item(&lists);

        int action;
        bool redo = false;
        while (!redo)
        {
            list_do_action(CONTEXT_TREE, HZ, &lists, &action);
            selected = gui_synclist_get_sel_pos(&lists) / LIST_LINES;
            switch (action)
            {
                case ACTION_STD_OK:
                case ACTION_STD_CONTEXT:
                {
                    int ret = book_page(book_order[selected]);
                    if (ret)
                        return ret;
                    /* the page may have changed what belongs here */
                    count = build_list(kind, field, value);
                    if (count == 0)
                        return 0;
                    if (selected >= count)
                        selected = count - 1;
                    redo = true;
                    break;
                }
                case ACTION_STD_CANCEL:
                    return 0;
                case ACTION_NONE:
                    break;
                default:
                    if (default_event_handler(action) == SYS_USB_CONNECTED)
                        return GO_TO_ROOT;
                    break;
            }
        }
    }
}

/* ---- Authors / Series / Narrators ------------------------------------ */

static int16_t group_first[AB_MAX_BOOKS];  /* a book with that value */
static int16_t group_count[AB_MAX_BOOKS];
static int ngroups;
static enum ab_str group_field;

static int cmp_group(const void *pa, const void *pb)
{
    const struct ab_book *a = ab_db_book(*(const int16_t *)pa);
    const struct ab_book *b = ab_db_book(*(const int16_t *)pb);
    return strnatcasecmp(ab_db_str(a, group_field), ab_db_str(b, group_field));
}

static void build_groups(enum ab_str field)
{
    int total = ab_db_count(), n = 0;
    group_field = field;
    for (int i = 0; i < total; i++)
        if (ab_db_str(ab_db_book(i), field)[0])
            book_order[n++] = i;
    qsort(book_order, n, sizeof *book_order, cmp_group);

    ngroups = 0;
    for (int i = 0; i < n; i++)
    {
        const char *v = ab_db_str(ab_db_book(book_order[i]), field);
        if (ngroups && !strcasecmp(v,
                ab_db_str(ab_db_book(group_first[ngroups - 1]), field)))
        {
            group_count[ngroups - 1]++;
            continue;
        }
        group_first[ngroups] = book_order[i];
        group_count[ngroups] = 1;
        ngroups++;
    }
}

static const char *group_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    const char *v = ab_db_str(ab_db_book(group_first[item]), group_field);
    snprintf(buf, size, "%s (%d)", v, group_count[item]);
    return buf;
}

static int group_screen(int title_id, enum ab_str field)
{
    struct simplelist_info info;
    build_groups(field);
    if (ngroups == 0)
    {
        splashf(HZ, "%s", str(LANG_AB_EMPTY));
        return 0;
    }

    int selection = 0;
    while (1)
    {
        build_groups(field);
        if (ngroups == 0)
            return 0;
        simplelist_info_init(&info, str(title_id), ngroups, NULL);
        info.get_name = group_name;
        info.title_icon = Icon_Book;
        info.selection = MIN(selection, ngroups - 1);
        simplelist_show_list(&info);
        if (info.selection < 0)
            return 0;
        selection = info.selection;

        char value[MAX_PATH];
        strmemccpy(value, ab_db_str(ab_db_book(group_first[selection]), field),
                   sizeof value);
        int ret = book_list_screen(value, LIST_GROUP, field, value);
        if (ret)
            return ret;
    }
}

/* ====================================================================== */
/* cards: a picture and facts above a list                                */
/* ====================================================================== */

struct card_screen
{
    const char *title;
    int  book;              /* the book the card shows, -1 for none */
    int  (*current_book)(void *data);   /* NULL: book never changes */
    bool root;              /* "Continue listening" style */
    int  (*count)(void *data);
    const char *(*name)(int item, void *data, char *buf, size_t size);
    /* returns a GO_TO_* value to leave with, or 0 to stay */
    int  (*select)(int item, void *data);
    void *data;
};

static int card_height(const struct viewport *ui)
{
    int h = ui->height * 2 / 5;
    int w = ui->width / 2;
    return MIN(h, w);
}

static void draw_card(const struct card_screen *cs, struct viewport *vp)
{
    struct screen *d = &screens[SCREEN_MAIN];
    const struct ab_book *b = ab_db_book(cs->book);
    struct viewport *last = d->set_viewport(vp);
    d->clear_viewport();
    if (!b)
    {
        d->update_viewport();
        d->set_viewport(last);
        return;
    }

    int pad = 6;
    int size = MIN(vp->height - 2 * pad, MAX_CARD_COVER);
    draw_cover(d, b, pad, pad, size, true);

    int x = size + 2 * pad;
    int lh = font_get(vp->font)->height;
    int y = pad;
    char buf[MAX_PATH];

    struct viewport text = *vp;
    text.x += x;
    text.width -= x + pad;
    d->set_viewport(&text);

    if (cs->root)
    {
        d->putsxy(0, y, str(LANG_AB_CONTINUE));
        y += lh + lh / 3;
    }
    title_text(b, buf, sizeof buf);
    d->putsxy(0, y, buf);
    y += lh;
    d->putsxy(0, y, or_unknown(ab_db_str(b, AB_S_AUTHOR)));
    y += lh;

    if (!cs->root)
    {
        const char *nar = ab_db_str(b, AB_S_NARRATOR);
        if (nar[0] && y + 3 * lh < vp->height)
        {
            snprintf(buf, sizeof buf, "%s: %s", str(LANG_AB_NARRATOR), nar);
            d->putsxy(0, y, buf);
            y += lh;
        }
        char dur[24];
        format_duration(dur, sizeof dur, b->length);
        if (y + 3 * lh < vp->height)
        {
            snprintf(buf, sizeof buf, "%s: %s \xc2\xb7 %s: %d",
                     str(LANG_AB_LENGTH), dur, str(LANG_AB_FILES), b->nfiles);
            d->putsxy(0, y, buf);
        }
    }

    /* the bottom: time left and the bar */
    int barh = MAX(4, lh / 5);
    int by = vp->height - pad - barh;
    progress_text(b, buf, sizeof buf);
    d->putsxy(0, by - lh - 2, buf);
    draw_progress_bar(d, 0, by, text.width, barh, b);

    d->set_viewport(vp);
    d->update_viewport();
    d->set_viewport(last);
}

static const char *card_list_name(int item, void *data, char *buf, size_t size)
{
    const struct card_screen *cs = data;
    const char *s = cs->name(item, cs->data, buf, size);
    return P2STR((unsigned char *)s);
}

static int card_screen_run(struct card_screen *cs)
{
    struct gui_synclist lists;
    struct viewport parent[NB_SCREENS];
    struct viewport card_vp;
    int selected = 0;

    while (1)
    {
        FOR_NB_SCREENS(i)
            viewport_set_defaults(&parent[i], i);
        if (cs->current_book)
            cs->book = cs->current_book(cs->data);

        bool card = cs->book >= 0 && screens[SCREEN_MAIN].depth > 1;
        card_vp = parent[SCREEN_MAIN];
        if (card)
        {
            int ch = card_height(&parent[SCREEN_MAIN]);
            card_vp.height = ch;
            parent[SCREEN_MAIN].y += ch;
            parent[SCREEN_MAIN].height -= ch;
        }

        int count = cs->count(cs->data);
        gui_synclist_init(&lists, card_list_name, cs, false, 1, parent);
        /* the card already says what this is */
        if (!card)
            gui_synclist_set_title(&lists, (char *)cs->title, Icon_Book);
        gui_synclist_set_nb_items(&lists, count);
        gui_synclist_select_item(&lists, MIN(selected, count - 1));
        if (card)
            draw_card(cs, &card_vp);
        gui_synclist_draw(&lists);
        gui_synclist_speak_item(&lists);

        bool redo = false;
        while (!redo)
        {
            int action;
            list_do_action(CONTEXT_TREE, HZ, &lists, &action);
            selected = gui_synclist_get_sel_pos(&lists);
            switch (action)
            {
                case ACTION_STD_OK:
                {
                    int ret = cs->select(selected, cs->data);
                    if (ret)
                        return ret;
                    redo = true;
                    break;
                }
                case ACTION_STD_CANCEL:
                    return 0;
                case ACTION_NONE:
                    /* progress moves while a book plays */
                    if (card && (audio_status() & AUDIO_STATUS_PLAY))
                        draw_card(cs, &card_vp);
                    break;
                default:
                    if (default_event_handler(action) == SYS_USB_CONNECTED)
                        return GO_TO_ROOT;
                    /* a theme or status bar change moves things */
                    if (action == ACTION_REDRAW)
                        redo = true;
                    break;
            }
        }
    }
}

/* ====================================================================== */
/* sleep timer                                                            */
/* ====================================================================== */

static const int sleep_minutes[] = { 0, 15, 30, 45, 60, 90, 120, -1 };

static const char *sleep_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    int m = sleep_minutes[item];
    if (m == 0)
        return str(LANG_OFF);
    if (m < 0)
        return str(LANG_AB_END_OF_CHAPTER);
    snprintf(buf, size, "%d min", m);
    return buf;
}

static void sleep_menu(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_SLEEP_TIMER),
                         ARRAYLEN(sleep_minutes), NULL);
    info.get_name = sleep_name;
    info.selection = audiobooks_sleep_at_chapter_end() ?
                     (int)ARRAYLEN(sleep_minutes) - 1 : 0;
    simplelist_show_list(&info);
    if (info.selection < 0)
        return;
    audiobooks_set_sleep(sleep_minutes[info.selection]);
}

static void sleep_status(char *buf, size_t size)
{
    if (audiobooks_sleep_at_chapter_end())
        snprintf(buf, size, "%s: %s", str(LANG_SLEEP_TIMER),
                 str(LANG_AB_END_OF_CHAPTER));
    else if (get_sleep_timer_active())
    {
        int s = get_sleep_timer();
        snprintf(buf, size, "%s: %d:%02d", str(LANG_SLEEP_TIMER),
                 s / 60, s % 60);
    }
    else
        snprintf(buf, size, "%s: %s", str(LANG_SLEEP_TIMER), str(LANG_OFF));
}

/* ====================================================================== */
/* chapters                                                               */
/* ====================================================================== */

static struct
{
    int count;
    bool cue;
    uint32_t offset[256];     /* cue: ms into the file */
    char     title[256][48];
    int      current;
} chapters;

static void basename_noext(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    strmemccpy(out, slash ? slash + 1 : path, size);
    char *dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = '\0';
}

static void load_chapters(int book)
{
    const struct ab_book *b = ab_db_book(book);
    const struct ab_progress *p = ab_progress_get(b->id);
    chapters.count = 0;
    chapters.cue = false;
    chapters.current = p ? p->file : 0;

    if (b->single)
    {
        static struct mp3entry id3;
        struct cuesheet_file cf;
        const char *path = ab_db_str(b, AB_S_PATH);
        if (get_metadata(&id3, -1, path) && look_for_cuesheet_file(&id3, &cf))
        {
            int h = core_alloc(sizeof(struct cuesheet));
            if (h > 0)
            {
                core_pin(h);
                struct cuesheet *cue = core_get_data(h);
                if (parse_cuesheet(&cf, cue))
                {
                    int n = MIN(cue->track_count, (int)ARRAYLEN(chapters.offset));
                    uint32_t pos = p ? p->elapsed : 0;
                    chapters.current = 0;
                    for (int i = 0; i < n; i++)
                    {
                        struct cue_track_info *t = get_cue_track(cue, i);
                        chapters.offset[i] = t->offset;
                        strmemccpy(chapters.title[i],
                                   get_cue_track_title(cue, t),
                                   sizeof chapters.title[i]);
                        if (!chapters.title[i][0])
                            snprintf(chapters.title[i], sizeof chapters.title[i],
                                     "%d", i + 1);
                        if (t->offset <= pos)
                            chapters.current = i;
                    }
                    chapters.count = n;
                    chapters.cue = true;
                }
                core_unpin(h);
                core_free(h);
            }
        }
        if (chapters.cue)
            return;
    }

    ab_lock();
    const char **names;
    int n = ab_db_list_files(ab_db_str(b, AB_S_PATH), b->single, &names);
    n = MIN(n, (int)ARRAYLEN(chapters.title));
    for (int i = 0; i < n; i++)
    {
        basename_noext(names[i], chapters.title[i], sizeof chapters.title[i]);
        chapters.offset[i] = 0;
    }
    ab_unlock();
    chapters.count = n;
}

static const char *chapter_name(int item, void *data, char *buf, size_t size)
{
    const struct ab_book *b = data;
    char dur[16];
    unsigned long ms;
    if (chapters.cue)
        ms = chapters.offset[item];
    else
    {
        ms = item < b->nfiles ? ab_db_lengths(b)[item] : 0;
    }
    format_time(dur, sizeof dur, ms);
    snprintf(buf, size, "%s%s  %s", item == chapters.current ? "> " : "",
             chapters.title[item], dur);
    return buf;
}

static int chapters_screen(int book)
{
    struct simplelist_info info;
    load_chapters(book);
    if (chapters.count == 0)
    {
        splashf(HZ, "%s", str(LANG_AB_EMPTY));
        return 0;
    }
    const struct ab_book *b = ab_db_book(book);
    simplelist_info_init(&info, str(LANG_AB_CHAPTERS), chapters.count,
                         (void *)b);
    info.get_name = chapter_name;
    info.selection = chapters.current;
    simplelist_show_list(&info);
    if (info.selection < 0)
        return 0;
    bool ok = chapters.cue ?
              audiobooks_play_at(book, 0, chapters.offset[info.selection]) :
              audiobooks_play_at(book, info.selection, 0);
    return ok ? GO_TO_WPS : 0;
}

/* ====================================================================== */
/* book page                                                              */
/* ====================================================================== */

enum page_item
{
    PAGE_RESUME,
    PAGE_START_OVER,
    PAGE_CHAPTERS,
    PAGE_BOOKMARKS,
    PAGE_SPEED,
    PAGE_FINISHED,
    PAGE_SLEEP,
    PAGE_DESCRIPTION,
};

struct page
{
    int book;
    int items[8];
    int count;
    char bookmark_file[MAX_PATH];
};

static void page_build(struct page *pg)
{
    const struct ab_book *b = ab_db_book(pg->book);
    const struct ab_progress *p = ab_progress_get(b->id);
    pg->count = 0;
    pg->items[pg->count++] = PAGE_RESUME;
    if (p && (p->position > 0 || p->finished))
        pg->items[pg->count++] = PAGE_START_OVER;
    pg->items[pg->count++] = PAGE_CHAPTERS;

    /* the bookmark file of the folder the book's playlist is made from */
    char dir[MAX_PATH];
    strmemccpy(dir, ab_db_str(b, AB_S_PATH), sizeof dir);
    if (b->single)
    {
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[slash == dir ? 1 : 0] = '\0';
    }
    if (!strcmp(dir, "/"))
        strcpy(pg->bookmark_file, "/root_dir.bmark");
    else
    {
        strmemccpy(pg->bookmark_file, dir, sizeof pg->bookmark_file);
        strlcat(pg->bookmark_file, ".bmark", sizeof pg->bookmark_file);
    }
    if (file_exists(pg->bookmark_file))
        pg->items[pg->count++] = PAGE_BOOKMARKS;

#ifdef HAVE_PITCHCONTROL
    pg->items[pg->count++] = PAGE_SPEED;
#endif
    pg->items[pg->count++] = PAGE_FINISHED;
    pg->items[pg->count++] = PAGE_SLEEP;
    if (ab_db_str(b, AB_S_DESC)[0])
        pg->items[pg->count++] = PAGE_DESCRIPTION;
}

static int page_count(void *data)
{
    struct page *pg = data;
    page_build(pg);
    return pg->count;
}

static const char *page_name(int item, void *data, char *buf, size_t size)
{
    struct page *pg = data;
    const struct ab_book *b = ab_db_book(pg->book);
    const struct ab_progress *p = ab_progress_get(b->id);
    switch (pg->items[item])
    {
        case PAGE_RESUME:
            return str(p && p->position > 0 && !p->finished ?
                       LANG_AB_RESUME : LANG_AB_PLAY);
        case PAGE_START_OVER:  return str(LANG_AB_START_OVER);
        case PAGE_CHAPTERS:    return str(LANG_AB_CHAPTERS);
        case PAGE_BOOKMARKS:   return str(LANG_AB_BOOKMARKS);
#ifdef HAVE_PITCHCONTROL
        case PAGE_SPEED:
        {
            int32_t sp = p && p->speed ? p->speed : PITCH_SPEED_100;
            snprintf(buf, size, "%s: %ld.%02ldx", str(LANG_AB_SPEED),
                     (long)(sp / PITCH_SPEED_100),
                     (long)((sp % PITCH_SPEED_100) * 100 / PITCH_SPEED_100));
            return buf;
        }
#endif
        case PAGE_FINISHED:
            return str(p && p->finished ? LANG_AB_MARK_UNFINISHED
                                        : LANG_AB_MARK_FINISHED);
        case PAGE_SLEEP:
            sleep_status(buf, size);
            return buf;
        case PAGE_DESCRIPTION: return str(LANG_AB_DESCRIPTION);
    }
    return "";
}

/* Word-wrapped text in a list. */
#define TEXT_LINES 64
static char text_lines[TEXT_LINES][128];

static const char *text_line(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return text_lines[item];
}

static void show_text(const char *title, const char *text)
{
    struct simplelist_info info;
    struct viewport vp;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    int maxw = vp.width - 8;
    int n = 0;
    const char *p = text;

    while (*p && n < TEXT_LINES)
    {
        char *line = text_lines[n];
        size_t fit = 0, len = 0;
        /* take words while they fit */
        while (p[len])
        {
            size_t next = len;
            while (p[next] == ' ')
                next++;
            while (p[next] && p[next] != ' ')
                next++;
            if (next >= sizeof text_lines[0])
                break;
            memcpy(line, p, next);
            line[next] = 0;
            int w;
            font_getstringsize(line, &w, NULL, vp.font);
            if (w > maxw && fit)
                break;
            fit = len = next;
            if (w > maxw)
                break;
        }
        if (!fit)
            fit = MIN(strlen(p), sizeof text_lines[0] - 1);
        memcpy(line, p, fit);
        line[fit] = 0;
        n++;
        p += fit;
        while (*p == ' ')
            p++;
    }

    simplelist_info_init(&info, (char *)title, n, NULL);
    info.get_name = text_line;
    simplelist_show_list(&info);
}

static bool book_is_playing(uint32_t id)
{
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return false;
    struct mp3entry *id3 = audio_current_track();
    if (!id3)
        return false;
    ab_lock();
    const struct ab_book *b = ab_db_book(ab_db_find_file(id3->path, NULL));
    bool same = b && b->id == id;
    ab_unlock();
    return same;
}

static int page_select(int item, void *data)
{
    struct page *pg = data;
    const struct ab_book *b = ab_db_book(pg->book);
    struct ab_progress *p;
    switch (pg->items[item])
    {
        case PAGE_RESUME:
            if (book_is_playing(b->id))
                return GO_TO_WPS;
            return audiobooks_play(pg->book, false) ? GO_TO_WPS : 0;
        case PAGE_START_OVER:
            return audiobooks_play(pg->book, true) ? GO_TO_WPS : 0;
        case PAGE_CHAPTERS:
            return chapters_screen(pg->book);
        case PAGE_BOOKMARKS:
            return bookmark_load(pg->bookmark_file, false) ? GO_TO_WPS : 0;
#ifdef HAVE_PITCHCONTROL
        case PAGE_SPEED:
            /* Speed is kept per book by the playback watcher, so set it
             * with the book playing. */
            if (!book_is_playing(b->id) && !audiobooks_play(pg->book, false))
                return 0;
            gui_syncpitchscreen_run();
            return 0;
#endif
        case PAGE_FINISHED:
            p = ab_progress_touch(b->id);
            if (p->finished)
            {
                p->finished = 0;
                uint32_t pos = 0;
                const uint32_t *len = ab_db_lengths(b);
                for (int i = 0; i < p->file && i < b->nfiles; i++)
                    pos += len[i];
                p->position = pos + p->elapsed;
            }
            else
            {
                p->finished = 1;
                p->position = b->length;
                p->last_played = ab_now();
            }
            ab_progress_dirty();
            ab_progress_save();
            return 0;
        case PAGE_SLEEP:
            sleep_menu();
            return 0;
        case PAGE_DESCRIPTION:
            show_text(ab_db_str(b, AB_S_TITLE), ab_db_str(b, AB_S_DESC));
            return 0;
    }
    return 0;
}

static int book_page(int book)
{
    static struct page pg;
    char title[MAX_PATH];
    const struct ab_book *b = ab_db_book(book);
    if (!b)
        return 0;
    strmemccpy(title, or_unknown(ab_db_str(b, AB_S_TITLE)), sizeof title);
    pg.book = book;
    struct card_screen cs = {
        .title = title,
        .book = book,
        .root = false,
        .count = page_count,
        .name = page_name,
        .select = page_select,
        .data = &pg,
    };
    return card_screen_run(&cs);
}

/* ====================================================================== */
/* settings                                                               */
/* ====================================================================== */

#define MAX_WPS_NAMES 64
static char wps_names[MAX_WPS_NAMES][MAX_FILENAME + 1];
static int nwps;

static int cmp_names(const void *a, const void *b)
{
    return strnatcasecmp(a, b);
}

static const char *wps_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return item == 0 ? (const char *)str(LANG_AB_SAME_AS_MUSIC) : wps_names[item - 1];
}

int audiobooks_choose_wps(void)
{
    nwps = 0;
    DIR *d = opendir(WPS_DIR);
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)) && nwps < MAX_WPS_NAMES)
        {
            size_t len = strlen(e->d_name);
            if (len > 4 && len - 4 <= MAX_FILENAME &&
                !strcasecmp(e->d_name + len - 4, ".wps"))
            {
                memcpy(wps_names[nwps], e->d_name, len - 4);
                wps_names[nwps][len - 4] = '\0';
                nwps++;
            }
        }
        closedir(d);
    }
    qsort(wps_names, nwps, sizeof wps_names[0], cmp_names);

    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_AB_WPS), nwps + 1, NULL);
    info.get_name = wps_name;
    info.title_icon = Icon_Wps;
    info.selection = 0;
    for (int i = 0; i < nwps; i++)
        if (!strcmp(wps_names[i], global_settings.audiobook_wps))
            info.selection = i + 1;
    simplelist_show_list(&info);
    if (info.selection < 0)
        return 0;

    if (info.selection == 0)
        global_settings.audiobook_wps[0] = '\0';
    else
        strmemccpy(global_settings.audiobook_wps, wps_names[info.selection - 1],
                   sizeof global_settings.audiobook_wps);
    settings_save();
    skin_set_wps_book_mode(skin_wps_book_mode(), true);
    return 0;
}

static int show_folder(void)
{
    const char *f = global_settings.audiobook_folder;
    splashf(HZ * 3, "%s: %s\n%s", str(LANG_AB_FOLDER), f[0] ? f : "-",
            str(LANG_AB_FOLDER_HINT));
    return 0;
}

MENUITEM_FUNCTION(ab_folder_item, 0, ID2P(LANG_AB_FOLDER),
                  show_folder, NULL, Icon_Folder);
MENUITEM_FUNCTION(ab_wps_item, 0, ID2P(LANG_AB_WPS),
                  audiobooks_choose_wps, NULL, Icon_Wps);
MENUITEM_SETTING(ab_rewind_after_item, &global_settings.ab_rewind_after, NULL);
MENUITEM_SETTING(ab_rewind_amount_item, &global_settings.ab_rewind_amount, NULL);
MENUITEM_SETTING(ab_finished_item, &global_settings.ab_finished_pct, NULL);
MENUITEM_SETTING(ab_autoscan_item, &global_settings.ab_autoscan, NULL);
MAKE_MENU(ab_settings_menu, ID2P(LANG_AB_SETTINGS), NULL, Icon_Config,
          &ab_folder_item, &ab_wps_item, &ab_rewind_after_item,
          &ab_rewind_amount_item, &ab_finished_item, &ab_autoscan_item);

/* ====================================================================== */
/* root                                                                   */
/* ====================================================================== */

enum root_item
{
    ROOT_CONTINUE,
    ROOT_LIBRARY,
    ROOT_AUTHORS,
    ROOT_SERIES,
    ROOT_NARRATORS,
    ROOT_RECENT,
    ROOT_IN_PROGRESS,
    ROOT_FINISHED,
    ROOT_SLEEP,
    ROOT_RESCAN,
    ROOT_SETTINGS,
};

struct root
{
    int items[16];
    int count;
    int last_book;
};

static int root_count(void *data)
{
    struct root *r = data;
    r->last_book = ab_progress_last_book();
    r->count = 0;
    if (r->last_book >= 0)
        r->items[r->count++] = ROOT_CONTINUE;
    for (int i = ROOT_LIBRARY; i <= ROOT_SETTINGS; i++)
        r->items[r->count++] = i;
    return r->count;
}

static const char *root_name(int item, void *data, char *buf, size_t size)
{
    struct root *r = data;
    static const int ids[] = {
        [ROOT_LIBRARY] = LANG_AB_LIBRARY,
        [ROOT_AUTHORS] = LANG_AB_AUTHORS,
        [ROOT_SERIES] = LANG_AB_SERIES,
        [ROOT_NARRATORS] = LANG_AB_NARRATORS,
        [ROOT_RECENT] = LANG_AB_RECENT,
        [ROOT_IN_PROGRESS] = LANG_AB_IN_PROGRESS,
        [ROOT_FINISHED] = LANG_AB_FINISHED,
        [ROOT_RESCAN] = LANG_AB_RESCAN,
        [ROOT_SETTINGS] = LANG_AB_SETTINGS,
    };
    switch (r->items[item])
    {
        case ROOT_CONTINUE:
        {
            char title[MAX_PATH];
            const struct ab_book *b = ab_db_book(r->last_book);
            strmemccpy(title, b ? or_unknown(ab_db_str(b, AB_S_TITLE)) : "",
                       sizeof title);
            snprintf(buf, size, "%s: %s", str(LANG_AB_RESUME), title);
            return buf;
        }
        case ROOT_SLEEP:
            sleep_status(buf, size);
            return buf;
        default:
            return str(ids[r->items[item]]);
    }
}

static int root_select(int item, void *data)
{
    struct root *r = data;
    switch (r->items[item])
    {
        case ROOT_CONTINUE:
        {
            const struct ab_book *b = ab_db_book(r->last_book);
            if (b && book_is_playing(b->id))
                return GO_TO_WPS;
            return audiobooks_play(r->last_book, false) ? GO_TO_WPS : 0;
        }
        case ROOT_LIBRARY:
            return book_list_screen(str(LANG_AB_LIBRARY), LIST_LIBRARY,
                                    AB_S_PATH, NULL);
        case ROOT_AUTHORS:
            return group_screen(LANG_AB_AUTHORS, AB_S_AUTHOR);
        case ROOT_SERIES:
            return group_screen(LANG_AB_SERIES, AB_S_SERIES);
        case ROOT_NARRATORS:
            return group_screen(LANG_AB_NARRATORS, AB_S_NARRATOR);
        case ROOT_RECENT:
            return book_list_screen(str(LANG_AB_RECENT), LIST_RECENT,
                                    AB_S_PATH, NULL);
        case ROOT_IN_PROGRESS:
            return book_list_screen(str(LANG_AB_IN_PROGRESS), LIST_IN_PROGRESS,
                                    AB_S_PATH, NULL);
        case ROOT_FINISHED:
            return book_list_screen(str(LANG_AB_FINISHED), LIST_FINISHED,
                                    AB_S_PATH, NULL);
        case ROOT_SLEEP:
            sleep_menu();
            return 0;
        case ROOT_RESCAN:
            ab_lock();
            ab_db_scan(true, false);
            ab_unlock();
            if (!ab_db_count())
                splashf(HZ, "%s", str(LANG_AB_EMPTY));
            return 0;
        case ROOT_SETTINGS:
        {
            int ret = do_menu(&ab_settings_menu, NULL, NULL, false);
            return ret == MENU_ATTACHED_USB ? GO_TO_ROOT : 0;
        }
    }
    return 0;
}

static int root_book(void *data)
{
    struct root *r = data;
    root_count(r);
    return r->last_book;
}

void audiobooks_ui_init(void)
{
    covers_open();
}

int audiobooks_screen(void)
{
    static struct root r;

    ab_progress_load();
    if (!ab_db_load() || !ab_db_count())
    {
        ab_lock();
        ab_db_scan(false, false);
        ab_unlock();
        if (!ab_db_count())
            splashf(HZ, "%s", str(LANG_AB_EMPTY));
    }
    struct card_screen cs = {
        .title = str(LANG_AUDIOBOOKS),
        .book = -1,
        .current_book = root_book,
        .root = true,
        .count = root_count,
        .name = root_name,
        .select = root_select,
        .data = &r,
    };
    int ret = card_screen_run(&cs);
    if (ret == 0)
        ret = GO_TO_ROOT;

    ab_progress_save();
    return ret;
}
