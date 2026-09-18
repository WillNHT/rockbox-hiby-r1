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
/* The static. Long enough to read as a tune, short enough that nobody waits
 * for it. */
#define PRADIO_STATIC_MS    320
#define PRADIO_STATIC_AMP   2200

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

    unsigned long length = 0;
    int track = pick_track(n, &length);

    /* The static plays over the gap between the last screen and the first
     * sample, which is the gap it exists to cover. Tuning in while
     * something else is still playing is the ordinary case, so it ducks
     * the music the way the other device sounds do - a cue, because
     * missing it is missing the only thing that says the dial moved. */
    if (global_settings.radio_static)
    {
        beep_duck(PRADIO_STATIC_MS, global_settings.sound_duck_cue);
        beep_play_noise(PRADIO_STATIC_MS, PRADIO_STATIC_AMP);
        sleep(HZ * PRADIO_STATIC_MS / 1000);
    }

    playlist_start(track, random_offset(length), 0);
    return true;
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

MAKE_MENU(pradio_settings_menu, ID2P(LANG_RADIO_SETTINGS), NULL, Icon_Wps,
          &pradio_folder_item,
          &pradio_min_length,
          &pradio_static,
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

    while (1)
    {
        scan_stations();

        struct simplelist_info info;
        simplelist_info_init(&info, str(LANG_PSEUDO_RADIO),
                             ROW_SETTINGS + 1, NULL);
        info.get_name = row_name;
        info.title_icon = Icon_Audio;
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
