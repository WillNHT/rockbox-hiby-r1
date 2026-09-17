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
 * The audiobook index.
 *
 * A book is a folder of audio files, or a single .m4b. Folders named like
 * "CD 1" or "Disc 2" inside a book are part of it. The library root never
 * is a book itself: loose files there are one book each.
 *
 * What a book is called comes from, in order: the tags of its first file
 * (album = title, album artist/artist = author, composer = narrator,
 * grouping = series), the folder names (Author/Series/NN - Title), then
 * sidecar files (metadata.json, reader.txt, desc.txt) for whatever is
 * still missing.
 *
 * The file on disk is the memory image: a header, the book records and a
 * string pool. A rescan reads tags only for books whose folder changed.
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "string-extra.h"
#include "strnatcmp.h"
#include "rbunicode.h"
#include "crc32.h"
#include "core_alloc.h"
#include "timefuncs.h"
#include "metadata.h"
#include "filetypes.h"
#include "settings.h"
#include "splash.h"
#include "action.h"
#include "lang.h"
#include "language.h"
#include "abdb.h"

#define DB_MAGIC   "ABDB0001"

struct db_header
{
    char     magic[8];
    uint32_t count;
    uint32_t pool_size;
};

static int db_handle = -1;

static struct db_header *db_hdr(void)
{
    return db_handle > 0 ? core_get_data(db_handle) : NULL;
}

static struct ab_book *db_books(void)
{
    return (struct ab_book *)(db_hdr() + 1);
}

static const char *db_pool(void)
{
    struct db_header *h = db_hdr();
    return (const char *)(h + 1) + h->count * sizeof(struct ab_book);
}

uint32_t ab_now(void)
{
#if CONFIG_RTC
    return (uint32_t)mktime(get_time());
#else
    return current_tick / HZ;
#endif
}

static void db_free(void)
{
    if (db_handle > 0)
    {
        core_unpin(db_handle);
        core_free(db_handle);
    }
    db_handle = -1;
}

bool ab_db_load(void)
{
    if (db_handle > 0)
        return true;

    int fd = open(AB_DB_FILE, O_RDONLY);
    if (fd < 0)
        return false;

    struct db_header h;
    off_t size = filesize(fd);
    bool ok = false;
    if (read(fd, &h, sizeof h) == sizeof h &&
        !memcmp(h.magic, DB_MAGIC, 8) && h.count <= AB_MAX_BOOKS &&
        (off_t)(sizeof h + h.count * sizeof(struct ab_book) + h.pool_size) == size)
    {
        int handle = core_alloc(size);
        if (handle > 0)
        {
            core_pin(handle);
            char *p = core_get_data(handle);
            memcpy(p, &h, sizeof h);
            size_t rest = size - sizeof h;
            if (read(fd, p + sizeof h, rest) == (ssize_t)rest)
            {
                db_handle = handle;
                ok = true;
            }
            else
            {
                core_unpin(handle);
                core_free(handle);
            }
        }
    }
    close(fd);
    return ok;
}

int ab_db_count(void)
{
    struct db_header *h = db_hdr();
    return h ? (int)h->count : 0;
}

const struct ab_book *ab_db_book(int index)
{
    if (index < 0 || index >= ab_db_count())
        return NULL;
    return &db_books()[index];
}

const char *ab_db_str(const struct ab_book *b, enum ab_str s)
{
    return db_pool() + b->str[s];
}

const uint32_t *ab_db_lengths(const struct ab_book *b)
{
    return (const uint32_t *)(db_pool() + b->lengths);
}

int ab_db_find_id(uint32_t id)
{
    int n = ab_db_count();
    const struct ab_book *books = n ? db_books() : NULL;
    for (int i = 0; i < n; i++)
        if (books[i].id == id)
            return i;
    return -1;
}

/* Does path start with the directory dir (case-insensitive, FAT)? */
static bool path_is_under(const char *path, const char *dir, size_t len)
{
    while (len > 1 && dir[len - 1] == '/')
        len--;
    if (len == 0 || strncasecmp(path, dir, len))
        return false;
    return path[len] == '/' || path[len] == '\0';
}

bool ab_path_in_library(const char *path)
{
    const char *root = global_settings.audiobook_folder;
    size_t len = strlen(root);
    if (!path || len == 0 || (len == 1 && root[0] == '/'))
        return false;
    return path_is_under(path, root, len);
}

/* --- listing a book's files ------------------------------------------- */

#define NAMEBUF_SIZE (AB_MAX_FILES * 96)
static char name_buf[NAMEBUF_SIZE];
static const char *name_ptrs[AB_MAX_FILES];

static bool is_audio(const char *name)
{
    return (filetype_get_attr(name) & FILE_ATTR_MASK) == FILE_ATTR_AUDIO;
}

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

/* "CD 1", "cd1", "Disc 02", "Disk3", "Part 1" */
static bool is_disc_folder(const char *name)
{
    static const char * const words[] = { "cd", "disc", "disk", "part" };
    for (size_t i = 0; i < ARRAYLEN(words); i++)
    {
        size_t n = strlen(words[i]);
        if (strncasecmp(name, words[i], n))
            continue;
        const char *p = name + n;
        while (*p == ' ' || *p == '_' || *p == '-')
            p++;
        if (!isdigit((unsigned char)*p))
            continue;
        while (isdigit((unsigned char)*p))
            p++;
        if (*p == '\0')
            return true;
    }
    return false;
}

static int name_cmp(const void *a, const void *b)
{
    return strnatcasecmp(*(const char **)a, *(const char **)b);
}

/* Adds the audio files of dir to the list; with discs also those of disc
 * folders in it. prefix is the path the names are given relative to. */
static void list_dir(const char *dir, bool discs, int *count, size_t *used)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && *count < AB_MAX_FILES)
    {
        if (e->d_name[0] == '.')
            continue;
        struct dirinfo info = dir_get_info(d, e);
        char full[MAX_PATH];
        path_append(full, dir, e->d_name, sizeof full);
        if (info.attribute & ATTR_DIRECTORY)
        {
            if (discs && is_disc_folder(e->d_name))
            {
                /* Recursing holds a second handle open; the disc folder
                 * has no disc folders of its own worth following. */
                list_dir(full, false, count, used);
            }
            continue;
        }
        if (!is_audio(e->d_name))
            continue;
        size_t len = strlen(full) + 1;
        if (*used + len > NAMEBUF_SIZE)
            break;
        memcpy(name_buf + *used, full, len);
        name_ptrs[(*count)++] = name_buf + *used;
        *used += len;
    }
    closedir(d);
}

int ab_db_list_files(const char *path, bool single, const char ***names)
{
    int count = 0;
    size_t used = 0;
    if (single)
    {
        strmemccpy(name_buf, path, NAMEBUF_SIZE);
        name_ptrs[0] = name_buf;
        count = 1;
    }
    else
    {
        list_dir(path, true, &count, &used);
        qsort(name_ptrs, count, sizeof *name_ptrs, name_cmp);
    }
    *names = name_ptrs;
    return count;
}

int ab_db_find_file(const char *path, int *file_index)
{
    int n = ab_db_count();
    if (!path || !n || !ab_path_in_library(path))
        return -1;

    const struct ab_book *books = db_books();
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < n; i++)
    {
        const char *bp = ab_db_str(&books[i], AB_S_PATH);
        size_t len = strlen(bp);
        if (books[i].single ? !strcasecmp(path, bp)
                            : path_is_under(path, bp, len))
        {
            /* the deepest folder wins */
            if (best < 0 || len > best_len)
            {
                best = i;
                best_len = len;
            }
        }
    }
    if (best < 0)
        return -1;

    if (file_index)
    {
        *file_index = 0;
        if (!books[best].single)
        {
            const char **names;
            int count = ab_db_list_files(ab_db_str(&books[best], AB_S_PATH),
                                         false, &names);
            for (int i = 0; i < count; i++)
                if (!strcasecmp(names[i], path))
                {
                    *file_index = i;
                    break;
                }
        }
    }
    return best;
}

/* --- scanning --------------------------------------------------------- */

struct candidate
{
    uint32_t path;     /* offset into cand_paths */
    uint32_t mtime;
    uint16_t nfiles;
    uint8_t  single;
};

#define CAND_POOL_SIZE  (256 * 1024)
#define NEW_POOL_SIZE   (1024 * 1024)

static struct candidate cands[AB_MAX_BOOKS];
static int ncands;
static int cand_handle = -1;
static size_t cand_used;

static int new_handle = -1;       /* new books, then the new pool */
static size_t new_pool_used;
static bool new_pool_full;

#define NEW_BOOKS()  ((struct ab_book *)core_get_data(new_handle))
#define NEW_POOL()   ((char *)core_get_data(new_handle) + \
                      AB_MAX_BOOKS * sizeof(struct ab_book))

static void add_candidate(const char *path, uint32_t mtime, int nfiles,
                          bool single)
{
    size_t len = strlen(path) + 1;
    if (ncands >= AB_MAX_BOOKS || cand_used + len > CAND_POOL_SIZE)
        return;
    memcpy((char *)core_get_data(cand_handle) + cand_used, path, len);
    cands[ncands].path = cand_used;
    cands[ncands].mtime = mtime;
    cands[ncands].nfiles = nfiles;
    cands[ncands].single = single;
    ncands++;
    cand_used += len;
}

static void walk(const char *dir, uint32_t mtime, bool is_root, int depth)
{
    if (depth > 8 || ncands >= AB_MAX_BOOKS)
        return;

    /* First pass: what is in here? */
    int audio = 0, m4b = 0, discs = 0;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        struct dirinfo info = dir_get_info(d, e);
        if (info.attribute & ATTR_DIRECTORY)
            discs += is_disc_folder(e->d_name);
        else if (is_audio(e->d_name))
        {
            audio++;
            m4b += has_ext(e->d_name, ".m4b");
        }
    }
    closedir(d);

    if (!is_root && (audio > m4b || (audio == 0 && discs > 0)))
    {
        /* A folder book; its mtime came from the parent's listing. */
        const char **names;
        int n = ab_db_list_files(dir, false, &names);
        if (n > 0)
        {
            add_candidate(dir, mtime, n, false);
            return;
        }
    }

    /* Otherwise every audio file here is a book, and subfolders are
     * looked at in turn. Names are collected first so no directory handle
     * stays open while recursing. */
    int handle = core_alloc(32 * 1024);
    if (handle <= 0)
        return;
    core_pin(handle);
    char *list = core_get_data(handle);
    size_t used = 0;

    d = opendir(dir);
    if (d)
    {
        while ((e = readdir(d)))
        {
            if (e->d_name[0] == '.')
                continue;
            struct dirinfo info = dir_get_info(d, e);
            bool isdir = info.attribute & ATTR_DIRECTORY;
            if (!isdir && !is_audio(e->d_name))
                continue;
            size_t len = strlen(e->d_name) + 2;
            if (used + len + 4 + 1 > 32 * 1024)
                break;
            list[used] = isdir ? 'd' : 'f';
            uint32_t mt = info.mtime;
            memcpy(list + used + 1, &mt, 4);
            strcpy(list + used + 5, e->d_name);
            used += len + 4;
        }
        closedir(d);
    }
    list[used] = 0;

    for (size_t pos = 0; list[pos] && ncands < AB_MAX_BOOKS; )
    {
        char kind = list[pos];
        uint32_t mt;
        memcpy(&mt, list + pos + 1, 4);
        const char *name = list + pos + 5;
        char full[MAX_PATH];
        path_append(full, dir, name, sizeof full);
        if (kind == 'f')
            add_candidate(full, mt, 1, true);
        else
            walk(full, mt, false, depth + 1);
        pos += strlen(name) + 1 + 5;
    }

    core_unpin(handle);
    core_free(handle);
}

static uint32_t pool_add(const char *s)
{
    if (!s || !*s)
        return 0;
    size_t len = strlen(s) + 1;
    if (new_pool_used + len > NEW_POOL_SIZE)
    {
        new_pool_full = true;
        return 0;
    }
    memcpy(NEW_POOL() + new_pool_used, s, len);
    uint32_t off = new_pool_used;
    new_pool_used += len;
    return off;
}

static uint32_t pool_add_u32s(const uint32_t *v, int n)
{
    size_t pad = (4 - new_pool_used % 4) % 4;
    size_t len = n * sizeof(uint32_t);
    if (new_pool_used + pad + len > NEW_POOL_SIZE)
    {
        new_pool_full = true;
        return 0;
    }
    memset(NEW_POOL() + new_pool_used, 0, pad);
    new_pool_used += pad;
    uint32_t off = new_pool_used;
    memcpy(NEW_POOL() + off, v, len);
    new_pool_used += len;
    return off;
}

/* Strings of one book being put together. */
struct meta
{
    char s[AB_NUM_STR][MAX_PATH];
};

static void set_if_empty(struct meta *m, enum ab_str which, const char *v)
{
    if (m->s[which][0] || !v)
        return;
    while (*v == ' ')
        v++;
    if (!strmemccpy(m->s[which], v, MAX_PATH))
    {
        /* cut short: do not leave half a character at the end */
        char *end = m->s[which] + strlen(m->s[which]);
        char *lead = end;
        while (lead > m->s[which] && (lead[-1] & 0xc0) == 0x80)
            lead--;
        if (lead > m->s[which] && (lead[-1] & 0x80))
        {
            unsigned char c = lead[-1];
            int need = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            if (end - lead != need)
                lead[-1] = '\0';
        }
    }
    /* no trailing blanks or line ends */
    char *end = m->s[which] + strlen(m->s[which]);
    while (end > m->s[which] && (end[-1] == ' ' || end[-1] == '\r' ||
                                 end[-1] == '\n' || end[-1] == '\t'))
        *--end = '\0';
}

/* "Series #3", "Series, Book 3", "Series 3" -> name and number */
static void split_series(struct meta *m, const char *v)
{
    char buf[MAX_PATH];
    strmemccpy(buf, v, sizeof buf);
    char *num = NULL;
    char *p = strrchr(buf, '#');
    if (p)
    {
        num = p + 1;
        *p = '\0';
    }
    else if ((p = strcasestr(buf, ", book ")) || (p = strcasestr(buf, " book ")))
    {
        num = strchr(p + 1, ' ') + 1;
        *p = '\0';
    }
    if (num)
    {
        char *t = buf + strlen(buf);
        while (t > buf && (t[-1] == ' ' || t[-1] == ','))
            *--t = '\0';
    }
    set_if_empty(m, AB_S_SERIES, buf);
    if (num && isdigit((unsigned char)*num))
        set_if_empty(m, AB_S_SERIES_NO, num);
}

/* "03 - Title", "Book 3 - Title", "3. Title" -> 3, Title */
static const char *split_number(const char *name, char *num, size_t numsz)
{
    const char *p = name;
    num[0] = '\0';
    if (!strncasecmp(p, "book ", 5))
        p += 5;
    else if (!strncasecmp(p, "vol ", 4) || !strncasecmp(p, "vol.", 4))
        p += 4;
    else if (*p == '#')
        p++;
    if (!isdigit((unsigned char)*p))
        return name;
    const char *start = p;
    while (isdigit((unsigned char)*p) || *p == '.')
        p++;
    if (p[-1] == '.')
        p--;
    size_t len = MIN((size_t)(p - start), numsz - 1);
    const char *rest = p;
    while (*rest == ' ' || *rest == '-' || *rest == '.' || *rest == '_')
        rest++;
    if (rest == p || !*rest)
        return name;  /* "1984" is a title, not a number */
    memcpy(num, start, len);
    num[len] = '\0';
    while (len > 1 && num[0] == '0')
    {
        memmove(num, num + 1, len);
        len--;
    }
    return rest;
}

static void meta_from_folders(struct meta *m, const char *path, bool single)
{
    const char *root = global_settings.audiobook_folder;
    size_t rlen = strlen(root);
    while (rlen > 0 && root[rlen - 1] == '/')
        rlen--;
    char rel[MAX_PATH];
    strmemccpy(rel, path + rlen + (path[rlen] == '/'), sizeof rel);
    if (single)
    {
        char *dot = strrchr(rel, '.');
        if (dot)
            *dot = '\0';
    }

    char *comps[16];
    int n = 0;
    for (char *save, *tok = strtok_r(rel, "/", &save);
         tok && n < 16; tok = strtok_r(NULL, "/", &save))
        comps[n++] = tok;
    if (n == 0)
        return;

    char num[16];
    const char *title = comps[n - 1];
    if (n >= 3)
    {
        set_if_empty(m, AB_S_AUTHOR, comps[0]);
        set_if_empty(m, AB_S_SERIES, comps[n - 2]);
        title = split_number(title, num, sizeof num);
        if (num[0])
            set_if_empty(m, AB_S_SERIES_NO, num);
    }
    else if (n == 2)
        set_if_empty(m, AB_S_AUTHOR, comps[0]);
    set_if_empty(m, AB_S_TITLE, title);
}

static int read_small(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    /* skip a UTF-8 BOM */
    if (n >= 3 && !memcmp(buf, "\xef\xbb\xbf", 3))
        memmove(buf, buf + 3, n - 2);
    return n;
}

/* Four hex digits, or JSON_BAD_HEX. */
#define JSON_BAD_HEX 0x110000
static unsigned json_hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++)
    {
        int c = tolower((unsigned char)p[i]);
        if (isdigit(c))
            v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v * 16 + (c - 'a' + 10);
        else
            return JSON_BAD_HEX;
    }
    return v;
}

/* The first string value of "key", looking into an array and, for an
 * object, its "name". Enough for Audiobookshelf and OpenAudible files. */
static bool json_string(const char *json, const char *key, char *out, size_t size)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strcasestr(json, pat);
    if (!p)
        return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return false;
    while (*p && *p != '"')
    {
        if (*p == '{')
            return json_string(p, "name", out, size);
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '[')
            return false; /* a number, null, ... */
        p++;
    }
    if (*p != '"')
        return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 4 < size)
    {
        if (*p == '\\' && p[1])
        {
            p++;
            switch (*p)
            {
                case 'n': out[o++] = ' '; break;
                case 't': out[o++] = ' '; break;
                case 'u':
                {
                    unsigned cp = json_hex4(p + 1);
                    if (cp == JSON_BAD_HEX)
                    {
                        out[o++] = 'u';
                        break;
                    }
                    p += 4;
                    /* a surrogate pair is one codepoint */
                    if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' &&
                        p[2] == 'u')
                    {
                        unsigned lo = json_hex4(p + 3);
                        if (lo >= 0xdc00 && lo < 0xe000)
                        {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            p += 6;
                        }
                    }
                    if (cp >= 0xd800 && cp < 0xe000)
                        cp = 0xfffd;
                    unsigned char *u = utf8encode(cp, (unsigned char *)out + o);
                    o = (char *)u - out;
                    break;
                }
                default: out[o++] = *p; break;
            }
            p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

static void meta_from_sidecars(struct meta *m, const char *dir)
{
    static char buf[8192];
    char path[MAX_PATH], val[MAX_PATH];

    path_append(path, dir, "metadata.json", sizeof path);
    if (read_small(path, buf, sizeof buf) > 0)
    {
        if (json_string(buf, "title", val, sizeof val))
            set_if_empty(m, AB_S_TITLE, val);
        if (json_string(buf, "authors", val, sizeof val) ||
            json_string(buf, "author", val, sizeof val))
            set_if_empty(m, AB_S_AUTHOR, val);
        if (json_string(buf, "narrators", val, sizeof val) ||
            json_string(buf, "narrator", val, sizeof val))
            set_if_empty(m, AB_S_NARRATOR, val);
        if (json_string(buf, "series", val, sizeof val))
            split_series(m, val);
        if (json_string(buf, "description", val, sizeof val))
            set_if_empty(m, AB_S_DESC, val);
    }

    path_append(path, dir, "reader.txt", sizeof path);
    if (!m->s[AB_S_NARRATOR][0] && read_small(path, buf, MAX_PATH) > 0)
    {
        char *nl = strpbrk(buf, "\r\n");
        if (nl)
            *nl = '\0';
        set_if_empty(m, AB_S_NARRATOR, buf);
    }

    path_append(path, dir, "desc.txt", sizeof path);
    if (!m->s[AB_S_DESC][0] && read_small(path, buf, MAX_PATH) > 0)
    {
        for (char *c = buf; *c; c++)
            if (*c == '\r' || *c == '\n' || *c == '\t')
                *c = ' ';
        set_if_empty(m, AB_S_DESC, buf);
    }
}

static bool find_cover(const char *dir, const char *stem, char *out, size_t size)
{
    static const char * const names[] = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
    };
    char first[MAX_PATH] = "";
    int best = ARRAYLEN(names) + 1;
    size_t stemlen = stem ? strlen(stem) : 0;

    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        const char *n = e->d_name;
        bool pic = has_ext(n, ".jpg") || has_ext(n, ".jpeg") ||
                   has_ext(n, ".png");
        if (!pic || n[0] == '.')
            continue;
        int rank = ARRAYLEN(names);
        if (stem && !strncasecmp(n, stem, stemlen) && n[stemlen] == '.')
            rank = -1;
        for (size_t i = 0; rank == (int)ARRAYLEN(names) && i < ARRAYLEN(names); i++)
            if (!strcasecmp(n, names[i]))
                rank = i;
        /* any picture will do for a folder book, but not for one file
         * among many */
        if (rank == (int)ARRAYLEN(names) && stem)
            continue;
        if (rank < best)
        {
            best = rank;
            strmemccpy(first, n, sizeof first);
        }
    }
    closedir(d);
    if (!first[0])
        return false;
    path_append(out, dir, first, size);
    return true;
}

static struct mp3entry scan_id3;
static uint32_t lengths_tmp[AB_MAX_FILES];

static bool read_book(const struct candidate *c, const char *path,
                      struct ab_book *b)
{
    static struct meta m;
    memset(&m, 0, sizeof m);
    memset(b, 0, sizeof *b);

    const char **names;
    int n = ab_db_list_files(path, c->single, &names);
    if (n <= 0)
        return false;

    /* names[] is reused by nothing below; copy the first one anyway,
     * since tag reading may need the path after other calls. */
    char first[MAX_PATH];
    strmemccpy(first, names[0], sizeof first);

    uint32_t total = 0;
    for (int i = 0; i < n; i++)
    {
        uint32_t len = 0;
        if (get_metadata(&scan_id3, -1, names[i]))
        {
            len = scan_id3.length;
            if (i == 0)
            {
                set_if_empty(&m, AB_S_TITLE, scan_id3.album);
                if (c->single && !m.s[AB_S_TITLE][0])
                    set_if_empty(&m, AB_S_TITLE, scan_id3.title);
                set_if_empty(&m, AB_S_AUTHOR, scan_id3.albumartist);
                set_if_empty(&m, AB_S_AUTHOR, scan_id3.artist);
                set_if_empty(&m, AB_S_NARRATOR, scan_id3.composer);
                if (scan_id3.comment)
                {
                    const char *cm = scan_id3.comment;
                    if (!strncasecmp(cm, "read by ", 8))
                        set_if_empty(&m, AB_S_NARRATOR, cm + 8);
                    else if (!strncasecmp(cm, "narrated by ", 12))
                        set_if_empty(&m, AB_S_NARRATOR, cm + 12);
                }
                if (scan_id3.grouping && scan_id3.grouping[0])
                    split_series(&m, scan_id3.grouping);
#ifdef HAVE_ALBUMART
                if (scan_id3.has_embedded_albumart)
                {
                    b->cover_type = scan_id3.albumart.type;
                    b->cover_pos = scan_id3.albumart.pos;
                    b->cover_size = scan_id3.albumart.size;
                }
#endif
            }
        }
        lengths_tmp[i] = len;
        total += len;
        if (n > 20)
            yield();
    }

    meta_from_folders(&m, path, c->single);

    char dir[MAX_PATH];
    strmemccpy(dir, path, sizeof dir);
    const char *stem = NULL;
    char stembuf[MAX_PATH];
    if (c->single)
    {
        char *slash = strrchr(dir, '/');
        if (slash)
        {
            strmemccpy(stembuf, slash + 1, sizeof stembuf);
            char *dot = strrchr(stembuf, '.');
            if (dot)
                *dot = '\0';
            stem = stembuf;
            if (slash == dir)
                slash[1] = '\0';
            else
                *slash = '\0';
        }
    }
    if (!c->single)
        meta_from_sidecars(&m, dir);

    if (!find_cover(dir, stem, m.s[AB_S_COVER], MAX_PATH))
    {
        if (b->cover_size)
            strmemccpy(m.s[AB_S_COVER], first, MAX_PATH);
    }
    else
        b->cover_type = b->cover_pos = b->cover_size = 0;

    strmemccpy(m.s[AB_S_PATH], path, MAX_PATH);
    b->id = crc_32(path, strlen(path), 0xffffffff);
    b->mtime = c->mtime;
    b->length = total;
    b->nfiles = n;
    b->single = c->single;
    for (int s = 0; s < AB_NUM_STR; s++)
        b->str[s] = pool_add(m.s[s]);
    b->lengths = pool_add_u32s(lengths_tmp, n);
    return !new_pool_full;
}

/* Copies an unchanged book from the old index. */
static bool copy_book(const struct ab_book *old, struct ab_book *b)
{
    *b = *old;
    for (int s = 0; s < AB_NUM_STR; s++)
        b->str[s] = pool_add(ab_db_str(old, s));
    memcpy(lengths_tmp, ab_db_lengths(old), old->nfiles * sizeof(uint32_t));
    b->lengths = pool_add_u32s(lengths_tmp, old->nfiles);
    return !new_pool_full;
}

static bool write_db(int count)
{
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", AB_DB_FILE);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    struct db_header h;
    memcpy(h.magic, DB_MAGIC, 8);
    h.count = count;
    h.pool_size = new_pool_used;
    bool ok = write(fd, &h, sizeof h) == sizeof h &&
              write(fd, NEW_BOOKS(), count * sizeof(struct ab_book)) ==
                    (ssize_t)(count * sizeof(struct ab_book)) &&
              write(fd, NEW_POOL(), new_pool_used) == (ssize_t)new_pool_used;
    close(fd);
    if (!ok)
    {
        remove(tmp);
        return false;
    }
    remove(AB_DB_FILE);
    return rename(tmp, AB_DB_FILE) == 0;
}

static void free_scan_buffers(void)
{
    if (cand_handle > 0)
    {
        core_unpin(cand_handle);
        core_free(cand_handle);
    }
    if (new_handle > 0)
    {
        core_unpin(new_handle);
        core_free(new_handle);
    }
    cand_handle = new_handle = -1;
}

bool ab_db_scan(bool full, bool quiet)
{
    const char *root = global_settings.audiobook_folder;
    if (!root[0] || !dir_exists(root))
    {
        /* No library: forget the old index so nothing stale shows. */
        if (ab_db_count())
        {
            db_free();
            remove(AB_DB_FILE);
        }
        return false;
    }

    ab_db_load();

    cand_handle = core_alloc(CAND_POOL_SIZE);
    new_handle = core_alloc(AB_MAX_BOOKS * sizeof(struct ab_book) + NEW_POOL_SIZE);
    if (cand_handle <= 0 || new_handle <= 0)
    {
        if (cand_handle > 0)
            core_free(cand_handle);
        if (new_handle > 0)
            core_free(new_handle);
        cand_handle = new_handle = -1;
        return false;
    }
    core_pin(cand_handle);
    core_pin(new_handle);
    ncands = 0;
    cand_used = 0;
    new_pool_used = 1;  /* offset 0 is the empty string */
    NEW_POOL()[0] = '\0';
    new_pool_full = false;

    if (!quiet)
        splashf(0, "%s", str(LANG_AB_SCANNING));

    char rootbuf[MAX_PATH];
    strmemccpy(rootbuf, root, sizeof rootbuf);
    size_t rl = strlen(rootbuf);
    while (rl > 1 && rootbuf[rl - 1] == '/')
        rootbuf[--rl] = '\0';
    walk(rootbuf, 0, true, 0);

    /* How much reading is there to do? */
    int old_count = ab_db_count();
    int todo = 0;
    static int8_t reuse[AB_MAX_BOOKS];   /* 1 = copy from old index */
    static int16_t old_idx[AB_MAX_BOOKS];
    for (int i = 0; i < ncands; i++)
    {
        const char *p = (char *)core_get_data(cand_handle) + cands[i].path;
        uint32_t id = crc_32(p, strlen(p), 0xffffffff);
        int o = ab_db_find_id(id);
        old_idx[i] = o;
        reuse[i] = !full && o >= 0 &&
                   ab_db_book(o)->mtime == cands[i].mtime &&
                   ab_db_book(o)->nfiles == cands[i].nfiles &&
                   ab_db_book(o)->single == cands[i].single;
        if (!reuse[i])
            todo++;
    }

    if (!full && todo == 0 && ncands == old_count)
    {
        free_scan_buffers();
        return true;  /* nothing changed */
    }

    bool cancelled = false;
    int count = 0, done = 0;
    uint32_t now = ab_now();
    for (int i = 0; i < ncands && !new_pool_full; i++)
    {
        char path[MAX_PATH];
        strmemccpy(path, (char *)core_get_data(cand_handle) + cands[i].path,
                   sizeof path);
        struct ab_book b;
        bool ok;
        if (reuse[i])
            ok = copy_book(ab_db_book(old_idx[i]), &b);
        else
        {
            done++;
            splash_progress(done, todo, "%s (%d/%d)",
                            str(LANG_AB_SCANNING), done, todo);
            ok = read_book(&cands[i], path, &b);
            b.added = old_idx[i] >= 0 ? ab_db_book(old_idx[i])->added : 0;
            if (!b.added)
                b.added = cands[i].mtime ? cands[i].mtime : now;
            if (action_userabort(TIMEOUT_NOBLOCK))
            {
                cancelled = true;
                break;
            }
        }
        if (ok)
            NEW_BOOKS()[count++] = b;
    }

    bool ok = false;
    if (!cancelled)
    {
        db_free();
        ok = write_db(count);
        ab_db_load();
    }
    free_scan_buffers();
    return ok;
}
