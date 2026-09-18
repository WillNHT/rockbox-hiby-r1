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

/* Covers for the library views.
 *
 * A path is resolved to a picture like this:
 *   - an image file (.jpg, .jpeg, .png) is the picture;
 *   - a playlist (.m3u, .m3u8) is its cover, see playlist_cover.c;
 *   - anything else is taken as an audio file: its embedded art, or else
 *     the folder art the WPS would use for it.
 *
 * Decoding happens on the UI thread the first time an item is drawn, and
 * the result is kept, keyed by path and size, in an LRU of COVER_SLOTS
 * pictures. A path with no picture is remembered too, so a list full of
 * coverless items does not reopen every file on every redraw. */

#include "config.h"

#ifdef HAVE_COVER_VIEWS

#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "core_alloc.h"
#include "crc32.h"
#include "lcd.h"
#include "font.h"
#include "screen_access.h"
#include "settings.h"
#include "metadata.h"
#include "bmp.h"
#include "jpeg_load.h"
#ifdef HAVE_PNG
#include "png_load.h"
#endif
#include "albumart.h"
#include "playlist_cover.h"
#include "covers.h"
#ifdef HAVE_VIDEO
#include "video/video_lib.h"
#include "video/video_surface.h"
#include "video/video_art.h"
#endif

#define COVER_SLOTS  12
#define SLOT_BYTES   (COVER_MAX_SIZE * COVER_MAX_SIZE * sizeof(fb_data))

struct cover_slot
{
    uint32_t key;       /* crc of the path; 0 = empty */
    int      size;      /* the square asked for */
    int      w, h;      /* what the picture came out as */
    long     used;
    bool     missing;
};

static struct cover_slot slots[COVER_SLOTS];
static long cover_clock;
static int pix_handle = -1;
static int decode_handle = -1;
static size_t decode_size;

bool covers_init(void)
{
    if (pix_handle > 0)
        return true;

    memset(slots, 0, sizeof(slots));
    pix_handle = core_alloc(COVER_SLOTS * SLOT_BYTES);
    decode_size = SLOT_BYTES + JPEG_DECODE_OVERHEAD + 256 * 1024;
    decode_handle = core_alloc(decode_size);
    if (pix_handle <= 0 || decode_handle <= 0)
    {
        if (pix_handle > 0)
            core_free(pix_handle);
        if (decode_handle > 0)
            core_free(decode_handle);
        pix_handle = decode_handle = -1;
        return false;
    }
    core_pin(pix_handle);
    core_pin(decode_handle);
    return true;
}

static bool has_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e && !strcasecmp(path + n - e, ext);
}

static void bitmap_prepare(struct bitmap *bm, int size)
{
    memset(bm, 0, sizeof *bm);
    bm->width = size;
    bm->height = size;
    bm->data = core_get_data(decode_handle);
}

#define DECODE_FORMAT (FORMAT_NATIVE | FORMAT_DITHER | \
                       FORMAT_RESIZE | FORMAT_KEEP_ASPECT)

static bool decode_image(const char *path, int size, struct bitmap *bm)
{
    int rc = -1;

    /* .gif and .webp come from the video decoder (apps/video), as their
     * first frame; without it they count as no cover and the playlist
     * falls back to its first track. An animated PNG is drawn as its
     * first frame by the PNG decoder. */
    if (has_ext(path, ".gif") || has_ext(path, ".webp"))
    {
#if defined(HAVE_VIDEO) && LCD_DEPTH == 16
        bitmap_prepare(bm, size);
        return video_decode_still(path, size, size, RBV_FIT_CONTAIN,
                                  (fb_data *)bm->data);
#else
        return false;
#endif
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    bitmap_prepare(bm, size);
#ifdef HAVE_PNG
    if (has_ext(path, ".png"))
        rc = read_png_fd(fd, bm, decode_size, DECODE_FORMAT);
    else
#endif
#ifdef HAVE_JPEG
        rc = read_jpeg_fd(fd, 0, bm, decode_size, DECODE_FORMAT, NULL, NULL);
#endif
    close(fd);
    return rc > 0;
}

static bool decode_audio(const char *path, int size, struct bitmap *bm)
{
    static struct mp3entry id3;
    static char art[MAX_PATH];
    int rc = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    memset(&id3, 0, sizeof(id3));
    if (!get_metadata(&id3, fd, path))
    {
        close(fd);
        return false;
    }

    if (id3.has_embedded_albumart && id3.albumart.size > 0)
    {
        bitmap_prepare(bm, size);
#ifdef HAVE_PNG
        if (id3.albumart.type == AA_TYPE_PNG)
            rc = clip_png_fd(fd, id3.albumart.pos, id3.albumart.size, bm,
                             decode_size, DECODE_FORMAT);
        else
#endif
#ifdef HAVE_JPEG
        if (id3.albumart.type != AA_TYPE_BMP)
        {
            lseek(fd, id3.albumart.pos, SEEK_SET);
            rc = clip_jpeg_fd(fd, id3.albumart.type, id3.albumart.size, bm,
                              decode_size, DECODE_FORMAT, NULL, NULL);
        }
#endif
    }
    close(fd);
    if (rc > 0)
        return true;

    if (search_albumart_files(&id3, "", art, sizeof(art)))
        return decode_image(art, size, bm);
    return false;
}

static bool decode_path(const char *path, int size, struct bitmap *bm)
{
    static char resolved[MAX_PATH];

    if (has_ext(path, ".m3u") || has_ext(path, ".m3u8"))
    {
        if (!playlist_cover_find(path, resolved, sizeof(resolved)))
            return false;
        path = resolved;
    }
    if (has_ext(path, ".jpg") || has_ext(path, ".jpeg") ||
        has_ext(path, ".png") || has_ext(path, ".gif") ||
        has_ext(path, ".webp"))
        return decode_image(path, size, bm);
    return decode_audio(path, size, bm);
}

static const struct bitmap *cover_get(const char *path, int size)
{
    static struct bitmap out;
    struct bitmap bm;
    uint32_t key;
    int slot = -1, victim = 0, i;

    if (pix_handle <= 0 || !path || !path[0])
        return NULL;
    if (size > COVER_MAX_SIZE)
        size = COVER_MAX_SIZE;
    if (size < 8)
        return NULL;

    key = crc_32(path, strlen(path), 0xffffffff) | 1;
    for (i = 0; i < COVER_SLOTS; i++)
    {
        if (slots[i].key == key && slots[i].size == size)
        {
            slot = i;
            break;
        }
        if (slots[i].used < slots[victim].used)
            victim = i;
    }

    if (slot < 0)
    {
        slot = victim;
        slots[slot].key = key;
        slots[slot].size = size;
        slots[slot].missing = !decode_path(path, size, &bm) ||
                              bm.width <= 0 || bm.height <= 0 ||
                              bm.width > size || bm.height > size;
        if (!slots[slot].missing)
        {
            slots[slot].w = bm.width;
            slots[slot].h = bm.height;
            memcpy((char *)core_get_data(pix_handle) + slot * SLOT_BYTES,
                   bm.data, (size_t)bm.width * bm.height * sizeof(fb_data));
        }
    }
    slots[slot].used = ++cover_clock;
    if (slots[slot].missing)
        return NULL;

    memset(&out, 0, sizeof out);
    out.width = slots[slot].w;
    out.height = slots[slot].h;
    out.format = FORMAT_NATIVE;
    out.data = (unsigned char *)core_get_data(pix_handle) + slot * SLOT_BYTES;
    return &out;
}

#ifdef HAVE_VIDEO
int cover_draw_animated(struct screen *d, const char *path,
                        int x, int y, int size, bool full)
{
    static char resolved[MAX_PATH];
    const char *img = path;

    if (d->screen_type != SCREEN_MAIN || d->depth < 16)
        return 0;
    if (has_ext(path, ".m3u") || has_ext(path, ".m3u8"))
    {
        if (!playlist_cover_find(path, resolved, sizeof(resolved)))
            img = NULL;
        else
            img = resolved;
    }
    if (img && video_is_image_anim(img))
    {
        if (video_clip_draw(d, img, x, y, size, size, RBV_FIT_CONTAIN, full))
            return 2;
    }
    if (!full)
        return 0;
    return cover_draw(d, path, x, y, size) ? 1 : 0;
}
#endif

bool cover_draw(struct screen *d, const char *path, int x, int y, int size)
{
    if (d->screen_type != SCREEN_MAIN || d->depth < 16)
        return false;
    const struct bitmap *bm = cover_get(path, size);
    if (!bm)
        return false;
    d->bmp(bm, x + (size - bm->width) / 2, y + (size - bm->height) / 2);
    return true;
}

void cover_draw_placeholder(struct screen *d, const char *text,
                            int x, int y, int size)
{
    char letter[8];
    int w, h, n = 1;
    unsigned fg = d->get_foreground();

    if (d->depth >= 16)
    {
        d->set_foreground(LCD_RGBPACK(0x3a, 0x3b, 0x3d));
        d->fillrect(x, y, size, size);
        d->set_foreground(fg);
    }
    else
        d->drawrect(x, y, size, size);

    if (!text)
        return;
    while (*text == ' ' || *text == '\t')
        text++;
    if (!*text)
        return;
    /* one character, whole UTF-8 sequence */
    while ((text[n] & 0xc0) == 0x80 && n < 4)
        n++;
    memcpy(letter, text, n);
    letter[n] = '\0';
    d->getstringsize(letter, &w, &h);
    d->set_drawmode(DRMODE_FG);
    d->putsxy(x + (size - w) / 2, y + (size - h) / 2, letter);
    d->set_drawmode(DRMODE_SOLID);
}

void covers_forget(const char *path)
{
    uint32_t key = path ? (crc_32(path, strlen(path), 0xffffffff) | 1) : 0;
    for (int i = 0; i < COVER_SLOTS; i++)
        if (!path || slots[i].key == key)
            slots[i].key = 0;
}

#endif /* HAVE_COVER_VIEWS */
