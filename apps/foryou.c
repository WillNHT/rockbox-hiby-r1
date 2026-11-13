/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * For You: mixes, charts and recaps out of the listening log (#28).
 *
 * Everything here is a query over /.rockbox/listens.tsv (apps/listens.c):
 * a sequential read when a screen is opened, never on the playback path,
 * and nothing kept between screens. The mixes are the same read with a
 * weight per track and a playlist written at the end of it.
 *
 * There are no ratings. How a track was listened to is the rating: heard
 * through counts for it, skipped counts against it.
 *
 * The folder a track is in stands for its album and the folder above that
 * for its artist, because that is what the log has without opening files.
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
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <time.h>

#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "file.h"
#include "lang.h"
#include "list.h"
#include "menu.h"
#include "metadata.h"
#include "misc.h"
#include "playlist.h"
#include "root_menu.h"
#include "settings.h"
#include "splash.h"
#include "string-extra.h"
#include "crc32.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "pradio.h"
#include "audiobooks/audiobooks.h"
#include "listens.h"
#include "foryou.h"

#define DAYSEC 86400L

/* --- the clock --------------------------------------------------------- */

/* The log is in UTC; hours, days and months are the listener's. Every time
 * below is local: the zone is added as a row is read. */
static long tz;
static long now;

static void clock_now(void)
{
    static bool seeded;
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    tz = tm.tm_hour * 3600L + tm.tm_min * 60 + tm.tm_sec - (long)(t % DAYSEC);
    if (tz > DAYSEC / 2)
        tz -= DAYSEC;
    else if (tz < -DAYSEC / 2)
        tz += DAYSEC;
    now = (long)t + tz;

    if (!seeded)
    {
        srand(current_tick);
        seeded = true;
    }
}

static void local_tm(long t, struct tm *tm)
{
    time_t tt = t;
    gmtime_r(&tt, tm);
}

static long month_start(long t)
{
    struct tm tm;
    local_tm(t, &tm);
    return t - t % DAYSEC - (tm.tm_mday - 1) * DAYSEC;
}

static long year_start(long t)
{
    struct tm tm;
    local_tm(t, &tm);
    return t - t % DAYSEC - tm.tm_yday * DAYSEC;
}

/* --- reading the log --------------------------------------------------- */

struct row
{
    char *path;
    long started;
    unsigned long played;       /* ms */
    bool heard;                 /* finished, or at least half of it */
    char source;                /* 'm'usic, 'a'udiobook, 'r'adio */
    uint32_t where;             /* finds the row again: see row_path() */
};

static const char * const logs[] = { LISTENS_OLD_FILE, LISTENS_FILE };
static char line[MAX_PATH + 160];

static char *field(char **p)
{
    char *f = *p;

    if (f && (*p = strchr(f, '\t')))
        *(*p)++ = '\0';
    return f;
}

/* Every listen that started in [from, to), oldest first.
 * ponytail: read_line is a few syscalls a row, so a full 8 MiB log is
 * around a second on the device; a year of listening is a few hundred KB.
 * Give this its own buffer if that second ever shows. */
static void scan(long from, long to, void (*fn)(const struct row *r))
{
    for (int f = 0; f < 2; f++)
    {
        int fd = open(logs[f], O_RDONLY);
        long off = 0;
        int n;

        if (fd < 0)
            continue;
        while ((n = read_line(fd, line, sizeof(line))) > 0)
        {
            struct row r;
            char *p = line;

            r.where = off * 2 + f;
            off += n;
            r.path = field(&p);
            char *started = field(&p);
            char *played = field(&p);
            char *length = field(&p);
            char *ended = field(&p);
            char *source = field(&p);

            /* a short line is a cut one; no start is the header */
            if (!source || (r.started = atol(started)) == 0)
                continue;
            r.started += tz;
            if (r.started < from || r.started >= to)
                continue;
            r.played = strtoul(played, NULL, 10);
            r.heard = *ended == 'f' ||
                      r.played * 2 >= strtoul(length, NULL, 10);
            r.source = *source;
            fn(&r);
        }
        close(fd);
    }
}

/* The path of a row seen earlier. A row is remembered as where it is in
 * the log rather than as its path: four bytes instead of a few hundred. */
static bool row_path(uint32_t where, char *buf, size_t size)
{
    int fd = open(logs[where & 1], O_RDONLY);
    int n;

    if (fd < 0)
        return false;
    lseek(fd, where / 2, SEEK_SET);
    n = read_line(fd, buf, size);
    close(fd);
    if (n <= 0)
        return false;
    char *tab = strchr(buf, '\t');
    if (tab)
        *tab = '\0';
    return true;
}

/* --- tracks, albums, artists ------------------------------------------- */

enum { BY_TRACK, BY_ALBUM, BY_ARTIST };

/* How much of a path names the track, its folder, or the folder above */
static size_t path_len(const char *path, int kind)
{
    size_t len = strlen(path);

    while (kind-- > 0)
    {
        while (len > 0 && path[len - 1] != '/')
            len--;
        if (len > 0)
            len--;
    }
    return len;
}

static uint32_t path_key(const char *path, int kind)
{
    uint32_t key = crc_32(path, path_len(path, kind), 0xffffffff);
    return key ? key : 1;       /* 0 is an empty slot */
}

/* What to call it on screen. Cuts the path it is given. */
static const char *path_name(char *path, int kind)
{
    path[path_len(path, kind)] = '\0';
    char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (kind == BY_TRACK)
    {
        char *dot = strrchr(name, '.');
        if (dot && dot != name)
            *dot = '\0';
    }
    return name;
}

/* --- the tally --------------------------------------------------------- */

/* One entry per track, album or artist, whichever is being counted.
 * Fixed: past TALLY_SIZE * 3/4 of them, new ones are not counted. */
struct tally
{
    uint32_t key;
    uint32_t where;             /* its latest row */
    uint32_t artist;
    uint32_t first, last;       /* started, local */
    uint32_t secs;
    uint16_t plays, skips;
    uint16_t near;              /* times heard around the seed track */
    uint16_t picked;
};

#define TALLY_SIZE 8192         /* a power of two */
static struct tally tally[TALLY_SIZE];
static int ntally;
static int tally_kind, tally_hour;

/* "Because you played": tracks heard within half an hour of the seed */
#define NEAR_SECS 1800
static uint32_t seed_key, seed_artist;
static long seed_at;
static struct
{
    struct tally *t;
    long at;
} recent[8];
static unsigned nrecent;

#define HEARD(t) ((t)->plays - (t)->skips)

static struct tally *tally_find(uint32_t key, bool add)
{
    unsigned i = key & (TALLY_SIZE - 1);

    while (tally[i].key && tally[i].key != key)
        i = (i + 1) & (TALLY_SIZE - 1);
    if (!tally[i].key)
    {
        if (!add || ntally >= TALLY_SIZE * 3 / 4)
            return NULL;
        tally[i].key = key;
        ntally++;
    }
    return &tally[i];
}

static void tally_row(const struct row *r)
{
    /* books and stations are not songs: they are in the totals instead */
    if (r->source != 'm')
        return;
    if (tally_hour >= 0 && r->started % DAYSEC / 3600 != tally_hour)
        return;

    struct tally *t = tally_find(path_key(r->path, tally_kind), true);
    if (!t)
        return;
    if (!t->plays)
    {
        t->first = r->started;
        t->artist = path_key(r->path, BY_ARTIST);
    }
    if (t->plays < USHRT_MAX)
        t->plays++;
    if (!r->heard)
        t->skips++;
    t->secs += r->played / 1000;
    t->last = r->started;
    t->where = r->where;

    if (!seed_key)
        return;
    if (t->key == seed_key)
    {
        seed_at = r->started;
        for (int i = 0; i < 8; i++)
            if (recent[i].t && r->started - recent[i].at < NEAR_SECS)
                recent[i].t->near++;
    }
    else if (seed_at && r->started - seed_at < NEAR_SECS)
        t->near++;
    recent[nrecent % 8].t = t;
    recent[nrecent++ % 8].at = r->started;
}

static void tally_log(int kind, long from, long to, int hour)
{
    memset(tally, 0, sizeof(tally));
    memset(recent, 0, sizeof(recent));
    ntally = 0;
    seed_at = 0;
    tally_kind = kind;
    tally_hour = hour;
    scan(from, to, tally_row);
}

static void tally_all_tracks(void)
{
    tally_log(BY_TRACK, 0, LONG_MAX, -1);
}

/* --- weights ----------------------------------------------------------- */

/* Each is what an entry is worth to one list; 0 keeps it out. */
typedef long (*weight_fn)(const struct tally *t);

/* The stand-in for a rating. A track skipped more than it is heard sinks
 * to 1 - still there, rarely picked - and comes back if that changes. */
static long like(const struct tally *t)
{
    long w = 10 + 10 * HEARD(t) - 6 * t->skips;
    return w < 1 ? 1 : w;
}

static long age(const struct tally *t)
{
    return now - (long)t->last;
}

static long w_foryou(const struct tally *t)
{
    if (age(t) < DAYSEC)
        return 1;               /* just heard: let it rest */
    return age(t) > 14 * DAYSEC ? like(t) * 2 : like(t);
}

static long w_familiar(const struct tally *t)
{
    return HEARD(t) >= 3 ? like(t) : 0;
}

static long w_lesser(const struct tally *t)
{
    return HEARD(t) < 3 ? like(t) : 0;
}

static long w_rediscover(const struct tally *t)
{
    return age(t) > global_settings.foryou_age * DAYSEC ? like(t) : 0;
}

static long w_forgotten(const struct tally *t)
{
    return HEARD(t) >= 5 ? w_rediscover(t) : 0;
}

static long w_album(const struct tally *t)
{
    return HEARD(t) >= 3 && age(t) > global_settings.foryou_age * DAYSEC
           ? HEARD(t) : 0;
}

static long w_similar(const struct tally *t)
{
    if (t->key == seed_key)
        return 0;
    return ((t->artist == seed_artist ? 3 : 0) + 2 * t->near) * like(t);
}

static long w_heard(const struct tally *t)
{
    return HEARD(t) * 4096L + MIN(t->secs / 60, 4095) + 1;
}

static long w_skipped(const struct tally *t)
{
    return t->skips >= 2 ? t->skips * 100L / t->plays * 256 + t->skips : 0;
}

static long first_from;
static long w_first(const struct tally *t)
{
    return (long)t->first >= first_from ? (long)t->first : 0;
}

/* The n heaviest, heaviest first. Returns how many there were. */
static int tally_top(struct tally **out, int n, weight_fn weight)
{
    int count = 0;

    while (count < n)
    {
        struct tally *best = NULL;
        long best_w = 0;

        for (int i = 0; i < TALLY_SIZE; i++)
        {
            struct tally *t = &tally[i];
            long w;

            if (t->key && !t->picked && (w = weight(t)) > best_w)
            {
                best = t;
                best_w = w;
            }
        }
        if (!best)
            break;
        best->picked = 1;
        out[count++] = best;
    }
    return count;
}

/* --- text screens ------------------------------------------------------ */

/* A screen is a list of lines. A line that names a track or an album can
 * be played from it. */
enum { PLAY_NONE, PLAY_TRACK, PLAY_FOLDER };

#define TEXT_LINES 64
static char text[TEXT_LINES][72];
static uint32_t text_where[TEXT_LINES];
static unsigned char text_play[TEXT_LINES];
static int ntext;

static void say(const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    va_list ap;

    if (ntext >= TEXT_LINES)
        return;
    va_start(ap, fmt);
    vsnprintf(text[ntext], sizeof(text[0]), fmt, ap);
    va_end(ap);
    text_play[ntext++] = PLAY_NONE;
}

/* The line just said plays this row */
static void say_plays(int play, uint32_t where)
{
    if (ntext > 0)
    {
        text_play[ntext - 1] = play;
        text_where[ntext - 1] = where;
    }
}

static void say_time(const char *what, unsigned long secs)
{
    say("%s: %luh %02lum", what, secs / 3600, secs / 60 % 60);
}

static const char *text_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return text[item];
}

/* --- playing ----------------------------------------------------------- */

/* A mix is a playlist file, written whole and then played: the order it
 * is written in is the order it plays in. Written beside itself first -
 * Smart Shuffle reads the playlist it is about to replace. */
#define MIX_FILE "foryou.m3u8"
#define MIX_PATH ROCKBOX_DIR "/" MIX_FILE
static int mix_fd;
static int mix_count;

static bool mix_open(void)
{
    if (!warn_on_pl_erase())
        return false;
    mix_count = 0;
    mix_fd = creat(MIX_PATH ".tmp", 0666);
    return mix_fd >= 0;
}

static void mix_add(const char *path)
{
    if (!file_exists(path))     /* moved or deleted since it was heard */
        return;
    fdprintf(mix_fd, "%s\n", path);
    mix_count++;
}

static int nothing(void)
{
    splash(HZ * 2, ID2P(LANG_FORYOU_EMPTY));
    return 0;
}

static int mix_play(int start)
{
    close(mix_fd);
    rename(MIX_PATH ".tmp", MIX_PATH);
    if (mix_count == 0 || playlist_create(ROCKBOX_DIR, MIX_FILE) < 0)
        return nothing();
    playlist_start(start < mix_count ? start : 0, 0, 0);
    return GO_TO_WPS;
}

static int play_folder(uint32_t where)
{
    char path[MAX_PATH];

    if (!row_path(where, path, sizeof(path)) || !warn_on_pl_erase())
        return 0;
    path[path_len(path, BY_ALBUM)] = '\0';
    if (playlist_create(path, NULL) < 0 ||
        playlist_insert_directory_ex(NULL, path, PLAYLIST_INSERT_LAST,
                                     false, false, false) < 0 ||
        playlist_amount() == 0)
        return nothing();
    playlist_start(0, 0, 0);
    return GO_TO_WPS;
}

/* Every track on the screen, starting with the one that was chosen */
static int play_tracks(int chosen)
{
    char path[MAX_PATH];
    int start = 0;

    if (!mix_open())
        return 0;
    for (int i = 0; i < ntext; i++)
    {
        if (text_play[i] != PLAY_TRACK ||
            !row_path(text_where[i], path, sizeof(path)))
            continue;
        if (i == chosen)
            start = mix_count;
        mix_add(path);
    }
    return mix_play(start);
}

static int show(const char *title)
{
    int selection = 0;

    if (ntext == 0)
        return nothing();
    while (1)
    {
        struct simplelist_info info;
        int ret = 0;

        simplelist_info_init(&info, (char *)title, ntext, NULL);
        info.get_name = text_name;
        info.selection = selection;
        simplelist_show_list(&info);
        if (info.selection < 0 || info.selection >= ntext)
            return 0;
        selection = info.selection;

        if (text_play[selection] == PLAY_TRACK)
            ret = play_tracks(selection);
        else if (text_play[selection] == PLAY_FOLDER)
            ret = play_folder(text_where[selection]);
        if (ret)
            return ret;
    }
}

/* --- sequencing -------------------------------------------------------- */

/* An order to play things in: shuffled, then with no artist twice in a row
 * wherever the tracks allow it. */
#define SEQ_MAX 2000
static uint16_t seq[SEQ_MAX];
static uint32_t seq_artist[SEQ_MAX];
static unsigned char seq_low[SEQ_MAX];

static void seq_swap(int a, int b)
{
    uint16_t t = seq[a];
    seq[a] = seq[b];
    seq[b] = t;
}

static void spread(int from, int to)
{
    for (int i = from + 1; i < to; i++)
    {
        if (seq_artist[seq[i]] != seq_artist[seq[i - 1]])
            continue;
        for (int j = i + 1; j < to; j++)
        {
            if (seq_artist[seq[j]] != seq_artist[seq[i - 1]])
            {
                seq_swap(i, j);
                break;
            }
        }
    }
}

/* Orders n things whose seq_artist[] and seq_low[] are filled in. The low
 * ones - what the listener keeps skipping - go to the back. */
static void sequence(int n)
{
    int end = n;

    for (int i = 0; i < n; i++)
        seq[i] = i;
    for (int i = n - 1; i > 0; i--)
        seq_swap(i, rand() % (i + 1));
    for (int i = 0; i < end; )
    {
        if (seq_low[seq[i]])
            seq_swap(i, --end);
        else
            i++;
    }
    spread(0, end);
    spread(end, n);
}

/* --- from the database ------------------------------------------------- */

/* Tracks the log knows nothing about have to come from somewhere: these
 * are a few chosen evenly from the whole database, however big it is. */
#define FOUND_MAX 50
static char found[FOUND_MAX][MAX_PATH];
static int nfound;

/* How many chances a database track gets; 0 leaves it out */
typedef int (*want_fn)(struct tagcache_search *tcs, const char *path);

static void db_sample(int n, want_fn want)
{
    static struct tagcache_search tcs;
    static char fname[MAX_PATH];
    long seen = 0;

    nfound = 0;
    if (n > FOUND_MAX)
        n = FOUND_MAX;
    if (n <= 0 || !tagcache_is_usable() || !tagcache_search(&tcs, tag_filename))
        return;
    while (tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        for (int chances = want(&tcs, fname); chances > 0; chances--)
        {
            long slot = seen < n ? seen : rand() % (seen + 1);
            seen++;
            if (slot >= n)
                continue;
            strmemccpy(found[slot], fname, MAX_PATH);
            if (slot >= nfound)
                nfound = slot + 1;
        }
        yield();
    }
    tagcache_search_finish(&tcs);
}

/* Never played, as far as the log goes. Something added in the last month
 * gets three chances instead of one: the push a new arrival needs. */
static int want_unheard(struct tagcache_search *tcs, const char *path)
{
    if (tally_find(path_key(path, BY_TRACK), false) ||
        pradio_is_station_track(path) || audiobooks_is_book(path))
        return 0;
    return tagcache_get_numeric(tcs, tag_mtime) > now - tz - 30 * DAYSEC ? 3 : 1;
}

/* Mood has no signal on the card but the genre tag, so that is what this
 * reads until something better writes one. */
static const char * const moods[][9] =
{
    { "chill", "ambient", "lo-fi", "lofi", "downtempo", "jazz", "acoustic",
      "folk", NULL },
    { "rock", "metal", "punk", "dance", "electro", "techno", "drum", "hip",
      NULL },
    { "rock", "synth", "pop", "indie", "house", "funk", "disco", "country",
      NULL },
    { "classical", "piano", "new age", "soul", "bossa", "easy", "blues",
      "instrumental", NULL },
};
static int mood;

static int want_mood(struct tagcache_search *tcs, const char *path)
{
    char genre[64];
    struct tally *t = tally_find(path_key(path, BY_TRACK), false);

    if ((t && t->plays >= 3 && t->skips * 2 > t->plays) ||
        !tagcache_retrieve(tcs, tcs->idx_id, tag_genre, genre, sizeof(genre)))
        return 0;
    for (int i = 0; moods[mood][i]; i++)
        if (strcasestr(genre, moods[mood][i]))
            return 1;
    return 0;
}

/* --- the mixes --------------------------------------------------------- */

static struct tally *picks[FOUND_MAX];
static int npicks;

/* n more tracks out of the tally, by lot: the heavier, the likelier */
static void pick(weight_fn weight, int n)
{
    while (n-- > 0 && npicks < FOUND_MAX)
    {
        unsigned long total = 0, r;

        for (int i = 0; i < TALLY_SIZE; i++)
            if (tally[i].key && !tally[i].picked)
                total += weight(&tally[i]);
        if (!total)
            return;
        r = rand() % total;
        for (int i = 0; i < TALLY_SIZE; i++)
        {
            struct tally *t = &tally[i];
            unsigned long w;

            if (!t->key || t->picked)
                continue;
            w = weight(t);
            if (r < w)
            {
                t->picked = 1;
                picks[npicks++] = t;
                break;
            }
            r -= w;
        }
    }
}

/* What was picked and what was found, sequenced and played */
static int mix_finish(void)
{
    char path[MAX_PATH];
    int kept = 0;

    for (int i = 0; i < npicks; i++)
    {
        seq_artist[i] = picks[i]->artist;
        seq_low[i] = 0;
    }
    for (int i = 0; i < nfound; i++)
    {
        int j;
        for (j = 0; j < kept; j++)      /* a track with three chances */
            if (!strcmp(found[i], found[j]))
                break;
        if (j < kept)
            continue;
        if (kept != i)
            strcpy(found[kept], found[i]);
        seq_artist[npicks + kept] = path_key(found[kept], BY_ARTIST);
        seq_low[npicks + kept] = 0;
        kept++;
    }
    if (npicks + kept == 0)
        return nothing();

    sequence(npicks + kept);
    if (!mix_open())
        return 0;
    for (int i = 0; i < npicks + kept; i++)
    {
        int k = seq[i];
        if (k >= npicks)
            mix_add(found[k - npicks]);
        else if (row_path(picks[k]->where, path, sizeof(path)))
            mix_add(path);
    }
    return mix_play(0);
}

static void mix_begin(void)
{
    clock_now();
    seed_key = 0;
    npicks = nfound = 0;
    splash(0, ID2P(LANG_WAIT));
    tally_all_tracks();
}

static int mix_of(weight_fn weight)
{
    mix_begin();
    pick(weight, global_settings.foryou_mix_size);
    return mix_finish();
}

/* Mostly what the listener likes, a fifth of it never heard */
static int foryou_mix(void)
{
    int n = global_settings.foryou_mix_size;

    mix_begin();
    db_sample(n / 5, want_unheard);
    pick(w_foryou, n - nfound);
    return mix_finish();
}

/* 70% familiar, 20% barely played, 10% never played */
static int daily_mix(void)
{
    int n = global_settings.foryou_mix_size;

    mix_begin();
    db_sample(n / 10, want_unheard);
    pick(w_lesser, n / 5);
    pick(w_familiar, n - nfound - npicks);
    pick(w_foryou, n - nfound - npicks);    /* not enough of either yet */
    return mix_finish();
}

static int rediscover_mix(void)
{
    return mix_of(w_rediscover);
}

static int forgotten_mix(void)
{
    return mix_of(w_forgotten);
}

/* A mini-radio around what is playing, or around the last thing heard:
 * the same artist, and whatever tends to be played next to it. */
static int similar_mix(void)
{
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;

    mix_begin();
    if (id3 && id3->path[0])
    {
        seed_key = path_key(id3->path, BY_TRACK);
        seed_artist = path_key(id3->path, BY_ARTIST);
    }
    else
    {
        struct tally *last = NULL;

        for (int i = 0; i < TALLY_SIZE; i++)
            if (tally[i].key && (!last || tally[i].last > last->last))
                last = &tally[i];
        if (!last)
            return nothing();
        seed_key = last->key;
        seed_artist = last->artist;
    }
    tally_all_tracks();         /* again, now that there is a seed */
    pick(w_similar, global_settings.foryou_mix_size);
    return mix_finish();
}

static int mood_mix(void *param)
{
    mood = (intptr_t)param;
    mix_begin();
    db_sample(global_settings.foryou_mix_size, want_mood);
    return mix_finish();
}

/* The playlist that is on, reordered: no artist twice in a row, and what
 * keeps getting skipped at the back. */
static int smart_shuffle(void)
{
    static struct playlist_track_info info;
    int n = playlist_amount();

    if (n < 2)
        return nothing();
    if (n > SEQ_MAX)
    {
        splash(HZ * 2, ID2P(LANG_PLAYLIST_BUFFER_FULL));
        return 0;
    }
    clock_now();
    seed_key = 0;
    splash(0, ID2P(LANG_WAIT));
    tally_all_tracks();

    for (int i = 0; i < n; i++)
    {
        if (playlist_get_track_info(NULL, i, &info) < 0)
            return nothing();
        struct tally *t = tally_find(path_key(info.filename, BY_TRACK), false);
        seq_artist[i] = path_key(info.filename, BY_ARTIST);
        seq_low[i] = t && t->plays >= 3 && t->skips * 2 > t->plays;
    }
    sequence(n);

    if (!mix_open())
        return 0;
    for (int i = 0; i < n; i++)
        if (playlist_get_track_info(NULL, seq[i], &info) == 0)
            mix_add(info.filename);
    return mix_play(0);
}

/* --- lists that play --------------------------------------------------- */

static void say_top(int kind, int n, weight_fn weight, int play)
{
    struct tally *top[TEXT_LINES];
    char path[MAX_PATH];
    int count = tally_top(top, MIN(n, TEXT_LINES), weight);

    for (int i = 0; i < count; i++)
    {
        if (!row_path(top[i]->where, path, sizeof(path)))
            continue;
        if (weight == w_skipped)
            say("%s: %d/%d", path_name(path, kind), top[i]->skips,
                top[i]->plays);
        else if (weight == w_album)
            say("%s: %ldd", path_name(path, kind), age(top[i]) / DAYSEC);
        else if (weight == w_first)
            say("%s", path_name(path, kind));
        else
            say("%d. %s (%d)", i + 1, path_name(path, kind), HEARD(top[i]));
        say_plays(play, top[i]->where);
    }
}

/* "You haven't finished this album in 8 months" */
static int album_rediscovery(void)
{
    clock_now();
    ntext = 0;
    tally_log(BY_ALBUM, 0, LONG_MAX, -1);
    say_top(BY_ALBUM, 40, w_album, PLAY_FOLDER);
    return show(str(LANG_FORYOU_ALBUMS));
}

static struct tm today;

static void years_ago_row(const struct row *r)
{
    struct tm tm;

    if (r->source != 'm' || now - r->started < 300 * DAYSEC)
        return;
    local_tm(r->started, &tm);
    if (tm.tm_mon != today.tm_mon || tm.tm_mday != today.tm_mday)
        return;
    say("%d: %s", tm.tm_year + 1900, path_name(r->path, BY_TRACK));
    say_plays(PLAY_TRACK, r->where);
}

/* What was playing on this day in other years */
static int years_ago(void)
{
    clock_now();
    ntext = 0;
    local_tm(now, &today);
    scan(0, LONG_MAX, years_ago_row);
    return show(str(LANG_FORYOU_YEARS_AGO));
}

/* --- totals ------------------------------------------------------------ */

#define CAL_DAYS 1096           /* three years of days */
static struct
{
    unsigned long secs[3];      /* music, audiobooks, radio */
    unsigned long hours[24];
    long listens, heard;
} sum;
static unsigned short cal[CAL_DAYS];    /* minutes, by how many days ago */

static void sum_row(const struct row *r)
{
    long ago = now / DAYSEC - r->started / DAYSEC;

    sum.secs[r->source == 'm' ? 0 : r->source == 'a' ? 1 : 2] +=
        r->played / 1000;
    sum.hours[r->started % DAYSEC / 3600] += r->played / 1000;
    sum.listens++;
    sum.heard += r->heard;
    if (ago >= 0 && ago < CAL_DAYS)     /* any listen at all marks the day */
        cal[ago] = MIN(cal[ago] + r->played / 60000 + 1, USHRT_MAX);
}

static void sum_log(long from, long to)
{
    memset(&sum, 0, sizeof(sum));
    memset(cal, 0, sizeof(cal));
    scan(from, to, sum_row);
}

static int peak_hour(void)
{
    int peak = 0;

    for (int h = 1; h < 24; h++)
        if (sum.hours[h] > sum.hours[peak])
            peak = h;
    return peak;
}

/* The longest run of days listened on, and the one still going */
static void streaks(int *current, int *best)
{
    int run = 0;

    *best = 0;
    *current = -1;
    for (int i = 0; i < CAL_DAYS; i++)
    {
        if (cal[i])
            run++;
        /* today not listened on yet does not break a streak */
        else if (i > 0 || run > 0)
        {
            if (*current < 0)
                *current = run;
            run = 0;
        }
        if (run > *best)
            *best = run;
    }
    if (*current < 0)
        *current = run;
}

static int weeks_running(void)
{
    int weeks = 0;

    for (int w = 0; w < CAL_DAYS / 7; w++, weeks++)
    {
        int d;
        for (d = 0; d < 7; d++)
            if (cal[w * 7 + d])
                break;
        if (d == 7)
            break;
    }
    return weeks;
}

/* --- charts and recaps ------------------------------------------------- */

static void say_chart(int kind, int title, int n, long from, long to)
{
    tally_log(kind, from, to, -1);
    if (ntally == 0)
        return;
    say("- %s -", str(title));
    say_top(kind, n, w_heard, kind == BY_TRACK ? PLAY_TRACK :
                              kind == BY_ALBUM ? PLAY_FOLDER : PLAY_NONE);
}

enum { THIS_WEEK, THIS_MONTH, THIS_YEAR, ALL_TIME };

static int charts(void *param)
{
    long from = 0;

    clock_now();
    ntext = 0;
    switch ((intptr_t)param)
    {
        case THIS_WEEK:  from = now - 7 * DAYSEC; break;
        case THIS_MONTH: from = month_start(now); break;
        case THIS_YEAR:  from = year_start(now); break;
    }
    say_chart(BY_TRACK, LANG_ID3_TITLE, 10, from, LONG_MAX);
    say_chart(BY_ARTIST, LANG_ID3_ARTIST, 10, from, LONG_MAX);
    say_chart(BY_ALBUM, LANG_ID3_ALBUM, 10, from, LONG_MAX);
    return show(str(LANG_FORYOU_CHARTS));
}

/* The name of the artist most heard in [from, to), or false */
static bool top_artist(long from, long to, int hour, char *buf, size_t size)
{
    struct tally *top;

    tally_log(BY_ARTIST, from, to, hour);
    if (tally_top(&top, 1, w_heard) == 0 ||
        !row_path(top->where, buf, size))
        return false;
    const char *name = path_name(buf, BY_ARTIST);
    memmove(buf, name, strlen(name) + 1);
    return true;
}

/* One month or one year, said. The year gets the Wrapped extras. */
static void say_recap(long from, long to, bool year)
{
    struct tm tm;
    int days = 0, firsts = 0, top = year ? 5 : 3;

    sum_log(from, to);
    if (sum.listens == 0)
        return;

    local_tm(from, &tm);
    if (year)
        say("== %d ==", tm.tm_year + 1900);
    else
        say("== %d-%02d ==", tm.tm_year + 1900, tm.tm_mon + 1);
    say_time("Listened", sum.secs[0] + sum.secs[1] + sum.secs[2]);
    say_time("Music", sum.secs[0]);
    if (sum.secs[1])
        say_time("Audiobooks", sum.secs[1]);
    if (sum.secs[2])
        say_time("Radio", sum.secs[2]);
    for (int i = 0; i < CAL_DAYS; i++)
        days += cal[i] != 0;
    say("Days listened: %d", days);
    say("Heard through: %ld%% of %ld", sum.heard * 100 / sum.listens,
        sum.listens);

    if (year)
    {
        static char was[MAX_PATH], is[MAX_PATH];
        int current, best;
        int peak = peak_hour();
        long mid = from + (MIN(to, now) - from) / 2;

        streaks(&current, &best);
        say("Longest streak: %d days", best);
        say("Most listening: around %02d:00", peak);
        /* taste: who led the first half of it, and who the second */
        if (top_artist(from, mid, -1, was, sizeof(was)) &&
            top_artist(mid, to, -1, is, sizeof(is)) && strcmp(was, is))
            say("From %s to %s", was, is);
    }

    /* first listens: tracks whose first row ever is in here */
    tally_log(BY_TRACK, 0, to, -1);
    for (int i = 0; i < TALLY_SIZE; i++)
        firsts += tally[i].key && (long)tally[i].first >= from;
    say("New to you: %d tracks", firsts);

    say_chart(BY_ARTIST, LANG_ID3_ARTIST, top, from, to);
    say_chart(BY_ALBUM, LANG_ID3_ALBUM, top, from, to);
    say_chart(BY_TRACK, LANG_ID3_TITLE, top, from, to);
}

/* This month, and the one before it underneath */
static int monthly_recap(void)
{
    clock_now();
    ntext = 0;
    long from = month_start(now);
    say_recap(from, LONG_MAX, false);
    say_recap(month_start(from - DAYSEC), from, false);
    return show(str(LANG_FORYOU_RECAP));
}

static int year_wrapped(void)
{
    clock_now();
    ntext = 0;
    long from = year_start(now);
    say_recap(from, LONG_MAX, true);
    say_recap(year_start(from - DAYSEC), from, true);
    return show(str(LANG_FORYOU_WRAPPED));
}

/* Streaks, then a row a half month, which is what fits across the screen:
 * one mark a day, heavier for more. */
static int calendar(void)
{
    static char grid[24][17];
    struct tm tm;
    int current, best;

    clock_now();
    ntext = 0;
    sum_log(0, LONG_MAX);
    if (sum.listens == 0)
        return nothing();

    streaks(&current, &best);
    say("Streak: %d days (best %d)", current, best);
    say("Weeks in a row: %d", weeks_running());

    memset(grid, 0, sizeof(grid));
    local_tm(now, &tm);
    int this_month = tm.tm_year * 12 + tm.tm_mon;
    for (int ago = 0; ago < 366; ago++)
    {
        local_tm(now - ago * DAYSEC, &tm);
        int back = this_month - (tm.tm_year * 12 + tm.tm_mon);
        if (back >= 12)
            break;
        int half = tm.tm_mday > 15;
        grid[back * 2 + !half][tm.tm_mday - (half ? 16 : 1)] =
            cal[ago] == 0 ? '.' : cal[ago] < 30 ? '-' :
            cal[ago] < 120 ? '+' : '#';
    }
    for (int row = 0; row < 24; row++)
    {
        int month = this_month - row / 2;
        /* the oldest month starts part-way through */
        for (int d = 0; d < 16 && !grid[row][d]; d++)
            grid[row][d] = ' ';
        if (!grid[row][strspn(grid[row], " ")])
            continue;           /* the half of this month still to come */
        say("%02d-%02d %s", month % 12 + 1, row % 2 ? 1 : 16, grid[row]);
    }
    return show(str(LANG_FORYOU_CALENDAR));
}

/* "You usually listen to X around 10 PM", then the day hour by hour */
static int time_of_day(void)
{
    static char artist[MAX_PATH];
    unsigned long hours[24];
    int peak;

    clock_now();
    ntext = 0;
    sum_log(0, LONG_MAX);
    if (sum.listens == 0)
        return nothing();
    peak = peak_hour();
    memcpy(hours, sum.hours, sizeof(hours));

    if (top_artist(0, LONG_MAX, peak, artist, sizeof(artist)))
        say("Around %02d:00: mostly %s", peak, artist);
    for (int h = 0; h < 24; h++)
    {
        char bar[13];
        int len = hours[h] * 12 / (hours[peak] ? hours[peak] : 1);

        memset(bar, '#', len);
        bar[len] = '\0';
        say("%02d %-12s %luh %02lum", h, bar, hours[h] / 3600,
            hours[h] / 60 % 60);
    }
    return show(str(LANG_FORYOU_HOURS));
}

/* How many tracks were heard for the first time this month and this year -
 * the discovery count - and this year's, newest first. */
static int first_listens(void)
{
    int month = 0, year = 0;

    clock_now();
    ntext = 0;
    tally_all_tracks();
    first_from = year_start(now);
    for (int i = 0; i < TALLY_SIZE; i++)
    {
        if (!tally[i].key)
            continue;
        month += (long)tally[i].first >= month_start(now);
        year += (long)tally[i].first >= first_from;
    }
    if (ntally == 0)
        return nothing();
    say("New this month: %d", month);
    say("New this year: %d", year);
    say_top(BY_TRACK, 50, w_first, PLAY_TRACK);
    return show(str(LANG_FORYOU_FIRST));
}

/* How much gets heard through, and what gets skipped the most */
static int skips(void)
{
    clock_now();
    ntext = 0;
    sum_log(0, LONG_MAX);
    if (sum.listens == 0)
        return nothing();
    say("Heard through: %ld%% (%ld of %ld)", sum.heard * 100 / sum.listens,
        sum.heard, sum.listens);
    tally_all_tracks();
    say_top(BY_TRACK, 30, w_skipped, PLAY_TRACK);
    return show(str(LANG_FORYOU_SKIPS));
}

/* --- the menu ---------------------------------------------------------- */

#define MIX(name, lang, func)                                               \
    MENUITEM_FUNCTION(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), func, NULL, \
                      Icon_Playlist)
#define MIX_W_PARAM(name, lang, func, param)                                \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang),     \
                              func, (void *)(intptr_t)(param), NULL,        \
                              Icon_Playlist)

MIX(foryou_item, LANG_FORYOU, foryou_mix);
MIX(daily_item, LANG_FORYOU_DAILY, daily_mix);
MIX(similar_item, LANG_FORYOU_SIMILAR, similar_mix);
MIX(rediscover_item, LANG_FORYOU_REDISCOVER, rediscover_mix);
MIX(forgotten_item, LANG_FORYOU_FORGOTTEN, forgotten_mix);
MIX(shuffle_item, LANG_FORYOU_SHUFFLE, smart_shuffle);
MIX(albums_item, LANG_FORYOU_ALBUMS, album_rediscovery);
MIX(years_ago_item, LANG_FORYOU_YEARS_AGO, years_ago);

MIX_W_PARAM(mood_chill, LANG_FORYOU_CHILL, mood_mix, 0);
MIX_W_PARAM(mood_energetic, LANG_FORYOU_ENERGETIC, mood_mix, 1);
MIX_W_PARAM(mood_driving, LANG_FORYOU_DRIVING, mood_mix, 2);
MIX_W_PARAM(mood_relaxing, LANG_FORYOU_RELAXING, mood_mix, 3);
MAKE_MENU(mood_menu, ID2P(LANG_FORYOU_MOOD), NULL, Icon_Playlist,
          &mood_chill, &mood_energetic, &mood_driving, &mood_relaxing);

MIX_W_PARAM(charts_week, LANG_FORYOU_WEEK, charts, THIS_WEEK);
MIX_W_PARAM(charts_month, LANG_FORYOU_MONTH, charts, THIS_MONTH);
MIX_W_PARAM(charts_year, LANG_FORYOU_YEAR, charts, THIS_YEAR);
MIX_W_PARAM(charts_all, LANG_FORYOU_ALL, charts, ALL_TIME);
MAKE_MENU(charts_menu, ID2P(LANG_FORYOU_CHARTS), NULL, Icon_Playlist,
          &charts_week, &charts_month, &charts_year, &charts_all);

MIX(recap_item, LANG_FORYOU_RECAP, monthly_recap);
MIX(wrapped_item, LANG_FORYOU_WRAPPED, year_wrapped);
MIX(calendar_item, LANG_FORYOU_CALENDAR, calendar);
MIX(hours_item, LANG_FORYOU_HOURS, time_of_day);
MIX(first_item, LANG_FORYOU_FIRST, first_listens);
MIX(skips_item, LANG_FORYOU_SKIPS, skips);

MENUITEM_SETTING(mix_size_item, &global_settings.foryou_mix_size, NULL);
MENUITEM_SETTING(age_item, &global_settings.foryou_age, NULL);

MAKE_MENU(foryou_menu, ID2P(LANG_FORYOU), NULL, Icon_Heart,
          &foryou_item, &daily_item, &similar_item, &rediscover_item,
          &forgotten_item, &shuffle_item, &mood_menu, &albums_item,
          &years_ago_item, &charts_menu, &recap_item, &wrapped_item,
          &calendar_item, &hours_item, &first_item, &skips_item,
          &mix_size_item, &age_item);

int foryou_screen(void)
{
    return do_menu(&foryou_menu, NULL, NULL, false) == GO_TO_WPS
           ? GO_TO_WPS : GO_TO_PREVIOUS;
}
