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
#ifndef _RBVIDEO_H_
#define _RBVIDEO_H_

/* The interface between Rockbox and librbvideo.so, the video decoder.
 *
 * librbvideo.so is FFmpeg (and libwebp for animated WebP) behind this
 * small API, built by tools/rbvideo/build.sh and loaded at run time from
 * /.rockbox/lib. Rockbox never sees an FFmpeg header, so the two can be
 * updated separately; RBV_API_VERSION changes whenever this file does in
 * a way that breaks either side.
 *
 * Decoding runs on the library's own operating-system thread at the
 * lowest priority the system has, so it only ever gets the time audio
 * does not need. Rockbox tells it what time it is (rbv_set_clock) and
 * picks up the newest frame that is due (rbv_get_frame); the thread drops
 * frames, and skips decoding the ones nothing refers to, when it cannot
 * keep up. A slow video is a slower picture, never a skip in the music.
 *
 * Pixels are RGB565 in the machine's byte order. Times are milliseconds
 * from the start of the file. Everything here is safe to call from one
 * Rockbox thread; nothing is safe to call from two at once. */

#include <stdint.h>

#define RBV_API_VERSION 1

enum rbv_fit
{
    RBV_FIT_COVER = 0,      /* fill the frame, cropping the source */
    RBV_FIT_CONTAIN,        /* the whole source, bars around it */
    RBV_FIT_STRETCH,
};

enum rbv_flags
{
    RBV_LOOP      = 1 << 0, /* start again at the end (the clock wraps) */
    RBV_AUDIO     = 1 << 1, /* decode the audio too, for rbv_read_audio() */
    RBV_NO_VIDEO  = 1 << 2, /* audio only */
    RBV_FAST      = 1 << 3, /* cheaper, uglier decoding from the start */
};

enum rbv_status
{
    RBV_OK = 0,
    RBV_EOF = 1,            /* the last frame has been shown */
    RBV_ERROR = -1,
};

struct rbv_config
{
    int size;               /* sizeof(struct rbv_config) */
    int out_w, out_h;       /* the frame rbv_get_frame() fills */
    int fit;                /* enum rbv_fit */
    int flags;              /* enum rbv_flags */
    int audio_rate;         /* output rate for RBV_AUDIO, stereo S16 */
    int max_fps;            /* show at most this many frames a second, 0: all */
    int idle_ms;            /* stop decoding when no frame was asked for in this long, 0: never */
};

struct rbv_info
{
    int size;               /* sizeof(struct rbv_info) */
    int width, height;      /* of the picture, 0 without video */
    int64_t duration_ms;    /* 0 when unknown */
    int fps_x1000;          /* nominal frame rate, 0 when unknown */
    int has_video, has_audio;
    int frames;             /* for animated images, 0 when unknown */
    char codec[24];         /* video codec name, or audio codec's */
};

struct rbv_stats
{
    int size;               /* sizeof(struct rbv_stats) */
    int decoded, shown, dropped;
    int lag_ms;             /* how far behind the clock the last frame was */
};

struct rbv;

/* The table librbvideo.so exports, as `__header`. */
struct rbv_api
{
    int version;            /* RBV_API_VERSION */
    const char *build;      /* human-readable: library versions */

    int (*probe)(const char *path, struct rbv_info *info);
    struct rbv *(*open)(const char *path, const struct rbv_config *cfg);
    void (*close)(struct rbv *v);

    /* Where playback is. Frames up to media_ms are due. A jump of more
     * than a second, or backwards, seeks. With RBV_LOOP the clock is
     * taken modulo the duration. */
    void (*set_clock)(struct rbv *v, int64_t media_ms, int playing);

    /* Copies the newest due frame into dst (out_w x out_h, `stride`
     * pixels a row) if it is newer than the last one copied. Returns 1
     * when it copied, 0 when there is nothing new, and a negative
     * enum rbv_status when the stream failed. *pts gets the frame's time. */
    int (*get_frame)(struct rbv *v, uint16_t *dst, int stride,
                     int64_t *pts);

    /* Up to `frames` stereo frames of audio (RBV_AUDIO), and the time of
     * the first. Returns the count; 0 when none is ready yet. */
    int (*read_audio)(struct rbv *v, int16_t *buf, int frames,
                      int64_t *pts);

    int (*status)(struct rbv *v);           /* enum rbv_status */
    void (*stats)(struct rbv *v, struct rbv_stats *st);
    int (*info)(struct rbv *v, struct rbv_info *info);

    /* Borrows the newest due frame to draw it without a copy: *px is
     * out_w x out_h pixels, row by row, valid until unlock_frame(), which
     * must follow at once - the decoder waits on it. Returns the frame's
     * serial (> 0; it changes with every new frame), 0 when there is no
     * frame yet, or a negative enum rbv_status. */
    int (*lock_frame)(struct rbv *v, const uint16_t **px, int64_t *pts);
    void (*unlock_frame)(struct rbv *v);
};

#endif /* _RBVIDEO_H_ */
