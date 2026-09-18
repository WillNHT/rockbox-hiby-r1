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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "string-extra.h"
#include "strmemccpy.h"
#include "misc.h"
#include "audio.h"
#include "metadata.h"
#include "settings.h"
#include "power.h"
#include "screen_access.h"
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
#include "hiby_bluetooth.h"
#endif
#include "video_lib.h"
#include "video_surface.h"
#include "video_art.h"
#include "screensaver.h"

/* Tried in this order next to the track. */
static const char * const clip_exts[] =
{
    "mp4", "m4v", "webm", "mkv", "mov", "flv", "avi", "mpg", "mpeg",
};
static const char * const image_exts[] =
{
    "gif", "webp", "apng",
};

/* <track>.video.cfg */
struct sync
{
    long offset_ms;         /* the video starts this long into the track */
    long rate_x1000;        /* video speed against the track's */
    long start_ms;          /* trim at the start of the video */
    long end_ms;            /* trim at its end, 0 = none */
    bool hold;              /* outside the video: its first/last frame */
};

static struct
{
    char track[MAX_PATH];
    unsigned long track_gen;    /* bumps when the lookup must run again */
    unsigned long looked_gen;
    char music[MAX_PATH];       /* music video, "" for none */
    char canvas[MAX_PATH];      /* animated cover, "" for none */
    struct sync sync;
    struct video_surface surf;
} art;

/* ----------------------------------------------------------- lookup */

static bool try_exts(char *buf, size_t size, int stem,
                     const char * const exts[], int n)
{
    for (int i = 0; i < n; i++)
    {
        snprintf(buf + stem, size - stem, ".%s", exts[i]);
        if (file_exists(buf))
            return true;
    }
    return false;
}

bool video_find_sidecar(const char *path, const char *suffix, bool images,
                        char *buf, size_t size)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    int stem = (dot && (!slash || dot > slash)) ? dot - path
                                                : (int)strlen(path);
    int n;

    n = snprintf(buf, size, "%.*s%s", stem, path, suffix);
    if (n <= 0 || (size_t)n >= size - 8)
        return false;
    if (try_exts(buf, size, n, clip_exts, ARRAYLEN(clip_exts)))
        return true;
    if (images && try_exts(buf, size, n, image_exts, ARRAYLEN(image_exts)))
        return true;
    buf[0] = '\0';
    return false;
}

/* <dir>/<name>.<ext> for the album-wide canvas. */
static bool find_in_dir(const char *track, const char *name, bool images,
                        char *buf, size_t size)
{
    const char *slash = strrchr(track, '/');
    int dirlen = slash ? slash - track + 1 : 0;
    int n = snprintf(buf, size, "%.*s%s", dirlen, track, name);
    if (n <= 0 || (size_t)n >= size - 8)
        return false;
    if (try_exts(buf, size, n, clip_exts, ARRAYLEN(clip_exts)))
        return true;
    if (images && try_exts(buf, size, n, image_exts, ARRAYLEN(image_exts)))
        return true;
    buf[0] = '\0';
    return false;
}

/* "1.002" -> 1002 with scale 1000 */
static long parse_long(const char *s, long scale)
{
    bool neg = false;
    long whole = 0, frac = 0, div = 1, v;
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '-' || *s == '+')
        neg = (*s++ == '-');
    while (isdigit((unsigned char)*s))
        whole = whole * 10 + (*s++ - '0');
    if (*s == '.')
    {
        s++;
        while (isdigit((unsigned char)*s) && div < scale)
        {
            frac = frac * 10 + (*s++ - '0');
            div *= 10;
        }
    }
    v = whole * scale + frac * scale / div;
    return neg ? -v : v;
}

static void read_sync(const char *video, struct sync *sy)
{
    char path[MAX_PATH], line[128];
    const char *dot = strrchr(video, '.');
    int fd;

    memset(sy, 0, sizeof(*sy));
    sy->rate_x1000 = 1000;
    if (!dot)
        return;
    snprintf(path, sizeof(path), "%.*s.cfg", (int)(dot - video), video);
    fd = open_utf8(path, O_RDONLY);
    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        char *hash = strchr(line, '#');
        char *eq = strpbrk(line, "=:");
        char *key = line, *val, *end;
        if (hash)
            *hash = '\0';
        if (!eq)
            continue;
        *eq = '\0';
        val = eq + 1;
        while (isspace((unsigned char)*key))
            key++;
        end = eq;
        while (end > key && isspace((unsigned char)end[-1]))
            *--end = '\0';
        while (isspace((unsigned char)*val))
            val++;

        if (!strcasecmp(key, "offset_ms"))
            sy->offset_ms = strtol(val, NULL, 10);
        else if (!strcasecmp(key, "rate"))
            sy->rate_x1000 = parse_long(val, 1000);
        else if (!strcasecmp(key, "start_ms"))
            sy->start_ms = strtol(val, NULL, 10);
        else if (!strcasecmp(key, "end_ms"))
            sy->end_ms = strtol(val, NULL, 10);
        else if (!strcasecmp(key, "outside"))
            sy->hold = !strncasecmp(val, "hold", 4);
    }
    close(fd);
    if (sy->rate_x1000 < 250 || sy->rate_x1000 > 4000)
        sy->rate_x1000 = 1000;
}

static void lookup(struct mp3entry *id3)
{
    art.music[0] = '\0';
    art.canvas[0] = '\0';
    if (!global_settings.video_enabled)
        return;

    if (global_settings.video_music &&
        video_find_sidecar(id3->path, ".video", false, art.music,
                           sizeof(art.music)))
        read_sync(art.music, &art.sync);

    if (global_settings.video_canvas &&
        !video_find_sidecar(id3->path, ".canvas", true, art.canvas,
                            sizeof(art.canvas)) &&
        !find_in_dir(id3->path, "canvas", true, art.canvas,
                     sizeof(art.canvas)))
    {
        /* An animated cover.gif / cover.webp is a canvas too. */
        const char *slash = strrchr(id3->path, '/');
        int dirlen = slash ? slash - id3->path + 1 : 0;
        int n = snprintf(art.canvas, sizeof(art.canvas), "%.*scover",
                         dirlen, id3->path);
        if (n <= 0 || (size_t)n >= sizeof(art.canvas) - 8 ||
            !try_exts(art.canvas, sizeof(art.canvas), n, image_exts,
                      ARRAYLEN(image_exts)))
            art.canvas[0] = '\0';
    }
}

/* Loads what the playing track has, when the track changed. */
static struct mp3entry *update(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return NULL;
    if (strcmp(art.track, id3->path) || art.looked_gen != art.track_gen)
    {
        strmemccpy(art.track, id3->path, sizeof(art.track));
        art.looked_gen = art.track_gen;
        lookup(id3);
    }
    return id3;
}

/* ------------------------------------------------------------ clock */

static long latency_ms(void)
{
    long ms = global_settings.video_latency;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    if (bt_is_connected_fast())
        ms += global_settings.video_bt_latency;
#endif
    return ms;
}

/* Where the music video is for this track position, and whether the
 * track is inside it. */
static bool music_position(struct mp3entry *id3, long *ms)
{
    const struct sync *sy = &art.sync;
    long t = (long)id3->elapsed - latency_ms() - sy->offset_ms;
    long pos;

    if (t < 0)
    {
        *ms = sy->start_ms;
        return sy->hold;
    }
    pos = sy->start_ms + (long)((int64_t)t * sy->rate_x1000 / 1000);
    *ms = pos;
    if (sy->end_ms > 0 && pos > sy->end_ms)
    {
        *ms = sy->end_ms;
        return sy->hold;
    }
    return true;
}

static int battery_fps(void)
{
#if CONFIG_CHARGING
    if (charger_inserted())
        return 0;
#endif
    return screensaver_active() ? global_settings.saver_battery_fps
                                : global_settings.video_battery_fps;
}

/* ------------------------------------------------------------ public */

static enum video_art_kind wanted(struct mp3entry *id3, long *ms)
{
    if (!global_settings.video_enabled)
        return VIDEO_ART_NONE;
    if (art.music[0] && global_settings.video_show_music)
    {
        if (!music_position(id3, ms))
            return VIDEO_ART_NONE;
        /* A video that has run out shows the cover again, unless held. */
        if (!art.sync.hold && art.surf.v &&
            !strcmp(art.surf.path, art.music) &&
            video_surface_ended(&art.surf))
            return VIDEO_ART_NONE;
        return VIDEO_ART_MUSIC;
    }
    if (art.canvas[0])
        return VIDEO_ART_CANVAS;
    return VIDEO_ART_NONE;
}

enum video_art_kind video_art_kind(void)
{
    struct mp3entry *id3 = update();
    long ms;
    enum video_art_kind k;
    if (!id3)
        return VIDEO_ART_NONE;
    k = wanted(id3, &ms);
    /* A clip that will not open is no picture. */
    if (k == VIDEO_ART_MUSIC && art.surf.failed &&
        !strcmp(art.surf.path, art.music))
        return VIDEO_ART_NONE;
    if (k == VIDEO_ART_CANVAS && art.surf.failed &&
        !strcmp(art.surf.path, art.canvas))
        return VIDEO_ART_NONE;
    if (k != VIDEO_ART_NONE && !video_lib())
        return VIDEO_ART_NONE;
    return k;
}

bool video_art_track_has_music_video(void)
{
    return update() && art.music[0];
}

void video_art_toggle_music_video(void)
{
    global_settings.video_show_music = !global_settings.video_show_music;
}

bool video_art_draw(struct screen *display, int x, int y, int w, int h,
                    int fit, bool full)
{
    struct mp3entry *id3 = update();
    int status = audio_status();
    bool playing = (status & AUDIO_STATUS_PLAY) &&
                   !(status & AUDIO_STATUS_PAUSE);
    long ms = 0;
    enum video_art_kind k;

    if (!id3)
        return false;
    k = wanted(id3, &ms);
    if (k == VIDEO_ART_NONE)
        return false;

    if (k == VIDEO_ART_MUSIC)
    {
        if (!video_surface_open(&art.surf, art.music, w, h, fit, 0,
                                battery_fps()))
            return false;
        video_surface_set_clock(&art.surf, ms, playing);
    }
    else
    {
        if (!video_surface_open(&art.surf, art.canvas, w, h, fit, RBV_LOOP,
                                battery_fps()))
            return false;
        /* A canvas loops while the music plays and rests with it. */
        video_surface_run(&art.surf, playing);
    }
    return video_surface_draw(&art.surf, display, x, y, full);
}

/* ------------------------------------------------------------ clips */

#define MAX_CLIPS 4
#define CLIP_FORGET_TICKS (3 * HZ)

static struct
{
    struct video_surface surf;
    long last_draw;
} clips[MAX_CLIPS];

static int clip_fps(void)
{
#if CONFIG_CHARGING
    if (charger_inserted())
        return 0;
#endif
    return screensaver_active() ? global_settings.saver_battery_fps
                                : global_settings.video_battery_fps;
}

bool video_clip_draw(struct screen *display, const char *path,
                     int x, int y, int w, int h, int fit, bool full)
{
    struct video_surface *s = NULL;
    int oldest = 0;

    if (!global_settings.video_enabled || !path)
        return false;

    for (int i = 0; i < MAX_CLIPS; i++)
    {
        struct video_surface *c = &clips[i].surf;
        if (c->path[0] && !strcmp(c->path, path) && c->w == w && c->h == h
            && c->fit == fit)
        {
            s = c;
            oldest = i;
            break;
        }
        /* Something not drawn for a while is not on any screen. */
        if (c->path[0] &&
            TIME_AFTER(current_tick, clips[i].last_draw + CLIP_FORGET_TICKS))
            video_surface_close(c);
        if (clips[i].last_draw < clips[oldest].last_draw || !c->path[0])
            oldest = i;
    }
    if (!s)
    {
        s = &clips[oldest].surf;
        video_surface_close(s);
    }
    clips[oldest].last_draw = current_tick;

    if (!video_surface_open(s, path, w, h, fit, RBV_LOOP, clip_fps()))
        return false;
    video_surface_run(s, true);
    return video_surface_draw(s, display, x, y, full);
}

void video_art_release(void)
{
    video_surface_close(&art.surf);
    for (int i = 0; i < MAX_CLIPS; i++)
        video_surface_close(&clips[i].surf);
    art.track_gen++;
}
