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

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "system.h"
#include "file.h"
#include "string-extra.h"
#include "strmemccpy.h"
#include "rbunicode.h"
#include "audio.h"
#include "metadata.h"
#include "settings.h"
#include "appevents.h"
#include "lyrics.h"

#ifdef HAVE_LYRICS

/* A long song's LRC is 3-6 KiB; this leaves room for UTF-16 files and
 * translations kept alongside the original lines. */
#define RAW_SIZE    (64 * 1024)
#define TEXT_SIZE   (96 * 1024)
#define MAX_LINES   1500

struct line
{
    long time;          /* ms, -1 for none */
    int text;           /* offset into text[] */
};

static unsigned char raw[RAW_SIZE + 4];
static char text[TEXT_SIZE];
static int text_used;
static struct line lines[MAX_LINES];
static int nlines;
static enum lyrics_kind kind;
static long offset_ms;          /* LRC [offset:] - positive is earlier */

static char loaded_path[MAX_PATH];
static bool loaded;
static unsigned long generation;

/* ------------------------------------------------------------ storage */

static void reset(void)
{
    text_used = 0;
    nlines = 0;
    kind = LYRICS_NONE;
    offset_ms = 0;
}

/* Copies len bytes as a new string; returns its offset or -1. */
static int store(const char *s, int len)
{
    if (text_used + len + 1 > TEXT_SIZE)
        return -1;
    memcpy(text + text_used, s, len);
    text[text_used + len] = '\0';
    text_used += len + 1;
    return text_used - len - 1;
}

static bool add_line(long time, int textoff)
{
    if (nlines >= MAX_LINES || textoff < 0)
        return false;
    lines[nlines].time = time;
    lines[nlines].text = textoff;
    nlines++;
    return true;
}

/* ------------------------------------------------------------ encoding */

static bool valid_utf8(const unsigned char *s, int len)
{
    int i = 0;
    while (i < len)
    {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 :
                (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0 || i + n >= len + (n ? 0 : 1))
            return false;
        for (int k = 1; k <= n; k++)
            if ((s[i + k] & 0xc0) != 0x80)
                return false;
        i += n + 1;
    }
    return true;
}

/* Recode len bytes of src (enc as in ID3v2: 0 latin/codepage, 1 UTF-16
 * with BOM, 2 UTF-16BE, 3 UTF-8; or -1 to guess) into out as UTF-8.
 * Returns the UTF-8 length. out must hold outsize bytes. */
static int to_utf8(const unsigned char *src, int len, int enc,
                   char *out, int outsize)
{
    unsigned char *end;
    bool le = false;

    if (enc < 0)
    {
        if (len >= 3 && !memcmp(src, "\xef\xbb\xbf", 3))
        {
            src += 3;
            len -= 3;
            enc = 3;
        }
        else if (len >= 2 && (!memcmp(src, "\xff\xfe", 2) ||
                              !memcmp(src, "\xfe\xff", 2)))
            enc = 1;
        else if (len >= 4 && ((src[0] == 0) != (src[1] == 0)) &&
                 ((src[2] == 0) != (src[3] == 0)))
            enc = 1;    /* UTF-16 with no BOM: every other byte is zero */
        else if (valid_utf8(src, len))
            enc = 3;
        else
            enc = 0;
    }

    switch (enc)
    {
        case 1:
        case 2:
            if (enc == 2)
                le = false;
            else if (len >= 2 && utf16_has_bom(src, &le))
            {
                src += 2;
                len -= 2;
            }
            end = utf16decode(src, (unsigned char *)out, len / 2,
                              outsize - 1, le);
            break;
        case 3:
            if (len > outsize - 1)
                len = outsize - 1;
            memcpy(out, src, len);
            end = (unsigned char *)out + len;
            break;
        default:
            end = iso_decode_ex(src, (unsigned char *)out,
                                global_settings.default_codepage, len,
                                outsize - 1);
            break;
    }
    *end = '\0';
    return end - (unsigned char *)out;
}

/* ------------------------------------------------------------ parsing */

/* [mm:ss], [mm:ss.x..], [mm:ss:xx], [h:mm:ss.xx]; returns ms or -1 */
static long parse_time(const char *s, int len)
{
    long parts[3] = {0, 0, 0};
    int np = 0;
    long frac = 0;
    int fracdigits = 0;
    bool infrac = false;
    int i;

    if (len <= 0 || !isdigit((unsigned char)s[0]))
        return -1;
    for (i = 0; i < len; i++)
    {
        char c = s[i];
        if (isdigit((unsigned char)c))
        {
            if (infrac)
            {
                if (fracdigits < 3)
                {
                    frac = frac * 10 + (c - '0');
                    fracdigits++;
                }
            }
            else
                parts[np] = parts[np] * 10 + (c - '0');
        }
        else if (c == ':' && !infrac)
        {
            if (++np > 2)
                return -1;
        }
        else if (c == '.' && !infrac)
            infrac = true;
        else
            return -1;
    }
    if (np == 0)
        return -1;
    /* [mm:ss:xx] is centiseconds, not hours: LRC has no hours. */
    if (np == 2 && !infrac)
    {
        frac = parts[2];
        fracdigits = 2;
        np = 1;
    }
    while (fracdigits < 3)
    {
        frac *= 10;
        fracdigits++;
    }
    if (np == 2)
        return ((parts[0] * 60 + parts[1]) * 60 + parts[2]) * 1000 + frac;
    if (parts[1] >= 60)
        return -1;
    return (parts[0] * 60 + parts[1]) * 1000 + frac;
}

/* Drops enhanced LRC word times (<mm:ss.xx>) and trailing spaces. */
static int clean_text(char *s, int len)
{
    int r = 0, w = 0;
    while (r < len)
    {
        if (s[r] == '<')
        {
            char *close = memchr(s + r, '>', len - r);
            if (close && parse_time(s + r + 1, close - (s + r + 1)) >= 0)
            {
                r = close - s + 1;
                continue;
            }
        }
        s[w++] = s[r++];
    }
    while (w > 0 && (s[w - 1] == ' ' || s[w - 1] == '\t'))
        w--;
    return w;
}

static void sort_lines(void)
{
    /* Insertion sort: stable, and LRC files are all but sorted already. */
    for (int i = 1; i < nlines; i++)
    {
        struct line l = lines[i];
        int j = i - 1;
        while (j >= 0 && lines[j].time > l.time)
        {
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = l;
    }
}

/* Parses UTF-8 LRC or plain text (NUL-terminated, modified in place). */
static void parse_text(char *s)
{
    long times[16];
    bool any_time = false;
    int first = nlines;

    while (*s)
    {
        char *eol = s + strcspn(s, "\r\n");
        char *p = s;
        int nt = 0;
        bool tagline = false;

        /* Leading [..] tags: times, or [ar:..] and friends. */
        while (*p == '[')
        {
            char *close = memchr(p, ']', eol - p);
            if (!close)
                break;
            long t = parse_time(p + 1, close - p - 1);
            if (t >= 0)
            {
                if (nt < (int)ARRAYLEN(times))
                    times[nt++] = t;
            }
            else if (!strncasecmp(p + 1, "offset:", 7))
            {
                offset_ms = strtol(p + 8, NULL, 10);
                tagline = true;
            }
            else if (close > p + 1 && isalpha((unsigned char)p[1]) &&
                     memchr(p, ':', close - p))
                tagline = true;     /* [ar:], [ti:], [length:] ... */
            else
                break;
            p = close + 1;
        }

        int len = clean_text(p, eol - p);
        if (nt > 0)
        {
            int off = store(p, len);
            any_time = true;
            for (int i = 0; i < nt; i++)
                add_line(times[i], off);
        }
        else if (!tagline)
            add_line(-1, store(p, len));

        s = eol;
        if (*s == '\r')
            s++;
        if (*s == '\n')
            s++;
    }

    if (any_time)
    {
        /* Untimed lines in a timed file are headers and notes. */
        int w = first;
        for (int r = first; r < nlines; r++)
            if (lines[r].time >= 0)
                lines[w++] = lines[r];
        nlines = w;
        sort_lines();
        kind = LYRICS_SYNCED;
    }
    else
    {
        /* Plain text: no blank lines at either end. */
        while (nlines > first && !text[lines[first].text])
        {
            memmove(&lines[first], &lines[first + 1],
                    (nlines - first - 1) * sizeof(lines[0]));
            nlines--;
        }
        while (nlines > first && !text[lines[nlines - 1].text])
            nlines--;
        kind = nlines > first ? LYRICS_PLAIN : LYRICS_NONE;
    }
}

/* Recodes raw text and parses it. */
static bool load_text(const unsigned char *src, int len, int enc)
{
    /* Parsed in place in its own buffer, then kept in text[]. */
    static char tmp[TEXT_SIZE];
    reset();
    to_utf8(src, len, enc, tmp, sizeof(tmp));
    parse_text(tmp);
    return kind != LYRICS_NONE && nlines > 0;
}

/* ------------------------------------------------------------ files */

static bool try_file(const char *track, const char *ext)
{
    char path[MAX_PATH];
    const char *dot = strrchr(track, '.');
    const char *slash = strrchr(track, '/');
    int base = (dot && (!slash || dot > slash)) ? dot - track
                                                : (int)strlen(track);
    int fd, len;

    if (base + strlen(ext) >= sizeof(path))
        return false;
    memcpy(path, track, base);
    strcpy(path + base, ext);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    len = read(fd, raw, RAW_SIZE);
    close(fd);
    if (len <= 0)
        return false;
    memset(raw + len, 0, 4);
    return load_text(raw, len, -1);
}

/* ------------------------------------------------------------ ID3v2 */

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static unsigned long le32(const unsigned char *p)
{
    return ((unsigned long)p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
}

static unsigned long syncsafe(const unsigned char *p)
{
    return ((p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) |
           ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

/* Undo unsynchronisation (FF 00 -> FF) in place. */
static int unsync(unsigned char *p, int len)
{
    int r = 0, w = 0;
    while (r < len)
    {
        p[w++] = p[r];
        if (p[r] == 0xff && r + 1 < len && p[r + 1] == 0)
            r++;
        r++;
    }
    return w;
}

/* Length of the string at p in encoding enc, without its terminator;
 * *skip gets the length with it. */
static int id3_strlen(const unsigned char *p, int avail, int enc, int *skip)
{
    int i;
    if (enc == 1 || enc == 2)
    {
        for (i = 0; i + 1 < avail; i += 2)
            if (!p[i] && !p[i + 1])
            {
                *skip = i + 2;
                return i;
            }
        *skip = avail;
        return avail & ~1;
    }
    for (i = 0; i < avail; i++)
        if (!p[i])
        {
            *skip = i + 1;
            return i;
        }
    *skip = avail;
    return avail;
}

static bool parse_uslt(unsigned char *f, int len)
{
    int enc, skip;
    if (len < 5)
        return false;
    enc = f[0];
    f += 4;             /* encoding, language */
    len -= 4;
    id3_strlen(f, len, enc, &skip);     /* content descriptor */
    f += skip;
    len -= skip;
    if (len <= 0)
        return false;
    /* UTF-16 text without its own BOM takes the descriptor's. */
    if (enc == 1 && len >= 2 && memcmp(f, "\xff\xfe", 2) &&
        memcmp(f, "\xfe\xff", 2))
    {
        bool le = f[1] == 0 && f[0] != 0;
        return load_text(f, len, le ? -1 : 2);
    }
    return load_text(f, len, enc);
}

static bool parse_sylt(unsigned char *f, int len)
{
    static char tmp[512];
    int enc, skip, n;
    int line_off = -1;
    bool newline_style = false;
    unsigned char *p, *end = f + len;

    if (len < 7 || f[4] != 2)       /* only millisecond timestamps */
        return false;
    enc = f[0];
    p = f + 6;
    id3_strlen(p, end - p, enc, &skip);
    p += skip;

    /* Karaoke-style SYLT has a syllable per entry and starts lines with a
     * newline; otherwise each entry is a line. */
    for (unsigned char *q = p; q < end; )
    {
        n = id3_strlen(q, end - q, enc, &skip);
        if (n > 0 && to_utf8(q, MIN(n, 8), enc, tmp, sizeof(tmp)) > 0 &&
            (tmp[0] == '\n' || tmp[0] == '\r'))
        {
            newline_style = true;
            break;
        }
        q += skip + 4;
    }

    reset();
    while (p < end)
    {
        long t;
        int ulen;
        n = id3_strlen(p, end - p, enc, &skip);
        if (p + skip + 4 > end)
            break;
        t = be32(p + skip);
        ulen = to_utf8(p, n, enc, tmp, sizeof(tmp));
        char *s = tmp;
        bool starts = !newline_style || line_off < 0;
        if (*s == '\n' || *s == '\r')
        {
            starts = true;
            while (*s == '\n' || *s == '\r')
                s++, ulen--;
        }
        if (starts)
        {
            line_off = store(s, ulen);
            add_line(t, line_off);
        }
        else if (line_off >= 0 && text_used + ulen + 1 <= TEXT_SIZE)
        {
            /* The line being built is the last string stored. */
            memcpy(text + text_used - 1, s, ulen);
            text_used += ulen;
            text[text_used - 1] = '\0';
        }
        p += skip + 4;
    }
    if (nlines == 0)
        return false;
    sort_lines();
    kind = LYRICS_SYNCED;
    return true;
}

/* Walks an ID3v2 tag at the start of fd; prefers SYLT over USLT. */
static bool try_id3v2(int fd)
{
    unsigned char h[10];
    unsigned long size, pos;
    int ver, flags;
    bool global_unsync;
    off_t uslt_at = -1, sylt_at = -1;
    long uslt_len = 0, sylt_len = 0;
    int uslt_flags = 0, sylt_flags = 0;

    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, h, 10) != 10 ||
        memcmp(h, "ID3", 3))
        return false;
    ver = h[3];
    flags = h[5];
    size = syncsafe(h + 6);
    global_unsync = flags & 0x80;
    if (ver < 3 || ver > 4)
        return false;       /* v2.2 (three-letter frames) is rare now */

    pos = 10;
    if (flags & 0x40)
    {
        unsigned char e[4];
        if (read(fd, e, 4) != 4)
            return false;
        pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }

    while (pos + 10 <= size + 10)
    {
        unsigned char fh[10];
        unsigned long flen;
        if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, fh, 10) != 10)
            break;
        if (!fh[0])
            break;          /* padding */
        flen = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        if (!memcmp(fh, "SYLT", 4) && sylt_at < 0)
        {
            sylt_at = pos + 10;
            sylt_len = flen;
            sylt_flags = (fh[8] << 8) | fh[9];
        }
        else if (!memcmp(fh, "USLT", 4) && uslt_at < 0)
        {
            uslt_at = pos + 10;
            uslt_len = flen;
            uslt_flags = (fh[8] << 8) | fh[9];
        }
        pos += 10 + flen;
    }

    for (int pass = 0; pass < 2; pass++)
    {
        off_t at = pass ? uslt_at : sylt_at;
        long len = pass ? uslt_len : sylt_len;
        int ff = pass ? uslt_flags : sylt_flags;
        if (at < 0 || len <= 0)
            continue;
        if (ff & (ver == 4 ? 0x000c : 0x00c0))
            continue;       /* compressed or encrypted */
        if (len > RAW_SIZE)
            len = RAW_SIZE;
        if (lseek(fd, at, SEEK_SET) < 0 || read(fd, raw, len) != len)
            continue;
        if (ver == 4 && (ff & 0x0001))
        {
            memmove(raw, raw + 4, len - 4);     /* data length indicator */
            len -= 4;
        }
        if ((ver == 4 && (ff & 0x0002)) || (ver == 3 && global_unsync))
            len = unsync(raw, len);
        memset(raw + len, 0, 4);
        if (pass ? parse_uslt(raw, len) : parse_sylt(raw, len))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------ Vorbis */

/* A byte source that is either a plain file or the pages of an Ogg
 * stream, so one Vorbis comment reader serves FLAC, Vorbis and Opus. */
struct src
{
    int fd;
    bool ogg;
    long page_left;     /* data bytes left in the current Ogg page */
};

static bool ogg_next_page(struct src *s)
{
    unsigned char h[27], segs[255];
    long total = 0;
    if (read(s->fd, h, 27) != 27 || memcmp(h, "OggS", 4))
        return false;
    if (read(s->fd, segs, h[26]) != h[26])
        return false;
    for (int i = 0; i < h[26]; i++)
        total += segs[i];
    s->page_left = total;
    return true;
}

static bool src_read(struct src *s, unsigned char *buf, long n)
{
    while (n > 0)
    {
        long chunk = n;
        if (s->ogg)
        {
            if (s->page_left == 0 && !ogg_next_page(s))
                return false;
            if (chunk > s->page_left)
                chunk = s->page_left;
        }
        if (buf)
        {
            if (read(s->fd, buf, chunk) != chunk)
                return false;
            buf += chunk;
        }
        else if (lseek(s->fd, chunk, SEEK_CUR) < 0)
            return false;
        if (s->ogg)
            s->page_left -= chunk;
        n -= chunk;
    }
    return true;
}

static bool key_is(const unsigned char *kv, int len, const char *key)
{
    int k = strlen(key);
    return len > k && kv[k] == '=' && !strncasecmp((char *)kv, key, k);
}

/* Reads a Vorbis comment block (vendor, count, entries). Synced lyrics
 * win over plain ones wherever they are. */
static bool read_vorbis_comments(struct src *s)
{
    unsigned char b[4], key[24];
    unsigned long n, count;
    bool found = false;

    if (!src_read(s, b, 4))
        return false;
    if (!src_read(s, NULL, le32(b)))
        return false;
    if (!src_read(s, b, 4))
        return false;
    count = le32(b);

    while (count-- > 0)
    {
        long keylen;
        if (!src_read(s, b, 4))
            break;
        n = le32(b);
        keylen = MIN(n, sizeof(key));
        if (!src_read(s, key, keylen))
            break;

        const char *names[] = { "LYRICS", "UNSYNCEDLYRICS",
                                "UNSYNCED LYRICS", "SYNCEDLYRICS" };
        int match = -1;
        for (int i = 0; i < (int)ARRAYLEN(names); i++)
            if (key_is(key, keylen, names[i]))
                match = i;

        if (match < 0 || (found && kind == LYRICS_SYNCED))
        {
            if (!src_read(s, NULL, n - keylen))
                break;
            continue;
        }

        int klen = strlen(names[match]) + 1;
        long vlen = n - klen;
        long want = MIN(vlen, RAW_SIZE);
        /* The part of the value that came with the key. */
        memcpy(raw, key + klen, keylen - klen);
        if (!src_read(s, raw + keylen - klen, want - (keylen - klen)))
            break;
        if (vlen > want && !src_read(s, NULL, vlen - want))
            break;
        memset(raw + want, 0, 4);

        enum lyrics_kind had = found ? kind : LYRICS_NONE;
        if (load_text(raw, want, 3))
            found = true;
        else if (had != LYRICS_NONE)
            found = false;      /* the earlier ones were overwritten */
    }
    return found;
}

static bool try_flac(int fd, off_t start)
{
    unsigned char h[4];
    struct src s = { fd, false, 0 };

    if (lseek(fd, start, SEEK_SET) < 0 || read(fd, h, 4) != 4 ||
        memcmp(h, "fLaC", 4))
        return false;
    for (;;)
    {
        long len;
        if (read(fd, h, 4) != 4)
            return false;
        len = (h[1] << 16) | (h[2] << 8) | h[3];
        if ((h[0] & 0x7f) == 4)
            return read_vorbis_comments(&s);
        if (h[0] & 0x80)
            return false;
        if (lseek(fd, len, SEEK_CUR) < 0)
            return false;
    }
}

static bool try_ogg(int fd)
{
    unsigned char h[8];
    struct src s = { fd, true, 0 };

    if (lseek(fd, 0, SEEK_SET) != 0 || !ogg_next_page(&s))
        return false;
    /* The identification header is alone on the first page. */
    if (lseek(fd, s.page_left, SEEK_CUR) < 0)
        return false;
    s.page_left = 0;
    if (!src_read(&s, h, 7))
        return false;
    if (!memcmp(h, "\x03vorbis", 7))
        return read_vorbis_comments(&s);
    if (!memcmp(h, "OpusTag", 7) && src_read(&s, h, 1) && h[0] == 's')
        return read_vorbis_comments(&s);
    return false;
}

/* ------------------------------------------------------------ MP4 */

/* Finds atom `name` among the children in [start, end); returns its
 * payload's offset and size. */
static bool mp4_find(int fd, off_t start, off_t end, const char *name,
                     off_t *pay, off_t *paysize)
{
    unsigned char h[16];
    off_t pos = start;
    while (pos + 8 <= end)
    {
        off_t size, hdr = 8;
        if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, h, 8) != 8)
            return false;
        size = be32(h);
        if (size == 1)
        {
            if (read(fd, h + 8, 8) != 8)
                return false;
            size = (off_t)be32(h + 12);     /* files are < 4 GiB here */
            hdr = 16;
        }
        else if (size == 0)
            size = end - pos;
        if (size < hdr)
            return false;
        if (!memcmp(h + 4, name, 4))
        {
            *pay = pos + hdr;
            *paysize = size - hdr;
            return true;
        }
        pos += size;
    }
    return false;
}

static bool try_mp4(int fd)
{
    unsigned char h[8];
    off_t end = filesize(fd), p, n;

    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, h, 8) != 8 ||
        memcmp(h + 4, "ftyp", 4))
        return false;
    if (!mp4_find(fd, 0, end, "moov", &p, &n) ||
        !mp4_find(fd, p, p + n, "udta", &p, &n) ||
        !mp4_find(fd, p, p + n, "meta", &p, &n))
        return false;
    /* meta is a full atom (version and flags) in iTunes files, a plain
     * one in QuickTime files. */
    if (lseek(fd, p, SEEK_SET) < 0 || read(fd, h, 4) != 4)
        return false;
    if (!be32(h))
    {
        p += 4;
        n -= 4;
    }
    if (!mp4_find(fd, p, p + n, "ilst", &p, &n) ||
        !mp4_find(fd, p, p + n, "\xa9lyr", &p, &n) ||
        !mp4_find(fd, p, p + n, "data", &p, &n) || n <= 8)
        return false;
    n -= 8;         /* type and locale */
    if (n > RAW_SIZE)
        n = RAW_SIZE;
    if (lseek(fd, p + 8, SEEK_SET) < 0 || read(fd, raw, n) != n)
        return false;
    memset(raw + n, 0, 4);
    return load_text(raw, n, 3);
}

static bool try_embedded(const char *track)
{
    unsigned char h[10];
    off_t after_id3 = 0;
    bool found = false;
    int fd = open(track, O_RDONLY);
    if (fd < 0)
        return false;

    if (read(fd, h, 10) == 10)
    {
        if (!memcmp(h, "ID3", 3))
        {
            found = try_id3v2(fd);
            after_id3 = 10 + syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);
            if (!found)
                found = try_flac(fd, after_id3);
        }
        else if (!memcmp(h, "fLaC", 4))
            found = try_flac(fd, 0);
        else if (!memcmp(h, "OggS", 4))
            found = try_ogg(fd);
        else if (!memcmp(h + 4, "ftyp", 4))
            found = try_mp4(fd);
    }
    close(fd);
    return found;
}

/* ------------------------------------------------------------ public */

static void load(const char *track)
{
    bool found = false;
    bool file_first =
        global_settings.lyrics_source == LYRICS_FILE_FIRST;

    reset();
    for (int pass = 0; pass < 2 && !found; pass++)
    {
        if ((pass == 0) == file_first)
            found = try_file(track, ".lrc") || try_file(track, ".txt");
        else
            found = try_embedded(track);
    }
    if (!found)
        reset();
    strmemccpy(loaded_path, track, sizeof(loaded_path));
    loaded = true;
    generation++;
}

void lyrics_invalidate(void)
{
    loaded = false;
}

bool lyrics_position(struct lyrics_pos *pos)
{
    struct mp3entry *id3;
    long elapsed;
    int cur = -1;

    memset(pos, 0, sizeof(*pos));
    pos->current = -1;
    if (!global_settings.lyrics_enabled)
        return false;

    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return false;
    if (!loaded || strcmp(loaded_path, id3->path))
        load(id3->path);

    pos->generation = generation;
    if (kind == LYRICS_NONE || nlines == 0)
        return false;

    pos->kind = kind;
    pos->count = nlines;
    elapsed = id3->elapsed;

    if (kind == LYRICS_SYNCED)
    {
        long t = elapsed + offset_ms;
        int lo = 0, hi = nlines - 1;
        while (lo <= hi)            /* last line with time <= t */
        {
            int mid = (lo + hi) / 2;
            if (lines[mid].time <= t)
            {
                cur = mid;
                lo = mid + 1;
            }
            else
                hi = mid - 1;
        }
        if (cur >= 0)
        {
            pos->line_ms = t - lines[cur].time;
            pos->next_ms = cur + 1 < nlines ?
                lines[cur + 1].time - lines[cur].time :
                (long)id3->length - lines[cur].time;
        }
        else
        {
            pos->line_ms = t;
            pos->next_ms = lines[0].time;
        }
    }
    else if (id3->length > 0)
    {
        /* Plain lyrics follow the track through its middle 80 %: most
         * songs neither start nor end with singing. */
        long len = id3->length, lead = len / 10;
        long span = len - 2 * lead;
        long step = span / nlines;
        long t = elapsed - lead;
        if (t < 0)
            t = 0;
        cur = step > 0 ? MIN(t / step, nlines - 1) : 0;
        pos->line_ms = step > 0 ? t - cur * step : 0;
        pos->next_ms = step;
    }
    else
        cur = 0;

    pos->current = cur;
    return true;
}

const char *lyrics_line(int i)
{
    if (i < 0 || i >= nlines)
        return NULL;
    return text + lines[i].text;
}

#endif /* HAVE_LYRICS */
