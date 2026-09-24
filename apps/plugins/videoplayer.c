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

/* Video Player: plays what librbvideo can decode (MP4, MKV, WebM, FLV,
 * AVI, animated GIF and WebP - see apps/video/rbvideo.h), picture and
 * sound, with the panel turned sideways for a wide clip.
 *
 * The decoding happens in the library's own thread at the lowest priority
 * the system has; this plugin hands it the clock, takes the newest frame
 * and feeds the PCM mixer. A clip too heavy for the processor loses
 * frames, never sound.
 *
 * Keys (HiBy R1):
 *   Power         play / pause          Long Power  stop
 *   Play / Next   back / forward 10 s   Vol Up/Down volume
 *   Power + Next  turn the picture      Power + Play  how it fits
 */

#include "plugin.h"
#include "lib/helper.h"
#include "lib/pluginlib_actions.h"

#ifndef HAVE_VIDEO
#error videoplayer needs HAVE_VIDEO
#endif

#define AUDIO_RATE      44100
#define RING_FRAMES     (AUDIO_RATE * 3 / 2)     /* stereo frames */
#define OSD_TICKS       (3 * HZ)
#define SEEK_STEP_MS    10000

enum fit_mode { FIT_CONTAIN = 0, FIT_COVER, FIT_STRETCH, FIT_COUNT };

static const struct rbv_api *api;
static struct rbv *v;
static struct rbv_info info;

static uint16_t *frame;          /* out_w x out_h, from the plugin buffer */
static int out_w, out_h;         /* what the decoder fills */
static int dst_x, dst_y;         /* where it goes on the panel */
static bool rotated;
static int fit = FIT_CONTAIN;

static int16_t *ring;            /* stereo frames */
static volatile int ring_head;   /* read by the mixer */
static volatile int ring_tail;   /* written here */
static long handed_frames;       /* frames given to the mixer */
static long base_ms;             /* media time of the first handed frame */
static bool have_audio;
static bool audio_clock;         /* the sound is what the clock follows */
static bool paused;
static long silent_tick;         /* the clock when the sound does not run */
static long stall_tick;          /* when the sound last moved */
static long stall_ms;

/* ------------------------------------------------------------- audio */

static int ring_used(void)
{
    int n = ring_tail - ring_head;
    return n < 0 ? n + RING_FRAMES : n;
}

static void ring_reset(void)
{
    ring_head = ring_tail = 0;
}

/* Called from the mixer: hand it the next contiguous run. */
static void get_more(const void **start, size_t *size)
{
    int head = ring_head, tail = ring_tail;
    int n = tail - head;

    if (n < 0)
        n = RING_FRAMES - head;     /* to the end of the buffer */
    if (n > 512)
        n = 512;                    /* short runs: the clock follows them */
    if (n <= 0)
    {
        *start = NULL;
        *size = 0;
        return;
    }
    *start = &ring[2 * head];
    *size = (size_t)n * 4;
    ring_head = (head + n) % RING_FRAMES;
    handed_frames += n;
}

static void audio_start(void)
{
    if (!have_audio)
        return;
    rb->mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
    rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, get_more, NULL, 0);
}

static void pcm_off(void)
{
    rb->pcm_play_lock();
    rb->mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    rb->pcm_play_unlock();
}

/* The volume keys mean what they mean everywhere else. */
static void set_volume(int delta)
{
    int vol = rb->global_status->volume + delta;
    int lo = rb->sound_min(SOUND_VOLUME);
    int hi = rb->sound_max(SOUND_VOLUME);
    if (vol < lo)
        vol = lo;
    if (vol > hi)
        vol = hi;
    if (vol != rb->global_status->volume)
    {
        rb->global_status->volume = vol;
        rb->sound_set(SOUND_VOLUME, vol);
    }
}

/* Moves decoded sound from the decoder into the ring. */
static void audio_pump(void)
{
    int16_t buf[1024 * 2];
    int space, got;

    if (!have_audio)
        return;
    space = RING_FRAMES - 1 - ring_used();
    while (space > 1024)
    {
        int64_t pts;
        got = api->read_audio(v, buf, 1024, &pts);
        if (got <= 0)
            break;
        for (int i = 0; i < got; i++)
        {
            int tail = ring_tail;
            ring[2 * tail] = buf[2 * i];
            ring[2 * tail + 1] = buf[2 * i + 1];
            ring_tail = (tail + 1) % RING_FRAMES;
        }
        space -= got;
    }
}

/* Where playback is, in the clip. */
static long clock_ms(void)
{
    if (!audio_clock)
        return base_ms;     /* moved along by the caller */
    long pending = rb->mixer_channel_get_bytes_waiting(PCM_MIXER_CHAN_PLAYBACK)
                   / 4;
    long played = handed_frames - pending;
    if (played < 0)
        played = 0;
    return base_ms + played * 1000 / AUDIO_RATE;
}

/* Once a second, what the player and the decoder are doing, to
 * /.rockbox/videoplayer.log: a clip that does not play on the device is
 * then a file to read rather than a guess. Overwritten per clip. */
static int log_fd = -1;
static long osd_drawn = -1;
static long log_tick;

static void log_state(void)
{
    struct rbv_stats st;
    if (log_fd < 0 || TIME_BEFORE(*rb->current_tick, log_tick))
        return;
    log_tick = *rb->current_tick + HZ;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    api->stats(v, &st);
    rb->fdprintf(log_fd, "t=%ld status=%d dec=%d shown=%d drop=%d lag=%d "
                 "ring=%d handed=%ld mixer=%d aclock=%d\n",
                 clock_ms(), api->status(v), st.decoded, st.shown,
                 st.dropped, st.lag_ms, ring_used(), handed_frames,
                 (int)rb->mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK),
                 audio_clock);
}

/* --------------------------------------------------------------- video */

static void layout(void)
{
    int sw = info.width > 0 ? info.width : 1;
    int sh = info.height > 0 ? info.height : 1;
    int aw = rotated ? LCD_HEIGHT : LCD_WIDTH;    /* the area, as the clip
                                                   * sees it */
    int ah = rotated ? LCD_WIDTH : LCD_HEIGHT;

    if (fit == FIT_STRETCH || fit == FIT_COVER)
    {
        out_w = aw;
        out_h = ah;
    }
    else
    {
        if ((int64_t)sw * ah > (int64_t)sh * aw)
        {
            out_w = aw;
            out_h = (int)((int64_t)sh * aw / sw) & ~1;
        }
        else
        {
            out_h = ah;
            out_w = (int)((int64_t)sw * ah / sh) & ~1;
        }
        if (out_w < 2) out_w = 2;
        if (out_h < 2) out_h = 2;
    }
    dst_x = rotated ? (LCD_WIDTH - out_h) / 2 : (LCD_WIDTH - out_w) / 2;
    dst_y = rotated ? (LCD_HEIGHT - out_w) / 2 : (LCD_HEIGHT - out_h) / 2;
    rb->lcd_clear_display();
    rb->lcd_update();
}

static void draw_frame(void)
{
    if (!rotated)
    {
        rb->lcd_bitmap((const fb_data *)frame, dst_x, dst_y, out_w, out_h);
        rb->lcd_update_rect(dst_x, dst_y, out_w, out_h);
        return;
    }
    /* Turned a quarter: the clip's column x becomes the panel's row. */
    {
        static fb_data row[LCD_WIDTH];
        for (int x = 0; x < out_w; x++)
        {
            for (int y = 0; y < out_h; y++)
                row[y] = (fb_data)frame[(size_t)y * out_w + (out_w - 1 - x)];
            rb->lcd_bitmap(row, dst_x, dst_y + x, out_h, 1);
        }
        rb->lcd_update_rect(dst_x, dst_y, out_h, out_w);
    }
}

/* ----------------------------------------------------------------- OSD */

static void osd_draw(long now_ms)
{
    char left[16], right[16];
    int y = LCD_HEIGHT - 40;
    int bar_w = LCD_WIDTH - 40;
    long dur = info.duration_ms > 0 ? (long)info.duration_ms : 0;
    int fill = dur > 0 ? (int)((int64_t)bar_w * now_ms / dur) : 0;

    rb->snprintf(left, sizeof(left), "%ld:%02ld", now_ms / 60000,
                 (now_ms / 1000) % 60);
    rb->snprintf(right, sizeof(right), "%ld:%02ld", dur / 60000,
                 (dur / 1000) % 60);

    rb->lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    rb->lcd_fillrect(0, y - 26, LCD_WIDTH, 66);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_putsxy(20, y - 24, left);
    rb->lcd_putsxyf(LCD_WIDTH - 20 - rb->font_get_width(rb->font_get(FONT_UI),
                                                        '0') * 5,
                    y - 24, "%s%s", paused ? "|| " : "", right);
    rb->lcd_drawrect(20, y, bar_w, 8);
    if (fill > 0)
        rb->lcd_fillrect(20, y, fill, 8);
    rb->lcd_update_rect(0, y - 26, LCD_WIDTH, 66);
}

static void osd_clear(void)
{
    int y = LCD_HEIGHT - 40;
    rb->lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    rb->lcd_fillrect(0, y - 26, LCD_WIDTH, 66);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_update_rect(0, y - 26, LCD_WIDTH, 66);
}

/* --------------------------------------------------------------- main */

static void seek_to(long ms)
{
    if (ms < 0)
        ms = 0;
    if (info.duration_ms > 0 && ms > (long)info.duration_ms - 500)
        ms = (long)info.duration_ms - 500;
    pcm_off();
    ring_reset();
    handed_frames = 0;
    base_ms = ms;
    api->set_clock(v, ms, !paused);
    if (!paused)
        audio_start();
}

static char os_path[MAX_PATH];

/* Opens the clip at ms with the current size, turn and fit. */
static bool open_clip(long ms)
{
    struct rbv_config cfg;

    layout();
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = sizeof(cfg);
    cfg.out_w = out_w;
    cfg.out_h = out_h;
    cfg.fit = (fit == FIT_COVER) ? RBV_FIT_COVER :
              (fit == FIT_STRETCH) ? RBV_FIT_STRETCH : RBV_FIT_CONTAIN;
    cfg.flags = info.has_audio ? RBV_AUDIO : 0;
    cfg.audio_rate = AUDIO_RATE;
    v = api->open(os_path, &cfg);
    if (!v)
        return false;
    have_audio = info.has_audio;
    audio_clock = have_audio;
    stall_ms = -1;
    stall_tick = *rb->current_tick;
    ring_reset();
    handed_frames = 0;
    base_ms = ms;
    api->set_clock(v, ms, !paused);
    audio_pump();
    if (!paused)
        audio_start();
    return true;
}

/* The turn or the fit changed: the decoder scales to the frame, so the
 * stream is opened again at the same place. */
static bool reopen(void)
{
    long at = clock_ms();
    pcm_off();
    api->close(v);
    v = NULL;
    return open_clip(at);
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    long osd_until = 0;
    bool quit = false;
    bool ended = false;
    int old_freq;

    if (!path || !*path)
        return PLUGIN_ERROR;

    api = rb->video_lib();
    if (!api)
    {
        rb->splash(HZ * 2, "Video decoder not installed");
        return PLUGIN_ERROR;
    }

    /* The clip's own sound replaces the music. */
    if (rb->audio_status())
        rb->audio_stop();

    {
        size_t bufsize;
        unsigned char *buf = rb->plugin_get_buffer(&bufsize);
        size_t need_frame = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
        size_t need_ring = (size_t)RING_FRAMES * 2 * sizeof(int16_t);
        if (bufsize < need_frame + need_ring)
        {
            rb->splash(HZ * 2, "Out of memory");
            return PLUGIN_ERROR;
        }
        frame = (uint16_t *)buf;
        ring = (int16_t *)(buf + need_frame);
    }

    {
        const char *p = rb->video_os_path(path, os_path, sizeof(os_path));
        if (!p)
            return PLUGIN_ERROR;
        if (p != os_path)
            rb->strlcpy(os_path, p, sizeof(os_path));
    }
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (api->probe(os_path, &info) < 0 ||
        (!info.has_video && !info.has_audio))
    {
        rb->splash(HZ * 2, "Cannot play this file");
        return PLUGIN_ERROR;
    }
    /* A wide clip gets the panel sideways. */
    rotated = (info.width > 0 && info.height > 0 &&
               info.width > info.height);

    backlight_ignore_timeout();
    old_freq = rb->mixer_get_frequency();
    /* info, not have_audio: that is only set once the clip is open. */
    if (info.has_audio)
        rb->mixer_set_frequency(AUDIO_RATE);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    rb->lcd_update();

    log_fd = rb->open(ROCKBOX_DIR "/videoplayer.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd >= 0)
        rb->fdprintf(log_fd, "%s\n%dx%d %ldms video=%d audio=%d %s\n",
                     api->build, info.width, info.height,
                     (long)info.duration_ms, info.has_video, info.has_audio,
                     info.codec);
    if (!open_clip(0))
    {
        rb->splash(HZ * 2, "Cannot play this file");
        backlight_use_settings();
        return PLUGIN_ERROR;
    }
    osd_until = *rb->current_tick + OSD_TICKS;
    silent_tick = *rb->current_tick;

    while (!quit)
    {
        long now;
        int button;

        audio_pump();
        /* The mixer stops a channel whose callback comes back empty, and
         * at the start - or whenever the decoder falls behind - the ring
         * is empty. Start it again once there is something to play. */
        if (have_audio && !paused && ring_used() > 0 &&
            rb->mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_STOPPED)
            audio_start();
        log_state();
        /* If the mixer is not taking the sound (a simulator with no
         * audio device, say), the clip still has to play: the clock
         * falls back to the tick. */
        if (audio_clock && !paused)
        {
            long t = *rb->current_tick;
            long ms = clock_ms();
            if (ms != stall_ms)
            {
                stall_ms = ms;
                stall_tick = t;
            }
            else if (t - stall_tick > HZ)
            {
                audio_clock = false;
                base_ms = ms;
                silent_tick = t;
            }
        }
        if (!audio_clock)
        {
            /* No sound to follow: the clock is the clock. */
            long t = *rb->current_tick;
            if (!paused)
                base_ms += (t - silent_tick) * 1000 / HZ;
            silent_tick = t;
        }
        now = clock_ms();
        api->set_clock(v, now, !paused);

        if (info.has_video)
        {
            int64_t pts;
            if (api->get_frame(v, frame, out_w, &pts) > 0)
                draw_frame();
        }

        if (api->status(v) == RBV_EOF &&
            (!audio_clock || ring_used() == 0))
            ended = true;
        if (ended)
            quit = true;

        if (osd_until && TIME_AFTER(*rb->current_tick, osd_until))
        {
            osd_clear();
            osd_until = 0;
        }
        else if (osd_until && now / 250 != osd_drawn)
        {
            /* Four times a second is plenty for a clock and a bar; every
             * pass was a big-font redraw and a push the sound waited on. */
            osd_drawn = now / 250;
            osd_draw(now);
        }

        button = rb->button_get_w_tmo(HZ / 30);
        switch (button)
        {
            case BUTTON_NONE:
                break;
            case BUTTON_POWER | BUTTON_REL:
                paused = !paused;
                if (have_audio)
                    rb->mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK,
                                                 !paused);
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            case BUTTON_POWER | BUTTON_REPEAT:
                quit = true;
                break;
            case BUTTON_LEFT:
            case BUTTON_LEFT | BUTTON_REPEAT:
                seek_to(clock_ms() - SEEK_STEP_MS);
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            case BUTTON_RIGHT:
            case BUTTON_RIGHT | BUTTON_REPEAT:
                seek_to(clock_ms() + SEEK_STEP_MS);
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            case BUTTON_UP:
            case BUTTON_UP | BUTTON_REPEAT:
                set_volume(+1);
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            case BUTTON_DOWN:
            case BUTTON_DOWN | BUTTON_REPEAT:
                set_volume(-1);
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            case BUTTON_POWER | BUTTON_RIGHT:
                rotated = !rotated;
                if (!reopen())
                    quit = true;
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            case BUTTON_POWER | BUTTON_LEFT:
                fit = (fit + 1) % FIT_COUNT;
                if (!reopen())
                    quit = true;
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
            default:
                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                    quit = true;
                osd_until = *rb->current_tick + OSD_TICKS;
                break;
        }
    }

    pcm_off();
    if (log_fd >= 0)
        rb->close(log_fd);
    log_fd = -1;
    if (v)
        api->close(v);
    v = NULL;
    if (have_audio)
        rb->mixer_set_frequency(old_freq);
    backlight_use_settings();
    rb->lcd_clear_display();
    rb->lcd_update();
    return PLUGIN_OK;
}
