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
 * Pseudo-radio.
 *
 * A folder of long recordings - sets, mixes, concerts, field tapes - and a
 * way to drop into one part-way through, which is the whole of what makes a
 * radio feel like a radio. Tuning in is not "play this file": it picks a
 * recording at random and starts it at a random point, so what comes out of
 * the headphones is already underway and was not chosen by the listener.
 *
 * Stations are the subfolders of the radio folder. A folder with no
 * subfolders is one station, which is the case worth not making anybody
 * configure. There is no tuner and no station database: the folder tree is
 * the dial.
 *
 * The length floor is what keeps it honest. A three minute pop song entered
 * at 60% is a song with its first minute cut off; an hour-long set entered
 * at 60% is a radio. Files under the floor are passed over while a station
 * has anything longer, which is why tuning reads metadata for a handful of
 * candidates rather than scanning the folder up front - a radio folder can
 * be large and that scan would be paid on every tune.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "dir.h"
#include "file.h"
#include "lang.h"
#include "list.h"
#include "menu.h"
#include "metadata.h"
#include "misc.h"
#include "pathfuncs.h"
#include "playback.h"
#include "playlist.h"
#include "root_menu.h"
#include "screens.h"
#include "settings.h"
#include "splash.h"
#include "string-extra.h"
#include "strnatcmp.h"
#include "crc32.h"
#include "timefuncs.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "pradio.h"

#define PRADIO_MAX_STATIONS 48
/* How many random files a tune-in is willing to read metadata for before it
 * settles for the last one it looked at. Bounded because a station of
 * nothing but short files must still play something. */
#define PRADIO_TRIES        8
/* Never drop in this close to the end - the point is to land in the middle
 * of something, not on its last breath. */
#define PRADIO_TAIL_MS      (60 * 1000)
/* How long a pause has to be before coming back lands somewhere else. Below
 * this it is a phone call, not the afternoon. */
#define PRADIO_DRIFT_MIN_MS (30 * 1000)
/* How long a station stays on one recording before its schedule moves to
 * the next. Longer than most of the things a station is made of, so leaving
 * one for a few minutes and coming back finds the same recording still on
 * rather than a new one. */
#define PRADIO_ROTATE_SECS  (2 * 60 * 60)
/* A dynamic station is made of songs, not sets: its schedule moves on at
 * about the length of one. */
#define PRADIO_ROTATE_SONG  (4 * 60)
/* A dynamic station's playlist, next to its station.cfg. */
#define PRADIO_SETLIST      "setlist.m3u8"

/* --- the sounds -------------------------------------------------------- */

/* Every transition gets its own noise, because a radio you cannot hear
 * yourself operating is a file player. They are all the one generator in
 * beep.c with different knobs; what makes them distinct is the shape and
 * whether a tone glides through them.
 *
 * Kept quiet on purpose. These are punctuation between things worth
 * listening to, and a cue that competes with the programme is a cue nobody
 * wants twice. Strength scales all of them together - see the setting. */
enum pradio_sound
{
    SOUND_TUNE = 0,     /* arriving at a station                     */
    SOUND_RETUNE,       /* moving the dial to another one            */
    SOUND_PAUSE,        /* the carrier dropping out                  */
    SOUND_RESUME,       /* and coming back                           */
    SOUND_LEAVE,        /* switching off                             */
    PRADIO_SOUNDS
};

/* Any of them can be a file instead: a WAV of this name in
 * PRADIO_SOUND_DIR takes the place of the generated noise, and
 * band1.wav..band5.wav of the five below. Strength still scales it. */
#define PRADIO_SOUND_DIR ROCKBOX_DIR "/radio"
static const char * const sound_files[PRADIO_SOUNDS] =
    { "tune", "retune", "pause", "resume", "leave" };

static const struct beep_fx pradio_sounds[PRADIO_SOUNDS] =
{
    /* tune: hiss that swells and settles, with a carrier sliding into
     * place - the sound of landing on a station. */
    { 320, 2200, FX_SWELL,   0, 1400,  320,  0 },
    /* retune: shorter, harsher, the whistle sweeping the other way past
     * everything between here and there. */
    { 260, 2400, FX_FLAT,    0,  300, 2600, 12 },
    /* pause: the carrier falling away under it. */
    { 200, 1800, FX_FADEOUT, 2,    0,    0,  0 },
    /* resume: the same, rising. */
    { 200, 1800, FX_FADEIN,  2,    0,    0,  0 },
    /* leave: a low thump, the sound a set makes going off. */
    { 180, 2600, FX_FADEOUT, 4,    0,    0,  0 },
};

/* The rare ones. A radio that only ever makes a noise when you touch it is
 * a radio nobody believes; every hour or two something drifts past. Subtle
 * on purpose - these play *over* the programme, uninvited. */
static const struct beep_fx pradio_ambience[] =
{
    /* a station sliding past behind this one */
    { 900, 900,  FX_DIP,     0,  700, 1900,  0 },
    /* a moment of interference */
    { 420, 1100, FX_FLAT,    0,    0,    0, 45 },
    /* the signal fading out and back */
    { 1400, 800, FX_DIP,     1,    0,    0,  0 },
    /* a dial-up squeal from somewhere adjacent */
    { 600, 800,  FX_SWELL,   0, 2300,  900,  0 },
    /* distant weather on the band */
    { 1100, 700, FX_FLAT,    3,    0,    0, 10 },
};
#define PRADIO_AMBIENCE_N ARRAYLEN(pradio_ambience)

/* Roughly how often one of those turns up, in minutes, and how far either
 * side of it the actual gap lands. */
#define PRADIO_AMBIENCE_MINS   75
#define PRADIO_AMBIENCE_SPREAD 45

/* When the next uninvited noise is due, 0 while nothing is tuned in. */
static long ambience_tick;

static void ambience_schedule(void)
{
    int mins = PRADIO_AMBIENCE_MINS
             + (rand() % (2 * PRADIO_AMBIENCE_SPREAD + 1)) - PRADIO_AMBIENCE_SPREAD;
    ambience_tick = current_tick + (long)mins * 60 * HZ;
}

/* Called from the WPS loop, which is the only thing that keeps running
 * while a station plays and nobody touches anything. Cheap: a tick compare
 * until the hour or so is up. */
void pradio_ambience_tick(void)
{
    if (!global_settings.radio_ambience || !pradio_playing())
    {
        ambience_tick = 0;
        return;
    }

    if (ambience_tick == 0)
    {
        ambience_schedule();
        return;
    }

    if (TIME_BEFORE(current_tick, ambience_tick))
        return;

    int pick = rand() % PRADIO_AMBIENCE_N;
    struct beep_fx fx = pradio_ambience[pick];
    char path[MAX_PATH];
    snprintf(path, sizeof(path), PRADIO_SOUND_DIR "/band%d.wav", pick + 1);
    int ms = beep_play_wav(path, global_settings.radio_static_strength);
    if (ms > 0)
        beep_duck(ms, global_settings.sound_duck_cue / 2);

    fx.amplitude = fx.amplitude * global_settings.radio_static_strength / 100;
    if (ms == 0 && fx.amplitude > 0)
    {
        /* Ducked shallower than a cue: this one is meant to sound like it
         * is coming through the programme, not instead of it. */
        beep_duck(fx.duration, global_settings.sound_duck_cue / 2);
        beep_play_fx(&fx);
    }

    ambience_schedule();
}

/* Play one of the transition sounds, scaled by the user's strength. */
static void pradio_sound(enum pradio_sound which)
{
    if (!global_settings.radio_static || which >= PRADIO_SOUNDS)
        return;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), PRADIO_SOUND_DIR "/%s.wav", sound_files[which]);
    int ms = beep_play_wav(path, global_settings.radio_static_strength);
    if (ms > 0)
    {
        beep_duck(ms, global_settings.sound_duck_cue);
        return;
    }

    struct beep_fx fx = pradio_sounds[which];
    fx.amplitude = fx.amplitude * global_settings.radio_static_strength / 100;
    if (fx.amplitude <= 0)
        return;

    beep_duck(fx.duration, global_settings.sound_duck_cue);
    beep_play_fx(&fx);
}

/* Switching the set off. Called on the way out of a station, before
 * playback is torn down, because after it there is nothing to tell. */
void pradio_leaving(void)
{
    if (!pradio_playing())
        return;

    ambience_tick = 0;
    pradio_sound(SOUND_LEAVE);
}

static char stations[PRADIO_MAX_STATIONS][MAX_FILENAME + 1];
static int  nstations;

/* --- the folder ------------------------------------------------------- */

static bool under_radio(const char *path)
{
    const char *root = global_settings.radio_folder;
    size_t len = strlen(root);

    if (!path || len == 0 || (len == 1 && root[0] == '/'))
        return false;
    while (len > 1 && root[len - 1] == '/')
        len--;
    if (strncasecmp(path, root, len))
        return false;
    return path[len] == '/' || path[len] == '\0';
}

/* The playing playlist when it is a dynamic station's setlist, else NULL.
 * A setlist is a playlist file inside a station folder. */
static const char *setlist_playing(void)
{
    const char *pl = playlist_get_current()->filename;
    size_t n = strlen(pl), s = sizeof(PRADIO_SETLIST) - 1;

    if (n > s && pl[n - s - 1] == '/' && !strcasecmp(pl + n - s, PRADIO_SETLIST)
        && under_radio(pl))
        return pl;
    return NULL;
}

/* A dynamic station plays tracks from the library, which are nowhere near
 * the radio folder: what makes one of them a station track is that the
 * playlist it plays from is a station's setlist. So the setlist's path
 * stands in for the track's wherever a station is worked out from a path.
 * ponytail: any path counts while a setlist is on air - a thumbnail with
 * no art of its own shows the station's cover meanwhile; checking the path
 * is in the setlist would mean reading the setlist on every call. */
static const char *station_path(const char *path)
{
    if (!path)
        return NULL;
    if (under_radio(path))
        return path;
    return audio_status() ? setlist_playing() : NULL;
}

bool pradio_is_station_track(const char *path)
{
    return station_path(path) != NULL;
}

bool pradio_playing(void)
{
    if (audio_status() == 0)
        return false;
    struct mp3entry *id3 = audio_current_track();
    return id3 && pradio_is_station_track(id3->path);
}

static int cmp_names(const void *a, const void *b)
{
    return strnatcasecmp(a, b);
}

/* The subfolders of the radio folder, sorted. None means the folder is
 * itself the only station. */
static void scan_stations(void)
{
    nstations = 0;

    DIR *d = opendir(global_settings.radio_folder);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d)) && nstations < PRADIO_MAX_STATIONS)
    {
        if (e->d_name[0] == '.')
            continue;
        struct dirinfo info = dir_get_info(d, e);
        if (!(info.attribute & ATTR_DIRECTORY))
            continue;
        strmemccpy(stations[nstations], e->d_name, sizeof stations[0]);
        nstations++;
    }
    closedir(d);

    qsort(stations, nstations, sizeof stations[0], cmp_names);
}

static void station_dir(int station, char *buf, size_t size)
{
    if (station < 0 || station >= nstations)
        strmemccpy(buf, global_settings.radio_folder, size);
    else
        path_append(buf, global_settings.radio_folder, stations[station], size);
}

/* --- dynamic stations -------------------------------------------------- */

/* A static station is a folder of recordings. A dynamic one is a folder
 * with a station.cfg in it, and its music is whatever in the library
 * matches the file - a decade, a handful of artists, a genre:
 *
 *     year: 1976-1980
 *     artist: Blink-182, Green Day, Nirvana
 *     genre: Punk, Grunge
 *
 * Every line narrows it (an AND); the values on a line are alternatives
 * (an OR); a year value is a year or a range. The file decides which kind
 * a station is, so one is never both: a folder with a station.cfg plays
 * its setlist and nothing else.
 *
 * The setlist is setlist.m3u8 next to the station.cfg, built from the
 * database by a background thread so that nothing waits for it; the first
 * tune-in of a station that has none yet builds it on the spot. A setlist
 * is only rebuilt when asked (Rebuild Setlists in the radio settings, or a
 * long press on a station in the station list), so a station stays the
 * same station between database updates.
 *
 * ponytail: the station.cfg is the whole interface. Stations the player
 * makes for itself - from the listening log of #28, say - would be a
 * writer of station.cfg files and nothing new here. */
#define PRADIO_STATION_CFG "station.cfg"
#define DYN_MAX_YEARS 8

struct dyn_params
{
    int nyears;
    int year_lo[DYN_MAX_YEARS], year_hi[DYN_MAX_YEARS];
    char artists[256];      /* comma separated, "" = any */
    char genres[128];
};

static bool station_is_dynamic(const char *dir)
{
    char path[MAX_PATH];
    path_append(path, dir, PRADIO_STATION_CFG, sizeof(path));
    return file_exists(path);
}

static bool read_params(const char *dir, struct dyn_params *p)
{
    char path[MAX_PATH], line[256];
    int fd;

    memset(p, 0, sizeof(*p));
    path_append(path, dir, PRADIO_STATION_CFG, sizeof(path));
    fd = open_utf8(path, O_RDONLY);
    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof(line)) > 0)
    {
        char *name, *value;
        if (line[0] == '#' || !settings_parseline(line, &name, &value))
            continue;
        if (!strcasecmp(name, "year"))
        {
            char *tok, *save;
            for (tok = strtok_r(value, ",", &save);
                 tok && p->nyears < DYN_MAX_YEARS;
                 tok = strtok_r(NULL, ",", &save))
            {
                char *dash = strchr(tok, '-');
                p->year_lo[p->nyears] = atoi(tok);
                p->year_hi[p->nyears] = dash ? atoi(dash + 1)
                                             : p->year_lo[p->nyears];
                p->nyears++;
            }
        }
        else if (!strcasecmp(name, "artist"))
            strmemccpy(p->artists, value, sizeof(p->artists));
        else if (!strcasecmp(name, "genre"))
            strmemccpy(p->genres, value, sizeof(p->genres));
    }
    close(fd);
    return true;
}

/* Whether value is one of a comma separated list, ignoring case and the
 * spaces around the commas. */
static bool in_list(const char *list, const char *value)
{
    size_t vlen = strlen(value);

    while (*list)
    {
        while (*list == ' ' || *list == ',')
            list++;
        const char *end = strchr(list, ',');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        while (len > 0 && list[len - 1] == ' ')
            len--;
        if (len == vlen && len && !strncasecmp(list, value, len))
            return true;
        if (!end)
            break;
        list = end + 1;
    }
    return false;
}

/* One build at a time: the thread and a tune-in both come through here. */
static struct mutex setlist_mutex;

/* Write the station's setlist from the database. Returns the number of
 * tracks, or -1 when the database is not there to ask. */
static int build_setlist(const char *dir)
{
    static struct tagcache_search tcs;
    static struct dyn_params p;
    static char fname[MAX_PATH], val[128];
    char path[MAX_PATH], tmp[MAX_PATH + 4];
    int fd, count = 0;

    mutex_lock(&setlist_mutex);
    if (!read_params(dir, &p) || !tagcache_is_usable())
    {
        mutex_unlock(&setlist_mutex);
        return -1;
    }

    path_append(path, dir, PRADIO_SETLIST, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = creat(tmp, 0666);
    if (fd < 0 || !tagcache_search(&tcs, tag_filename))
    {
        if (fd >= 0)
            close(fd);
        mutex_unlock(&setlist_mutex);
        return -1;
    }

    while (tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        if (p.nyears)
        {
            long year = tagcache_get_numeric(&tcs, tag_year);
            int i;
            for (i = 0; i < p.nyears; i++)
                if (year >= p.year_lo[i] && year <= p.year_hi[i])
                    break;
            if (i == p.nyears)
                continue;
        }
        if (p.artists[0] &&
            !(tagcache_retrieve(&tcs, tcs.idx_id, tag_artist, val, sizeof(val))
              && in_list(p.artists, val)) &&
            !(tagcache_retrieve(&tcs, tcs.idx_id, tag_albumartist, val,
                                sizeof(val)) && in_list(p.artists, val)))
            continue;
        if (p.genres[0] &&
            !(tagcache_retrieve(&tcs, tcs.idx_id, tag_genre, val, sizeof(val))
              && in_list(p.genres, val)))
            continue;

        fdprintf(fd, "%s\n", fname);
        count++;
        yield();
    }
    tagcache_search_finish(&tcs);
    close(fd);
    rename(tmp, path);
    mutex_unlock(&setlist_mutex);
    return count;
}

/* The thread that keeps setlists from costing anybody a wait. */
enum { SETLIST_MISSING, SETLIST_ALL };
static struct event_queue setlist_queue SHAREDBSS_ATTR;
static long setlist_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];

static void setlist_thread(void)
{
    struct queue_event ev;
    static char dir[MAX_PATH], path[MAX_PATH];

    while (1)
    {
        queue_wait(&setlist_queue, &ev);

        /* Its own walk of the radio folder: the station list belongs to the
         * UI thread. */
        DIR *d = opendir(global_settings.radio_folder);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (e->d_name[0] == '.' ||
                !(dir_get_info(d, e).attribute & ATTR_DIRECTORY))
                continue;
            path_append(dir, global_settings.radio_folder, e->d_name,
                        sizeof(dir));
            if (!station_is_dynamic(dir))
                continue;
            path_append(path, dir, PRADIO_SETLIST, sizeof(path));
            if (ev.id == SETLIST_ALL || !file_exists(path))
                build_setlist(dir);
        }
        closedir(d);
    }
}

/* Start the thread the first time, and hand it a job: SETLIST_MISSING,
 * SETLIST_ALL, or -1 for none. */
static void setlists_request(int what)
{
    static bool started;
    if (!started)
    {
        mutex_init(&setlist_mutex);
        queue_init(&setlist_queue, false);
        create_thread(setlist_thread, setlist_stack, sizeof(setlist_stack),
                      0, "radio setlists"
                      IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
        started = true;
    }
    if (what >= 0)
        queue_post(&setlist_queue, what, 0);
}

/* Whether the station on air is a dynamic one: what %rd says. */
bool pradio_dynamic(void)
{
    return pradio_playing() && setlist_playing() != NULL;
}

/* --- the station clock ------------------------------------------------- */

/* A station is a transmitter, not a file: it is playing whether or not
 * anybody is listening. Where it has got to is a function of the clock and
 * of nothing else, so leaving one - for a pause, for another station, for a
 * week with the device switched off - and coming back finds it exactly as
 * far on as the time that went by. Nothing is stored, so there is nothing
 * to fall out of step.
 *
 * The station's own name is the phase, so no two stations are ever playing
 * the same second of the same thing, and the order its files go out in is
 * that same number rather than the tick - a schedule, stable but not
 * alphabetical, which is what a station has and a folder does not.
 *
 * ponytail: one recording at a time rather than a continuous tape across
 * the station's whole library - a real tape would have to read metadata for
 * every file on the station before it could say where "now" is. */
static uint32_t pradio_now(void)
{
#if CONFIG_RTC
    return (uint32_t)mktime(get_time());
#else
    return current_tick / HZ;
#endif
}

static unsigned int station_seed(int station)
{
    const char *name = (station >= 0 && station < nstations)
                     ? stations[station] : "";
    return crc_32(name, strlen(name), 0xffffffff);
}

/* Where in a recording of this length the station has got to. Never its
 * last minute - the point is to land in the middle of something, not on its
 * last breath - and a second of clock is a second of programme, which is
 * the whole of what makes leaving and coming back work. */
static unsigned long clock_offset(uint32_t now, unsigned int seed,
                                  unsigned long length)
{
    if (length <= PRADIO_TAIL_MS)
        return 0;

    unsigned long span = (length - PRADIO_TAIL_MS) / 1000;
    if (span == 0)
        return 0;
    return (unsigned long)((now + seed) % span) * 1000UL;
}

/* --- tuning in -------------------------------------------------------- */

static struct mp3entry tune_id3;

/* Rockbox seeds rand() with a constant, so without this every boot makes
 * the same "any station" choice. Once per run is enough - the station clock
 * does not use rand() at all. */
static void seed_rand(void)
{
    static bool done;
    if (!done)
    {
        srand(current_tick);
        done = true;
    }
}

static bool long_enough(unsigned long length)
{
    int mins = global_settings.radio_min_length;
    return mins <= 0 || length >= (unsigned long)mins * 60 * 1000UL;
}

/* Fill the playlist with a station's audio files, at any depth under it -
 * a station is one folder to point at, not one flat folder to populate by
 * hand. Returns how many tracks. */
static int build_playlist(const char *dir)
{
    if (playlist_create(dir, NULL) < 0)
        return 0;

    /* Silently: "Inserted 2 tracks (OFF to abort)" over the top of a
     * tune-in is the machinery of a file player showing through the one
     * screen whose whole job is not to look like one. */
    if (playlist_insert_directory_ex(NULL, dir, PLAYLIST_INSERT_LAST,
                                     false, true, false) < 0)
        return 0;

    return playlist_amount_ex(NULL);
}

/* Which of the station's recordings is on air now. The clock picks it, so
 * the same station found twice in one afternoon is where it should be
 * rather than somewhere new. Walks forward from there for one over the
 * length floor, because a station of mostly short files must still play
 * something. */
static int pick_track(int n, unsigned int seed, uint32_t now, uint32_t rotate,
                      unsigned long *length)
{
    static struct playlist_track_info info;

    int pick = (int)((now / rotate + seed) % (unsigned)n);
    int found = -1;

    /* The length handed back must be the length of the track handed back.
     * Settling for the scheduled track after none cleared the floor used
     * to return it with the length of the last one looked at, so the clock
     * seeked a short file by a long one's offset - off its end and into
     * the start of the next, which is every tune-in landing on a track's
     * first second. */
    *length = 0;
    for (int i = 0; i < PRADIO_TRIES && i < n; i++)
    {
        int idx = (pick + i) % n;

        if (playlist_get_track_info(NULL, idx, &info) < 0)
            continue;
        if (!get_metadata(&tune_id3, -1, info.filename))
            continue;

        if (found < 0 || long_enough(tune_id3.length))
        {
            found = idx;
            *length = tune_id3.length;
        }
        if (long_enough(tune_id3.length))
            break;
    }
    return found < 0 ? pick : found;
}

/* The station tuned last, by name, so the dial comes back where it was
 * across a reboot and a rescan that renumbered the folders. -1 when there
 * is nothing remembered or the folder has gone. */
static int last_station(void)
{
    if (global_settings.radio_last[0] == '\0')
        return -1;

    for (int i = 0; i < nstations; i++)
        if (!strcasecmp(stations[i], global_settings.radio_last))
            return i;
    return -1;
}

/* Set for the length of one tune() that came from the user moving the dial
 * rather than from arriving at the radio. */
static bool retune_sound;
/* Set for a tune() that only puts the station back where its clock says it
 * is: nothing on the dial moved, so nothing is heard doing it. */
static bool tune_quiet;

static bool tune(int station)
{
    /* a dialog; before anything is torn down */
    if (!warn_on_pl_erase())
        return false;

    char dir[MAX_PATH];
    station_dir(station, dir, sizeof dir);

    bool dynamic = station_is_dynamic(dir);
    int n;
    if (dynamic)
    {
        /* No setlist yet: this is the one wait, built on the spot. */
        char setlist[MAX_PATH];
        path_append(setlist, dir, PRADIO_SETLIST, sizeof(setlist));
        if (!file_exists(setlist))
        {
            splash(0, ID2P(LANG_RADIO_BUILDING));
            setlists_request(-1);       /* the mutex */
            build_setlist(dir);
        }
        n = playlist_create(dir, PRADIO_SETLIST) < 0 ? 0 :
            playlist_amount_ex(NULL);
    }
    else
        n = build_playlist(dir);
    if (n <= 0)
    {
        splashf(HZ, "%s", str(LANG_RADIO_EMPTY));
        return false;
    }

    /* Shuffled on the station's own number rather than on the tick:
     * otherwise a station with several files always opens the same way and
     * plays them in the same order - a jukebox with extra steps - but a
     * fresh order on every tune-in would mean the schedule the clock
     * indexes into was a different one each time. */
    unsigned int seed = station_seed(station);
    uint32_t now = pradio_now();

    if (n > 1)
        playlist_shuffle(seed, -1);

    unsigned long length = 0;
    int track = pick_track(n, seed, now,
                           dynamic ? PRADIO_ROTATE_SONG : PRADIO_ROTATE_SECS,
                           &length);

    /* The static plays over the gap between the last screen and the first
     * sample, which is the gap it exists to cover. Tuning in while
     * something else is still playing is the ordinary case, so it ducks
     * the music the way the other device sounds do - a cue, because
     * missing it is missing the only thing that says the dial moved. */
    if (global_settings.radio_static && !tune_quiet)
    {
        /* Retuning by hand and arriving from somewhere else are different
         * gestures and sound different; retune_sound says which this is. */
        pradio_sound(retune_sound ? SOUND_RETUNE : SOUND_TUNE);
    }

    strmemccpy(global_settings.radio_last,
               (station >= 0 && station < nstations) ? stations[station] : "",
               sizeof global_settings.radio_last);

    playlist_start(track, clock_offset(now, seed, length), 0);
    return true;
}

/* --- skipping while tuned in ------------------------------------------- */

/* Which station a radio track's path belongs to, or -1 if the radio folder
 * itself is the only station. */
static int station_of(const char *path)
{
    path = station_path(path);
    if (!path)
        return -1;

    const char *root = global_settings.radio_folder;
    size_t rootlen = strlen(root);
    while (rootlen > 1 && root[rootlen - 1] == '/')
        rootlen--;

    const char *rel = path + rootlen;
    while (*rel == '/')
        rel++;

    for (int i = 0; i < nstations; i++)
    {
        size_t len = strlen(stations[i]);
        if (!strncasecmp(rel, stations[i], len) &&
            (rel[len] == '/' || rel[len] == '\0'))
            return i;
    }
    return -1;
}

/* User-initiated prev/next while tuned in: a radio dial moves to another
 * station, it does not step to the next track of the one already playing.
 *
 * The dial turns one station along, in the order the station list shows
 * them, and wraps. It used to jump to a random one, and rand() is reseeded
 * by every tune-in's shuffle with that station's own fixed number - so
 * from any one station the "random" next was nearly always the same one,
 * and some stations could hardly be reached at all. */
bool pradio_skip(int dir)
{
    if (!pradio_playing())
        return false;

    scan_stations();

    retune_sound = true;

    bool ok;
    if (nstations <= 1)
    {
        ok = tune(nstations == 1 ? 0 : -1);
    }
    else
    {
        struct mp3entry *id3 = audio_current_track();
        int current = id3 ? station_of(id3->path) : -1;

        if (current < 0)
            current = dir > 0 ? -1 : 0;
        ok = tune((current + (dir > 0 ? 1 : -1) + nstations) % nstations);
    }

    retune_sound = false;
    return ok;
}

/* --- the station folder ------------------------------------------------ */

/* The station folder a radio track sits under, with its trailing slash, at
 * whatever depth the file itself is. String work only - the folder is the
 * first component under the radio folder by definition, so there is nothing
 * to go to disk for. */
bool pradio_station_dir(const char *path, char *buf, size_t size)
{
    path = station_path(path);
    if (!path)
        return false;

    const char *root = global_settings.radio_folder;
    size_t rootlen = strlen(root);
    while (rootlen > 1 && root[rootlen - 1] == '/')
        rootlen--;

    const char *rel = path + rootlen;
    while (*rel == '/')
        rel++;

    const char *slash = strchr(rel, '/');
    if (!slash)
        return false; /* the file sits in the radio folder itself */

    size_t len = slash - path;
    if (len + 2 > size)
        return false;

    memcpy(buf, path, len);
    buf[len] = '/';
    buf[len + 1] = '\0';
    return true;
}

/* Just the station's name, for a skin to put where the album would go. */
bool pradio_station_name(const char *path, char *buf, size_t size)
{
    char dir[MAX_PATH];

    if (!pradio_station_dir(path, dir, sizeof dir))
        return false;

    size_t len = strlen(dir);
    if (len > 1)
        dir[len - 1] = '\0';        /* the trailing slash */

    const char *name = strrchr(dir, '/');
    strmemccpy(buf, name ? name + 1 : dir, size);
    return true;
}

/* --- time passing while paused ----------------------------------------- */

static long pause_tick;

/* Put the station a track belongs to back where its clock says it is. The
 * clock may have moved it on to another recording altogether, so this is a
 * tune-in, not a seek within the file that happens to be loaded. */
static bool resync(const char *path)
{
    scan_stations();
    tune_quiet = true;
    bool ok = tune(station_of(path));
    tune_quiet = false;
    return ok;
}

/* A station does not wait for you. Come back after a while and it has moved
 * on by as much as you were away, which is the whole difference between
 * tuning in and pressing play. A pause across a power cycle is the resume
 * below, not this. */
void pradio_pause(bool paused)
{
    if (paused)
    {
        if (!pradio_playing())
        {
            pause_tick = 0;
            return;
        }
        pause_tick = current_tick;
        pradio_sound(SOUND_PAUSE);
        return;
    }

    if (pause_tick == 0 || !pradio_playing())
        return;

    pradio_sound(SOUND_RESUME);

    unsigned long away = (unsigned long)(current_tick - pause_tick) * 1000 / HZ;
    pause_tick = 0;
    if (away < PRADIO_DRIFT_MIN_MS)
        return;

    struct mp3entry *id3 = audio_current_track();
    if (id3)
        resync(id3->path);
}

/* Resuming playback - at power-up, onto the WPS - with a station last on.
 * The station kept going while the device was off, so it is tuned in again
 * rather than resumed at the second it was left on. */
bool pradio_resume(int index)
{
    static struct playlist_track_info info;

    if (playlist_get_track_info(NULL, index, &info) < 0)
        return false;

    /* A dynamic station's tracks are in the library: it is the setlist
     * being resumed that says this is the radio. */
    const char *setlist = setlist_playing();
    if (setlist)
        return resync(setlist);
    if (!pradio_is_station_track(info.filename))
        return false;
    return resync(info.filename);
}

/* --- settings --------------------------------------------------------- */

#define MAX_SKIN_NAMES 64
static char skin_names[MAX_SKIN_NAMES][MAX_FILENAME + 1];
static int nskins;

static const char *skin_name_cb(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return item == 0 ? (const char *)str(LANG_AB_SAME_AS_MUSIC)
                     : skin_names[item - 1];
}

/* The same chooser the audiobook WPS uses, with the extension as an
 * argument so the screensaver can share it. */
static int choose_skin(const char *ext, int title, char *dest, size_t dest_sz)
{
    size_t extlen = strlen(ext);

    nskins = 0;
    DIR *d = opendir(WPS_DIR);
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)) && nskins < MAX_SKIN_NAMES)
        {
            size_t len = strlen(e->d_name);
            if (len > extlen && len - extlen <= MAX_FILENAME &&
                !strcasecmp(e->d_name + len - extlen, ext))
            {
                memcpy(skin_names[nskins], e->d_name, len - extlen);
                skin_names[nskins][len - extlen] = '\0';
                nskins++;
            }
        }
        closedir(d);
    }
    qsort(skin_names, nskins, sizeof skin_names[0], cmp_names);

    struct simplelist_info info;
    simplelist_info_init(&info, str(title), nskins + 1, NULL);
    info.get_name = skin_name_cb;
    info.title_icon = Icon_Wps;
    info.selection = 0;
    for (int i = 0; i < nskins; i++)
        if (!strcmp(skin_names[i], dest))
            info.selection = i + 1;
    simplelist_show_list(&info);
    if (info.selection < 0)
        return 0;

    if (info.selection == 0)
        dest[0] = '\0';
    else
        strmemccpy(dest, skin_names[info.selection - 1], dest_sz);
    settings_save();
    return 0;
}

static int choose_wps(void)
{
    return choose_skin(".wps", LANG_RADIO_WPS, global_settings.radio_wps,
                       sizeof global_settings.radio_wps);
}

#ifdef HAVE_VIDEO
static int choose_saver(void)
{
    return choose_skin(".ss", LANG_RADIO_SAVER, global_settings.radio_saver,
                       sizeof global_settings.radio_saver);
}
#endif

static int show_folder(void)
{
    const char *f = global_settings.radio_folder;
    splashf(HZ * 3, "%s: %s\n%s", str(LANG_RADIO_FOLDER), f[0] ? f : "-",
            str(LANG_AB_FOLDER_HINT));
    return 0;
}

static int rebuild_setlists(void)
{
    setlists_request(SETLIST_ALL);
    splash(HZ * 2, ID2P(LANG_RADIO_REBUILDING));
    return 0;
}

MENUITEM_FUNCTION(pradio_folder_item, 0, ID2P(LANG_RADIO_FOLDER),
                  show_folder, NULL, Icon_Folder);
MENUITEM_FUNCTION(pradio_wps_item, 0, ID2P(LANG_RADIO_WPS),
                  choose_wps, NULL, Icon_Wps);
#ifdef HAVE_VIDEO
MENUITEM_FUNCTION(pradio_saver_item, 0, ID2P(LANG_RADIO_SAVER),
                  choose_saver, NULL, Icon_Wps);
#endif
static int pradio_stations(void);
MENUITEM_FUNCTION(pradio_stations_item, 0, ID2P(LANG_RADIO_STATIONS),
                  pradio_stations, NULL, Icon_Radio_screen);
MENUITEM_SETTING(pradio_min_length, &global_settings.radio_min_length, NULL);
MENUITEM_SETTING(pradio_static, &global_settings.radio_static, NULL);
MENUITEM_SETTING(pradio_static_strength,
                 &global_settings.radio_static_strength, NULL);
MENUITEM_SETTING(pradio_ambience_item, &global_settings.radio_ambience, NULL);
MENUITEM_FUNCTION(pradio_rebuild_item, 0, ID2P(LANG_RADIO_REBUILD),
                  rebuild_setlists, NULL, Icon_Playlist);

MAKE_MENU(pradio_settings_menu, ID2P(LANG_RADIO_SETTINGS), NULL, Icon_Radio_screen,
          &pradio_stations_item,
          &pradio_rebuild_item,
          &pradio_folder_item,
          &pradio_min_length,
          &pradio_static,
          &pradio_static_strength,
          &pradio_ambience_item,
          &pradio_wps_item
#ifdef HAVE_VIDEO
          , &pradio_saver_item
#endif
          );

/* --- the screen ------------------------------------------------------- */

/* Row 0 tunes across every station at once, which is what someone who has
 * not decided what to listen to actually wants. Everything after it is a
 * station. */
#define ROW_ANY       0
#define ROW_FIRST     1

static const char *row_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;

    if (item == ROW_ANY)
        return (const char *)str(LANG_RADIO_ANY);
    return stations[item - ROW_FIRST];
}

/* A long press on a dynamic station rebuilds its setlist, then and there:
 * the way to have one station catch up with the database without waiting
 * on all the others. */
static int station_action(int action, struct gui_synclist *list)
{
    int item = gui_synclist_get_sel_pos(list) - ROW_FIRST;
    char dir[MAX_PATH];

    if (action != ACTION_STD_CONTEXT || item < 0 || item >= nstations)
        return action;
    station_dir(item, dir, sizeof(dir));
    if (!station_is_dynamic(dir))
        return action;

    splash(0, ID2P(LANG_RADIO_BUILDING));
    setlists_request(-1);                       /* the mutex */
    int n = build_setlist(dir);
    splashf(HZ, "%s: %d", stations[item], n < 0 ? 0 : n);
    return ACTION_REDRAW;
}

/* Picking a station by name. Off the main path on purpose - a radio hands
 * you sound, not a menu - and reached from the radio settings, which is
 * where the rest of the deliberate choices already are. */
static int pradio_stations(void)
{
    int selection = ROW_ANY;

    while (1)
    {
        scan_stations();
        seed_rand();
        setlists_request(SETLIST_MISSING);

        struct simplelist_info info;
        simplelist_info_init(&info, str(LANG_RADIO_STATIONS),
                             ROW_FIRST + nstations, NULL);
        info.get_name = row_name;
        info.title_icon = Icon_Radio_screen;
        info.selection = selection;
        info.action_callback = station_action;
        simplelist_show_list(&info);

        if (info.selection < 0)
            return 0;
        selection = info.selection;

        /* "Any station" is a station picked for you, not a mode: the
         * choosing happens once, here, and the rest of the tune-in is the
         * same as if it had been picked by hand. */
        int station = (selection == ROW_ANY)
            ? (nstations > 0 ? rand() % nstations : -1)
            : selection - ROW_FIRST;

        if (tune(station))
            return 0;
    }
}

int pradio_screen(void)
{
    scan_stations();
    seed_rand();
    setlists_request(SETLIST_MISSING);

    /* A radio hands you sound, not a menu. The entry on the main menu is
     * the dial going back to where it was left - or, with nothing to go
     * back to, to a station picked for you. Nothing to choose, nothing to
     * confirm: the sound is the answer.
     *
     * Walking in on the station that is already playing is the one case
     * that does nothing at all. Re-tuning it would be the entry throwing
     * away the thing the user came back to look at. */
    int station = last_station();
    if (station < 0 && nstations > 0)
        station = rand() % nstations;

    if (pradio_playing())
    {
        struct mp3entry *id3 = audio_current_track();
        if (id3 && station_of(id3->path) == station)
            return GO_TO_WPS;
    }

    if (tune(station))
        return GO_TO_WPS;

    return GO_TO_PREVIOUS;
}
