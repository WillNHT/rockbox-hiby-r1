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

/* %yb: a block of lyrics that scrolls with the song, the line being sung
 * in the viewport's colour and the rest in a dimmer one. Long lines wrap
 * at spaces, or anywhere in CJK text. */

#include "config.h"
#include <string.h>
#include "system.h"
#include "font.h"
#include "rbunicode.h"
#include "screen_access.h"
#include "lyrics.h"
#include "wps_internals.h"
#include "skin_layer.h"
#include "skin_lyrics.h"

#if defined(HAVE_LYRICS) && !defined(__PCTOOL__)

#define MAX_ROWS        8       /* rows one lyric line may wrap into */
#define SCROLL_MS       350     /* how long a move to the next line takes */
#define ROW_BYTES       256

struct row
{
    unsigned short start, len;
};

static bool breaks_anywhere(ucschar_t ch)
{
    /* CJK, kana, Hangul, fullwidth forms: no spaces between words. */
    return (ch >= 0x2e80 && ch <= 0x9fff) || (ch >= 0xac00 && ch <= 0xd7af) ||
           (ch >= 0xf900 && ch <= 0xfaff) || (ch >= 0xff00 && ch <= 0xffef);
}

/* Splits s into rows no wider than w; returns the row count (>= 1). */
static int wrap(const char *s, struct font *pf, int w, struct row *rows)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    int start = 0, width = 0;
    int brk = -1, brk_resume = -1, brk_width = 0;

    while (*p)
    {
        ucschar_t ch;
        const unsigned char *next = utf8decode(p, &ch);
        int at = p - (const unsigned char *)s;
        int cw = font_is_zero_width(ch) ? 0 :
                 font_get_width(font_glyph_font(pf, ch), ch);

        if (ch == ' ')
        {
            brk = at;
            brk_resume = next - (const unsigned char *)s;
            brk_width = width + cw;
        }
        else if (breaks_anywhere(ch) && at > start)
        {
            brk = at;
            brk_resume = at;
            brk_width = width;
        }

        if (width + cw > w && at > start && n < MAX_ROWS - 1)
        {
            if (brk > start)
            {
                rows[n].start = start;
                rows[n].len = brk - start;
                n++;
                start = brk_resume;
                width -= brk_width;
            }
            else
            {
                rows[n].start = start;
                rows[n].len = at - start;
                n++;
                start = at;
                width = 0;
            }
            brk = -1;
        }
        width += cw;
        p = next;
    }
    rows[n].start = start;
    rows[n].len = (const char *)p - s - start;
    return n + 1;
}

static int line_rows(int i, struct font *pf, int w, struct row *rows)
{
    const char *s = lyrics_line(i);
    return s ? wrap(s, pf, w, rows) : 1;
}

static unsigned mix(unsigned a, unsigned b, int pct)
{
#if LCD_DEPTH > 1 && defined(HAVE_LCD_COLOR)
    int r = (RGB_UNPACK_RED(a) * pct + RGB_UNPACK_RED(b) * (100 - pct)) / 100;
    int g = (RGB_UNPACK_GREEN(a) * pct +
             RGB_UNPACK_GREEN(b) * (100 - pct)) / 100;
    int bl = (RGB_UNPACK_BLUE(a) * pct +
              RGB_UNPACK_BLUE(b) * (100 - pct)) / 100;
    return LCD_RGBPACK(r, g, bl);
#else
    (void)b; (void)pct;
    return a;
#endif
}

static void draw_line(struct screen *display, struct viewport *vp,
                      struct font *pf, int i, int top, int w)
{
    struct row rows[MAX_ROWS];
    char buf[ROW_BYTES];
    const char *s = lyrics_line(i);
    int n;

    if (!s)
        return;
    n = wrap(s, pf, w, rows);
    for (int r = 0; r < n; r++)
    {
        int y = top + r * (int)pf->height;
        int len = MIN(rows[r].len, ROW_BYTES - 1);
        if (y + (int)pf->height <= 0 || y >= vp->height)
            continue;
        memcpy(buf, s + rows[r].start, len);
        buf[len] = '\0';
        display->putsxy(0, y, buf);
    }
}

void skin_lyrics_draw(struct gui_wps *gwps, struct skin_viewport *svp,
                      const struct skin_lyrics *ly, bool full)
{
    static unsigned long last_gen;
    static int last_line = -3, last_scroll = -1;
    static const struct skin_lyrics *last_ly;
    struct screen *display = gwps->display;
    struct viewport *vp = &svp->vp;
    struct viewport box;
    struct lyrics_pos pos;
    struct row rows[MAX_ROWS];
    struct font *pf;
    int lh, w, h, cur, scroll, ease_pm;
    int hcur, hprev;
    unsigned active, inactive;

    bool have = lyrics_position(&pos);
    cur = have ? MAX(pos.current, 0) : -1;

    box = *vp;
    box.x = vp->x + ly->x;
    box.y = vp->y + ly->y;
    box.width = ly->w > 0 ? ly->w : vp->width - ly->x;
    box.height = ly->h > 0 ? ly->h : vp->height - ly->y;
    box.flags &= ~VP_FLAG_ALIGNMENT_MASK;
    if (ly->align == SKIN_LYRICS_CENTER)
        box.flags |= VP_FLAG_ALIGN_CENTER;
    else if (ly->align == SKIN_LYRICS_RIGHT)
        box.flags |= VP_FLAG_ALIGN_RIGHT;
    w = box.width;
    h = box.height;

    pf = font_get(vp->font);
    lh = pf->height;

    /* How far the move to the current line has got, 0..1000. Plain
     * lyrics creep through the whole line instead. */
    ease_pm = 1000;
    if (have && pos.current >= 0)
    {
        if (pos.kind == LYRICS_SYNCED)
        {
            if (pos.line_ms < SCROLL_MS && pos.current > 0)
            {
                long t = pos.line_ms * 1000 / SCROLL_MS;
                /* ease out: 1 - (1 - t)^2 */
                ease_pm = 1000 - (1000 - t) * (1000 - t) / 1000;
            }
        }
        else if (pos.next_ms > 0)
            ease_pm = MIN(1000, pos.line_ms * 1000 / pos.next_ms);
    }

    /* The block's scroll position is the middle of the current line,
     * sliding there from the middle of the one before. */
    hcur = have ? line_rows(cur, pf, w, rows) * lh + ly->gap : 0;
    if (have && pos.kind == LYRICS_PLAIN)
    {
        int hnext = cur + 1 < pos.count ?
                    line_rows(cur + 1, pf, w, rows) * lh + ly->gap : hcur;
        scroll = (hcur + hnext) / 2 * ease_pm / 1000;
        hprev = 0;
    }
    else
    {
        hprev = have && cur > 0 ?
                line_rows(cur - 1, pf, w, rows) * lh + ly->gap : 0;
        scroll = -(hprev + hcur) / 2 * (1000 - ease_pm) / 1000;
    }

    /* The line being sung, -1 before the first, -2 with no lyrics. */
    int line = have ? pos.current : -2;
    if (!full && last_ly == ly && pos.generation == last_gen &&
        line == last_line && scroll == last_scroll)
        return;
    last_gen = pos.generation;
    last_line = line;
    last_scroll = scroll;
    last_ly = ly;

    display->set_viewport_ex(vp, VP_FLAG_VP_SET_CLEAN);
    if (!full)
        skin_layer_clear_viewport(display, svp);
    if (!have)
        return;

    active = vp->fg_pattern;
    inactive = ly->has_inactive ? (unsigned)ly->inactive
                                : mix(vp->fg_pattern, vp->bg_pattern, 45);

    display->set_viewport_ex(&box, VP_FLAG_VP_SET_CLEAN);
    display->set_drawmode(DRMODE_FG);

    /* The current line, centred, then outwards until the box is full. */
    {
        /* The gap is spacing between lines, not part of the current one:
         * its text is what sits in the middle. */
        int top = h / 2 - (hcur - ly->gap) / 2 - scroll;
        int y;
        bool lit = pos.current >= 0;

        display->set_foreground(lit ? active : inactive);
        draw_line(display, &box, pf, cur, top, w);

        display->set_foreground(inactive);
        y = top;
        for (int i = cur - 1; i >= 0 && y > 0; i--)
        {
            int hi = line_rows(i, pf, w, rows) * lh + ly->gap;
            y -= hi;
            draw_line(display, &box, pf, i, y, w);
        }
        y = top + hcur;
        for (int i = cur + 1; i < pos.count && y < h; i++)
        {
            draw_line(display, &box, pf, i, y, w);
            y += line_rows(i, pf, w, rows) * lh + ly->gap;
        }
    }

    display->set_foreground(vp->fg_pattern);
    display->set_drawmode(DRMODE_SOLID);
    display->set_viewport_ex(vp, VP_FLAG_VP_SET_CLEAN);
}

#endif /* HAVE_LYRICS && !__PCTOOL__ */
