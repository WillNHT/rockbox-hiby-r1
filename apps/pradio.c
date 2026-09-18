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

    struct beep_fx fx = pradio_ambience[rand() % PRADIO_AMBIENCE_N];
    fx.amplitude = fx.amplitude * global_settings.radio_static_strength / 100;
    if (fx.amplitude > 0)
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

bool pradio_is_station_track(const char *path)
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

/* --- tuning in -------------------------------------------------------- */

static struct mp3entry tune_id3;

/* Where in a recording of this length to drop in. Uniform over all of it
 * but the last minute: a radio has no reason to prefer the beginning, and
 * the beginning is the one part the listener could have had by pressing
 * play. */
static unsigned long random_offset(unsigned long length)
{
    if (length <= PRADIO_TAIL_MS)
        return 0;
    return (unsigned long)(rand() % (int)((length - PRADIO_TAIL_MS) / 1000))
           * 1000UL;
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

    if (playlist_insert_directory(NULL, dir, PLAYLIST_INSERT_LAST,
                                  false, true) < 0)
        return 0;

    return playlist_amount_ex(NULL);
}

/* Pick a track, preferring one over the length floor. Returns its index and
 * writes its length; the length is 0 when no metadata could be read. */
static int pick_track(int n, unsigned long *length)
{
    static struct playlist_track_info info;

    /* Rockbox seeds rand() with a constant, so without this every boot
     * tunes to the same track at the same second - which is the one thing
     * a radio must not do. Same treatment the shuffle gets. */
    srand(current_tick);

    int pick = rand() % n;

    *length = 0;
    for (int i = 0; i < PRADIO_TRIES && i < n; i++)
    {
        int idx = (i == 0) ? pick : rand() % n;

        if (playlist_get_track_info(NULL, idx, &info) < 0)
            continue;
        if (!get_metadata(&tune_id3, -1, info.filename))
            continue;

        pick = idx;
        *length = tune_id3.length;
        if (long_enough(tune_id3.length))
            break;
    }
    return pick;
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

static bool tune(int station)
{
    /* a dialog; before anything is torn down */
    if (!warn_on_pl_erase())
        return false;

    char dir[MAX_PATH];
    station_dir(station, dir, sizeof dir);

    int n = build_playlist(dir);
    if (n <= 0)
    {
        splashf(HZ, "%s", str(LANG_RADIO_EMPTY));
        return false;
    }

    /* Otherwise a station with several files always opens the same way and
     * plays them in the same order - a jukebox with extra steps. */
    if (n > 1)
        playlist_shuffle(current_tick, -1);

    unsigned long length = 0;
    int track = pick_track(n, &length);

    /* The static plays over the gap between the last screen and the first
     * sample, which is the gap it exists to cover. Tuning in while
     * something else is still playing is the ordinary case, so it ducks
     * the music the way the other device sounds do - a cue, because
     * missing it is missing the only thing that says the dial moved. */
    if (global_settings.radio_static)
    {
        /* Retuning by hand and arriving from somewhere else are different
         * gestures and sound different; retune_sound says which this is. */
        pradio_sound(retune_sound ? SOUND_RETUNE : SOUND_TUNE);
        sleep(HZ * pradio_sounds[retune_sound ? SOUND_RETUNE : SOUND_TUNE]
                   .duration / 1000);
    }

    strmemccpy(global_settings.radio_last,
               (station >= 0 && station < nstations) ? stations[station] : "",
               sizeof global_settings.radio_last);

    playlist_start(track, random_offset(length), 0);
    return true;
}

/* --- skipping while tuned in ------------------------------------------- */

/* Which station a radio track's path belongs to, or -1 if the radio folder
 * itself is the only station. */
static int station_of(const char *path)
{
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
 * station, it does not step to the next track of the one already playing. */
bool pradio_skip(void)
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

        int station = rand() % nstations;
        if (current >= 0 && station == current)
            station = (station + 1) % nstations;

        ok = tune(station);
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
    if (!pradio_is_station_track(path))
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

/* A station does not wait for you. Come back after a while and it has moved
 * on by as much as you were away, which is the whole difference between
 * tuning in and pressing play.
 * ponytail: current_tick, so a pause across a power cycle is not counted -
 * needs the RTC if "resume tomorrow" has to drift too. */
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
    if (!id3 || id3->length == 0)
        return;

    audio_pre_ff_rewind();
    audio_ff_rewind((id3->elapsed + away) % id3->length);
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

MENUITEM_FUNCTION(pradio_folder_item, 0, ID2P(LANG_RADIO_FOLDER),
                  show_folder, NULL, Icon_Folder);
MENUITEM_FUNCTION(pradio_wps_item, 0, ID2P(LANG_RADIO_WPS),
                  choose_wps, NULL, Icon_Wps);
#ifdef HAVE_VIDEO
MENUITEM_FUNCTION(pradio_saver_item, 0, ID2P(LANG_RADIO_SAVER),
                  choose_saver, NULL, Icon_Wps);
#endif
MENUITEM_SETTING(pradio_min_length, &global_settings.radio_min_length, NULL);
MENUITEM_SETTING(pradio_static, &global_settings.radio_static, NULL);
MENUITEM_SETTING(pradio_static_strength,
                 &global_settings.radio_static_strength, NULL);
MENUITEM_SETTING(pradio_ambience_item, &global_settings.radio_ambience, NULL);

MAKE_MENU(pradio_settings_menu, ID2P(LANG_RADIO_SETTINGS), NULL, Icon_Tuner,
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
 * not decided what to listen to actually wants; the last row is the
 * settings. Everything between is a station. */
#define ROW_ANY       0
#define ROW_FIRST     1
#define ROW_SETTINGS  (ROW_FIRST + nstations)

static const char *row_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;

    if (item == ROW_ANY)
        return (const char *)str(LANG_RADIO_ANY);
    if (item == ROW_SETTINGS)
        return (const char *)str(LANG_RADIO_SETTINGS);
    return stations[item - ROW_FIRST];
}

int pradio_screen(void)
{
    int selection = ROW_ANY;

    /* A radio hands you sound, not a menu: coming in from the main menu the
     * dial goes straight back to whatever it was on last, or to a station
     * picked for you when there is no last. The list is what you get by
     * coming back here from a station that is already playing, which is
     * also what stops this from tuning again on the way out. */
    if (!pradio_playing())
    {
        scan_stations();

        int station = last_station();
        if (station < 0 && nstations > 0)
            station = rand() % nstations;

        if (tune(station))
            return GO_TO_WPS;
    }

    while (1)
    {
        scan_stations();

        struct simplelist_info info;
        simplelist_info_init(&info, str(LANG_PSEUDO_RADIO),
                             ROW_SETTINGS + 1, NULL);
        info.get_name = row_name;
        info.title_icon = Icon_Tuner;
        info.selection = selection;
        simplelist_show_list(&info);

        if (info.selection < 0)
            return GO_TO_PREVIOUS;
        selection = info.selection;

        if (selection == ROW_SETTINGS)
        {
            do_menu(&pradio_settings_menu, NULL, NULL, false);
            continue;
        }

        /* "Any station" is a station picked for you, not a mode: the
         * choosing happens once, here, and the rest of the tune-in is the
         * same as if it had been picked by hand. */
        int station = (selection == ROW_ANY)
            ? (nstations > 0 ? rand() % nstations : -1)
            : selection - ROW_FIRST;

        if (tune(station))
            return GO_TO_WPS;
    }
}
