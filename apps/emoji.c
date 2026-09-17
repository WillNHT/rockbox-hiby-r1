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
 * Colour emoji.
 *
 * Pack file (all numbers little endian), written by tools/emojipack.py:
 *
 *   0   "RBEMOJI1"
 *   8   u16 size       pictures are size x size pixels
 *   10  u16 reserved
 *   12  u32 count
 *   16  u32 index_off  count entries of EMOJI_ENTRY bytes each
 *   20  u32 data_off   count pictures of size*size*3 bytes each
 *
 * An index entry is a length byte and up to EMOJI_SEQ codepoints of three
 * bytes each, sorted as codepoint sequences (a prefix sorts first). Variation
 * selector 16 is not part of any key: text may or may not carry it.
 * A picture is rows of pixels, each a big endian RGB565 value and one
 * byte of alpha (255 = opaque).
 *
 * Pictures are scaled to the line height when first drawn and kept in a
 * small cache. Both the UI thread and the scroll thread draw text, so the
 * cache has a lock.
 */
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "font.h"
#include "lcd.h"
#include "core_alloc.h"
#include "debug.h"
#include "emoji.h"

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)

#define EMOJI_MAGIC       "RBEMOJI1"
#define EMOJI_HEADER      24
#define EMOJI_SEQ         10
#define EMOJI_ENTRY       (1 + 3 * EMOJI_SEQ + 1)
#define EMOJI_MAX_SRC     128     /* biggest picture the pack may hold */
#define EMOJI_MAX_PX      64      /* biggest picture drawn */
#define EMOJI_SLOTS       48

static struct
{
    int  index_handle;          /* pinned, count * EMOJI_ENTRY bytes */
    int  count;
    int  size;
    long data_off;
    int  cache_handle;          /* pinned, EMOJI_SLOTS * slot_bytes */
    struct mutex lock;
    bool lock_ready;
} pack = { .index_handle = -1, .cache_handle = -1 };

static struct
{
    int  id;
    int  size;
    long used;
} slots[EMOJI_SLOTS];
static long use_clock;

#define SLOT_PIXELS     (EMOJI_MAX_PX * EMOJI_MAX_PX * sizeof(fb_data))
#define SLOT_ALPHA      ((EMOJI_MAX_PX + 1) / 2 * EMOJI_MAX_PX)
#define SLOT_BYTES      (SLOT_PIXELS + SLOT_ALPHA)

static unsigned char src_buf[EMOJI_MAX_SRC * EMOJI_MAX_SRC * 3];

static inline ucschar_t entry_cp(const unsigned char *e, int i)
{
    const unsigned char *p = e + 1 + 3 * i;
    return p[0] | (p[1] << 8) | ((ucschar_t)p[2] << 16);
}

/* <0, 0, >0 as the entry sorts before, equal to or after key[0..n) */
static int entry_cmp(const unsigned char *e, const ucschar_t *key, int n)
{
    int len = e[0];
    for (int i = 0; i < len && i < n; i++)
    {
        ucschar_t c = entry_cp(e, i);
        if (c != key[i])
            return c < key[i] ? -1 : 1;
    }
    return len - n;
}

/* first entry not sorting before key[0..n) */
static int lower_bound(const unsigned char *idx, const ucschar_t *key, int n)
{
    int lo = 0, hi = pack.count;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (entry_cmp(idx + mid * EMOJI_ENTRY, key, n) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static inline bool is_skin_tone(ucschar_t c)
{
    return c >= 0x1f3fb && c <= 0x1f3ff;
}

static int emoji_match(const ucschar_t *s, int *len)
{
    ucschar_t key[EMOJI_SEQ];
    unsigned char used[EMOJI_SEQ]; /* codepoints of s up to key[i] */
    int n = 0, consumed = 0;

    /* The key drops VS16, so remember how much of s each key covers. */
    while (n < EMOJI_SEQ && s[consumed])
    {
        ucschar_t c = s[consumed++];
        if (c == 0xfe0f)
        {
            if (n == 0)
                return -1;
            used[n - 1] = consumed;
            continue;
        }
        key[n] = c;
        used[n] = consumed;
        n++;
        if (consumed >= EMOJI_SEQ + 4)
            break;
    }
    if (n == 0)
        return -1;

    mutex_lock(&pack.lock);
    int ret = -1;
    if (pack.index_handle < 0)
        goto out;
    const unsigned char *idx = core_get_data(pack.index_handle);

    /* Quick no: nothing starts with this codepoint. Most text ends here. */
    int first = lower_bound(idx, key, 1);
    if (first >= pack.count ||
        entry_cp(idx + first * EMOJI_ENTRY, 0) != key[0])
        goto out;

    for (int k = n; k > 0; k--)
    {
        /* A sequence only ends on a joined codepoint, never on the joiner. */
        if (key[k - 1] == 0x200d)
            continue;
        int i = lower_bound(idx, key, k);
        if (i < pack.count && entry_cmp(idx + i * EMOJI_ENTRY, key, k) == 0)
        {
            int c = used[k - 1];
            /* Tones and selectors the pack has no picture for still
             * belong to this emoji; draw it plain rather than follow it
             * with a colour swatch. */
            while (s[c] == 0xfe0f || is_skin_tone(s[c]))
                c++;
            *len = c;
            ret = i;
            break;
        }
    }
out:
    mutex_unlock(&pack.lock);
    return ret;
}

static bool load_source(int id)
{
    int fd = open(EMOJI_PACK, O_RDONLY);
    if (fd < 0)
        return false;
    size_t bytes = (size_t)pack.size * pack.size * 3;
    bool ok = lseek(fd, pack.data_off + (off_t)id * bytes, SEEK_SET) >= 0 &&
              read(fd, src_buf, bytes) == (ssize_t)bytes;
    close(fd);
    return ok;
}

/* Box filter (or nearest neighbour when growing) with alpha weighting, so
 * transparent pixels do not darken the edges. */
static void scale_into(fb_data *px, unsigned char *alpha, int t)
{
    int s = pack.size;
    memset(alpha, 0, (t + 1) / 2 * t);

    for (int ty = 0; ty < t; ty++)
    {
        int y0 = ty * s / t, y1 = (ty + 1) * s / t;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int tx = 0; tx < t; tx++)
        {
            int x0 = tx * s / t, x1 = (tx + 1) * s / t;
            if (x1 <= x0)
                x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int y = y0; y < y1; y++)
            {
                const unsigned char *p = src_buf + (y * s + x0) * 3;
                for (int x = x0; x < x1; x++, p += 3)
                {
                    unsigned v = (p[0] << 8) | p[1];
                    unsigned pa = p[2];
                    r += ((v >> 11) & 0x1f) * pa;
                    g += ((v >> 5) & 0x3f) * pa;
                    b += (v & 0x1f) * pa;
                    a += pa;
                    cnt++;
                }
            }
            unsigned oa = a / cnt;
            unsigned r8 = 0, g8 = 0, b8 = 0;
            if (a)
            {
                r8 = (r / a) * 255 / 31;
                g8 = (g / a) * 255 / 63;
                b8 = (b / a) * 255 / 31;
            }
            px[ty * t + tx] = FB_RGBPACK(r8, g8, b8);
            /* the LCD's alpha nibbles count transparency */
            unsigned nib = 15 - (oa >> 4);
            int pos = ty * ((t + 1) & ~1) + tx;
            alpha[pos >> 1] |= nib << ((pos & 1) * 4);
        }
    }
}

static void emoji_draw(int id, int x, int y, int size, int ofs)
{
    if (size > EMOJI_MAX_PX)
        size = EMOJI_MAX_PX;
    if (size < 4 || pack.cache_handle < 0)
        return;

    mutex_lock(&pack.lock);

    int slot = -1, victim = 0;
    for (int i = 0; i < EMOJI_SLOTS; i++)
    {
        if (slots[i].id == id && slots[i].size == size)
        {
            slot = i;
            break;
        }
        if (slots[i].used < slots[victim].used)
            victim = i;
    }

    unsigned char *mem = core_get_data(pack.cache_handle);
    if (slot < 0)
    {
        slot = victim;
        slots[slot].id = -1;
        if (!load_source(id))
        {
            mutex_unlock(&pack.lock);
            return;
        }
        mem = core_get_data(pack.cache_handle);
        scale_into((fb_data *)(mem + slot * SLOT_BYTES),
                   mem + slot * SLOT_BYTES + (size_t)size * size * sizeof(fb_data),
                   size);
        slots[slot].id = id;
        slots[slot].size = size;
    }
    slots[slot].used = ++use_clock;

    unsigned char *base = mem + slot * SLOT_BYTES;
    /* lcd_bmp_part() wants the alpha right after the pixels */
    size_t pix = (size_t)size * size * sizeof(fb_data);

    struct bitmap bm = {
        .width = size,
        .height = size,
        .format = FORMAT_NATIVE,
        .maskdata = NULL,
        .alpha_offset = pix,
        .data = base,
    };

    struct viewport *vp = lcd_current_viewport;
    int mode = vp->drawmode;
    if (mode & DRMODE_BG)
    {
        /* Text in solid mode paints its own background; so does this. */
        vp->drawmode = (mode & DRMODE_INVERSEVID) ?
                        DRMODE_SOLID : (DRMODE_SOLID | DRMODE_INVERSEVID);
        lcd_fillrect(x, y, size - ofs, size);
    }
    vp->drawmode = DRMODE_FG;
    lcd_bmp_part(&bm, ofs, 0, x, y, size - ofs, size);
    vp->drawmode = mode;

    mutex_unlock(&pack.lock);
}

static const struct emoji_ops ops = {
    .match = emoji_match,
    .draw  = emoji_draw,
};

static uint32_t get_le(const unsigned char *p, int n)
{
    uint32_t v = 0;
    while (n--)
        v = (v << 8) | p[n];
    return v;
}

void emoji_close(void)
{
    font_set_emoji_ops(NULL);
    if (pack.lock_ready)
        mutex_lock(&pack.lock);
    if (pack.index_handle >= 0)
    {
        core_unpin(pack.index_handle);
        pack.index_handle = core_free(pack.index_handle);
    }
    if (pack.lock_ready)
        mutex_unlock(&pack.lock);
}

bool emoji_init(void)
{
    unsigned char hdr[EMOJI_HEADER];

    if (!pack.lock_ready)
    {
        mutex_init(&pack.lock);
        pack.lock_ready = true;
    }
    emoji_close();

    int fd = open(EMOJI_PACK, O_RDONLY);
    if (fd < 0)
        return false;

    bool ok = false;
    if (read(fd, hdr, sizeof hdr) == sizeof hdr &&
        !memcmp(hdr, EMOJI_MAGIC, 8))
    {
        int size = get_le(hdr + 8, 2);
        int count = get_le(hdr + 12, 4);
        long index_off = get_le(hdr + 16, 4);
        long data_off = get_le(hdr + 20, 4);

        if (size > 0 && size <= EMOJI_MAX_SRC &&
            count > 0 && count < 65536 &&
            lseek(fd, index_off, SEEK_SET) == index_off)
        {
            size_t bytes = (size_t)count * EMOJI_ENTRY;
            int h = core_alloc(bytes);
            if (h > 0)
            {
                core_pin(h);
                if (read(fd, core_get_data(h), bytes) == (ssize_t)bytes)
                {
                    mutex_lock(&pack.lock);
                    pack.index_handle = h;
                    pack.count = count;
                    pack.size = size;
                    pack.data_off = data_off;
                    for (int i = 0; i < EMOJI_SLOTS; i++)
                        slots[i].id = -1;
                    mutex_unlock(&pack.lock);
                    ok = true;
                }
                else
                {
                    core_unpin(h);
                    core_free(h);
                }
            }
        }
    }
    close(fd);

    if (ok && pack.cache_handle < 0)
    {
        int h = core_alloc(EMOJI_SLOTS * SLOT_BYTES);
        if (h > 0)
        {
            core_pin(h);
            pack.cache_handle = h;
        }
    }

    if (ok && pack.cache_handle >= 0)
    {
        font_set_emoji_ops(&ops);
        return true;
    }
    emoji_close();
    return false;
}

#else /* no colour LCD */

bool emoji_init(void) { return false; }
void emoji_close(void) { }

#endif
