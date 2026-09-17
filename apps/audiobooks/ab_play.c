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
 * Books while they play.
 *
 * A low priority thread looks at playback once a second. While a track
 * from the library plays it keeps the book's progress (and speed) up to
 * date, marks the book finished past the threshold, rewinds after a long
 * pause, and pauses at the end of a chapter when the sleep timer says so.
 * It gives the music its own speed back when a non-book track starts.
 */
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "audio.h"
#include "playback.h"
#include "playlist.h"
#include "settings.h"
#include "powermgmt.h"
#include "misc.h"
#include "splash.h"
#include "lang.h"
#include "string-extra.h"
#include "pathfuncs.h"
#include "sound.h"
#include "cuesheet.h"
#include "metadata.h"
#ifdef HAVE_PITCHCONTROL
#include "tdspeed.h"
#endif
#include "abdb.h"
#include "audiobooks.h"

#define SAVE_INTERVAL   (60 * HZ)

static struct mutex ab_mutex;

void ab_lock(void)   { mutex_lock(&ab_mutex); }
void ab_unlock(void) { mutex_unlock(&ab_mutex); }

bool audiobooks_is_book(const char *path)
{
    return ab_path_in_library(path);
}

/* --- speed ------------------------------------------------------------ */

static bool music_speed_saved;
static int32_t music_speed, music_pitch;

static void apply_book_speed(const struct ab_progress *p)
{
#ifdef HAVE_PITCHCONTROL
    if (!music_speed_saved)
    {
        music_speed = dsp_get_timestretch();
        music_pitch = sound_get_pitch();
        music_speed_saved = true;
    }
    if (p && p->speed)
    {
        sound_set_pitch(p->pitch ? p->pitch : PITCH_SPEED_100);
        dsp_set_timestretch(p->speed);
    }
#else
    (void)p;
#endif
}

static void restore_music_speed(void)
{
#ifdef HAVE_PITCHCONTROL
    if (!music_speed_saved)
        return;
    sound_set_pitch(music_pitch);
    dsp_set_timestretch(music_speed);
    music_speed_saved = false;
#endif
}

/* --- starting a book -------------------------------------------------- */

static void seconds_ago_rewind(struct ab_progress *p, int *file,
                               unsigned long *elapsed)
{
    int after = global_settings.ab_rewind_after;
    int amount = global_settings.ab_rewind_amount;
    if (!p || after <= 0 || amount <= 0 || !p->last_played)
        return;
    uint32_t now = ab_now();
    if (now < p->last_played || now - p->last_played < (uint32_t)after * 60)
        return;
    unsigned long back = amount * 1000UL;
    *elapsed = *elapsed > back ? *elapsed - back : 0;
    (void)file;
}

bool audiobooks_play_at(int book, int file, unsigned long elapsed)
{
    /* a dialog; not while holding the library */
    if (!warn_on_pl_erase())
        return false;

    ab_lock();
    const struct ab_book *b = ab_db_book(book);
    if (!b)
    {
        ab_unlock();
        return false;
    }

    char path[MAX_PATH];
    strmemccpy(path, ab_db_str(b, AB_S_PATH), sizeof path);
    uint32_t id = b->id;
    bool single = b->single;

    char dir[MAX_PATH];
    strmemccpy(dir, path, sizeof dir);
    if (single)
    {
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[slash == dir ? 1 : 0] = '\0';
    }

    const char **names;
    int n = ab_db_list_files(path, single, &names);
    if (n <= 0 || playlist_create(dir, NULL) < 0)
    {
        ab_unlock();
        splashf(HZ, "%s", str(LANG_AB_EMPTY));
        return false;
    }
    for (int i = 0; i < n; i++)
        playlist_insert_track(NULL, names[i], PLAYLIST_INSERT_LAST, false, false);
    playlist_sync(NULL);
    ab_unlock();

    if (file < 0 || file >= n)
    {
        file = 0;
        elapsed = 0;
    }

    struct ab_progress *p = ab_progress_touch(id);
    p->file = file;
    p->elapsed = elapsed;
    p->last_played = ab_now();
    ab_progress_dirty();

    apply_book_speed(p);
    playlist_start(file, elapsed, 0);
    return true;
}

bool audiobooks_play(int book, bool start_over)
{
    const struct ab_book *b = ab_db_book(book);
    if (!b)
        return false;
    struct ab_progress *p = ab_progress_get(b->id);
    int file = 0;
    unsigned long elapsed = 0;

    if (p && (start_over || p->finished))
    {
        /* Resuming a finished book starts it again. */
        p->finished = 0;
        p->position = 0;
        ab_progress_dirty();
    }
    else if (p)
    {
        file = p->file;
        elapsed = p->elapsed;
        seconds_ago_rewind(p, &file, &elapsed);
    }
    return audiobooks_play_at(book, file, elapsed);
}

/* --- the watcher ------------------------------------------------------ */

static struct
{
    char     path[MAX_PATH];   /* track last seen */
    uint32_t book_id;          /* 0: not a book */
    int      file;
    uint32_t before;           /* ms of the files before this one */
    bool     playing, paused;
    long     pause_tick;       /* when the pause started; 0 = none */
    long     last_save;
    unsigned long last_elapsed;
    bool     at_last_file;
} w;

/* End of chapter */
static struct
{
    bool armed;
    char path[MAX_PATH];       /* the track the chapter is in */
    unsigned long cue_end;     /* ms; 0 = the end of the file */
} eoc;

static void arm_chapter_end(const struct mp3entry *id3)
{
    eoc.cue_end = 0;
    eoc.path[0] = '\0';
    if (!id3)
        return;
    strmemccpy(eoc.path, id3->path, sizeof eoc.path);
    struct cuesheet *cue = id3->cuesheet;
    if (cue && cue->curr_track_idx + 1 < cue->track_count)
        eoc.cue_end = get_cue_track(cue, cue->curr_track_idx + 1)->offset;
}

void audiobooks_set_sleep(int minutes)
{
    if (minutes < 0)
    {
        set_sleeptimer_duration(0);
        eoc.armed = false;
        arm_chapter_end((audio_status() & AUDIO_STATUS_PLAY) ?
                        audio_current_track() : NULL);
        eoc.armed = true;
    }
    else
    {
        eoc.armed = false;
        set_sleeptimer_duration(minutes);
    }
}

bool audiobooks_sleep_at_chapter_end(void)
{
    return eoc.armed;
}

static void rewind_by_setting(unsigned long elapsed)
{
    unsigned long back = global_settings.ab_rewind_amount * 1000UL;
    audio_pre_ff_rewind();
    audio_ff_rewind(elapsed > back ? elapsed - back : 0);
}

static bool long_pause(void)
{
    int after = global_settings.ab_rewind_after;
    return w.pause_tick && after > 0 && global_settings.ab_rewind_amount > 0 &&
           TIME_AFTER(current_tick, w.pause_tick + after * 60 * HZ);
}

void audiobooks_before_resume(void)
{
    if (w.book_id && long_pause())
    {
        struct mp3entry *id3 = audio_current_track();
        if (id3)
            rewind_by_setting(id3->elapsed);
    }
    w.pause_tick = 0;
}

static void track_changed(const struct mp3entry *id3)
{
    strmemccpy(w.path, id3->path, sizeof w.path);

    ab_lock();
    int file = 0;
    int book = ab_db_find_file(id3->path, &file);
    const struct ab_book *b = ab_db_book(book);
    uint32_t id = b ? b->id : 0;
    uint32_t before = 0;
    bool last = false;
    if (b)
    {
        const uint32_t *len = ab_db_lengths(b);
        for (int i = 0; i < file && i < b->nfiles; i++)
            before += len[i];
        last = file == b->nfiles - 1;
    }
    ab_unlock();

    if (id && !w.book_id)
        apply_book_speed(ab_progress_get(id));
    else if (!id && w.book_id)
        restore_music_speed();
    else if (id && id != w.book_id)
    {
        struct ab_progress *p = ab_progress_get(id);
        if (p && p->speed)
            apply_book_speed(p);
    }

    w.book_id = id;
    w.file = file;
    w.before = before;
    w.at_last_file = last;
}

static void update_progress(const struct mp3entry *id3)
{
    struct ab_progress *p = ab_progress_touch(w.book_id);
    p->file = w.file;
    p->elapsed = id3->elapsed;
    p->position = w.before + id3->elapsed;
    p->last_played = ab_now();
#ifdef HAVE_PITCHCONTROL
    p->speed = dsp_get_timestretch();
    p->pitch = sound_get_pitch();
#endif

    ab_lock();
    const struct ab_book *b = ab_db_book(ab_db_find_id(w.book_id));
    if (b && b->length && !p->finished &&
        (uint64_t)p->position * 100 >=
            (uint64_t)b->length * global_settings.ab_finished_pct)
        p->finished = 1;
    ab_unlock();

    ab_progress_dirty();
    w.last_elapsed = id3->elapsed;
}

static void chapter_end_check(const struct mp3entry *id3, bool changed)
{
    if (!eoc.armed || w.paused)
        return;
    if (!eoc.path[0])
    {
        arm_chapter_end(id3);
        return;
    }
    bool end = changed && strcmp(eoc.path, id3->path);
    if (!end && eoc.cue_end && id3->elapsed >= eoc.cue_end)
        end = true;
    if (end)
    {
        eoc.armed = false;
        audio_pause();
    }
}

static void watch(void)
{
    int status = audio_status();
    bool playing = status & AUDIO_STATUS_PLAY;
    bool paused = status & AUDIO_STATUS_PAUSE;
    struct mp3entry *id3 = playing ? audio_current_track() : NULL;

    if (id3 && id3->path[0])
    {
        bool changed = strcmp(id3->path, w.path) != 0;
        if (changed)
            track_changed(id3);

        /* Right after a start the position may not have caught up with
         * the resume point yet; do not overwrite a good one with 0. */
        if (w.book_id && !paused && !changed)
        {
            struct ab_progress *p = ab_progress_get(w.book_id);
            if (id3->elapsed > 0 || !p || p->file != w.file || !p->elapsed)
                update_progress(id3);
        }

        chapter_end_check(id3, changed);

        if (paused && !w.paused)
        {
            w.pause_tick = current_tick;
            ab_progress_save();
            w.last_save = current_tick;
        }
        else if (!paused && w.paused && w.pause_tick)
        {
            /* resumed by something other than the WPS */
            if (w.book_id && long_pause())
                rewind_by_setting(id3->elapsed);
            w.pause_tick = 0;
        }
    }
    else if (w.playing)
    {
        /* Stopped. A book that ran out at its last file is finished. */
        if (w.book_id && w.at_last_file)
        {
            struct ab_progress *p = ab_progress_get(w.book_id);
            ab_lock();
            const struct ab_book *b = ab_db_book(ab_db_find_id(w.book_id));
            if (p && b && b->nfiles &&
                w.last_elapsed + 5000 >= ab_db_lengths(b)[b->nfiles - 1])
            {
                p->finished = 1;
                ab_progress_dirty();
            }
            ab_unlock();
        }
        w.path[0] = '\0';
        w.book_id = 0;
        w.pause_tick = 0;
        ab_progress_save();
        w.last_save = current_tick;
    }

    w.playing = playing && id3;
    w.paused = paused;

    if (TIME_AFTER(current_tick, w.last_save + SAVE_INTERVAL))
    {
        ab_progress_save();
        w.last_save = current_tick;
    }
}

static struct event_queue ab_queue SHAREDBSS_ATTR;
static long ab_stack[(DEFAULT_STACK_SIZE + 0x2000) / sizeof(long)];
static const char ab_thread_name[] = "audiobooks";

static void ab_thread(void)
{
    struct queue_event ev;
    while (1)
    {
        queue_wait_w_tmo(&ab_queue, &ev, HZ);
        watch();
    }
}

void audiobooks_init(void)
{
    mutex_init(&ab_mutex);
    ab_progress_load();
    ab_db_load();
    audiobooks_ui_init();
    w.last_save = current_tick;
    queue_init(&ab_queue, false);
    create_thread(ab_thread, ab_stack, sizeof(ab_stack), 0, ab_thread_name
                  IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
}

void audiobooks_startup_scan(void)
{
    if (!global_settings.ab_autoscan)
        return;
    ab_lock();
    ab_db_scan(false, true);
    ab_unlock();
}
