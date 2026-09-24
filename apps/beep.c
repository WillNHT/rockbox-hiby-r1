/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (c) 2011 Michael Sevakis
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
#include <stdlib.h>
#include "system.h"
#include "settings.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "pcmbuf.h"
#include "timeout.h"
#include "misc.h"
#include "fixedpoint.h"
#include "file.h"
#include <string.h>

/** Beep generation, CPU optimized **/
#include "asm/beep.c"

/** Audio prioritisation **/

/* A device sound is only useful if it can be heard over what is playing, so
 * the music steps aside for as long as the sound lasts and a little after.
 * The tail keeps a run of chirps from flapping the volume between notes. */
#define DUCK_TAIL_MS    150

static struct timeout duck_tmo;
static int duck_depth;          /* 0 when the music is at full volume */

/* Fading the music in after a restart is the same lever held down longer:
 * a duck that lifts a step at a time. Kept apart from duck_depth so a cue
 * during the fade - the radio tuning in, say - does not end it. */
#define FADE_STEP_TICKS (HZ / 20)
static struct timeout fade_tmo;
static int fade_depth;          /* 0 when not fading in */
static int fade_step;

static void duck_apply(void)
{
    pcmbuf_duck(MAX(duck_depth, fade_depth));
}

static int duck_release(struct timeout *tmo)
{
    (void)tmo;
    duck_depth = 0;
    duck_apply();
    return 0;                   /* <= 0 unregisters */
}

static int fade_tick(struct timeout *tmo)
{
    (void)tmo;
    fade_depth = MAX(fade_depth - fade_step, 0);
    duck_apply();
    return fade_depth > 0 ? FADE_STEP_TICKS : 0;
}

/* Bring the music up from silence over 'duration' ms. */
void beep_fade_in(unsigned int duration)
{
    int steps = MAX(1, (int)(HZ * duration / 1000 / FADE_STEP_TICKS));

    fade_step = MAX(1, 100 / steps);
    fade_depth = 100;
    duck_apply();
    timeout_register(&fade_tmo, fade_tick, FADE_STEP_TICKS, 0);
}

/* Step the music aside by 'percent' for the length of a sound. Several
 * sounds overlapping keep the deepest of them until they have all ended. */
void beep_duck(unsigned int duration, int percent)
{
    if (percent <= 0 || !global_settings.sound_duck)
        return;

    if (percent > duck_depth)
    {
        duck_depth = percent;
        duck_apply();
    }

    timeout_register(&duck_tmo, duck_release,
                     HZ * (duration + DUCK_TAIL_MS) / 1000 + 1, 0);
}

static bool beep_noise;         /* Generating noise, not a square wave */
static void beep_generate_noise(int16_t *buf, int count, int amplitude);
static void beep_generate_fx(int16_t *buf, int count);

/* Non-zero duration means the radio generator has the channel - see the
 * note above beep_play_fx(). */
static struct beep_fx beep_fx;
static int      fx_done;        /* samples generated so far          */
static int      fx_total;       /* samples in the whole sound        */
static int32_t  fx_lp;          /* one-pole lowpass state            */
static uint32_t fx_tone_phase;
static uint32_t beep_phase;     /* Phase of square wave generator */
static uint32_t beep_step;      /* Step of square wave generator on each sample */
#ifdef BEEP_GENERIC
static int16_t  beep_amplitude; /* Amplitude of square wave generator */
#else
/* Optimized routines do XOR with phase sign bit in both channels at once */
static uint32_t beep_amplitude; /* Amplitude of square wave generator */
#endif
static int beep_count;          /* Number of samples remaining to generate */

#define BEEP_COUNT(fs, duration) ((fs) / 1000 * (duration))

/* Reserve enough static space for keyclick to fit in worst case */
#define BEEP_BUF_COUNT  BEEP_COUNT(PLAY_SAMPR_MAX, KEYCLICK_DURATION)
static int16_t beep_buf[BEEP_BUF_COUNT*2] IBSS_ATTR __attribute__((aligned(4)));

/* Callback to generate the beep frames - also don't want inlining of
   call below in beep_play */
static void __attribute__((noinline))
beep_get_more(const void **start, size_t *size)
{
    int count = beep_count;

    if (count > 0)
    {
        count = MIN(count, BEEP_BUF_COUNT);
        beep_count -= count;
        *start = beep_buf;
        *size = count * 2 * sizeof (int16_t);
        if (beep_fx.duration)
            beep_generate_fx(beep_buf, count);
        else if (beep_noise)
            beep_generate_noise(beep_buf, count, beep_amplitude);
        else
            beep_generate((void *)beep_buf, count, &beep_phase,
                          beep_step, beep_amplitude);
    }
}

/** Radio noises **/

/* One generator covers the whole family of sounds a radio makes, because
 * they are all the same thing with different knobs: hiss is noise, a
 * heterodyne whistle is noise with a gliding tone through it, a dropout is
 * noise with its level pulled out from under it, interference is noise
 * chopped into bursts, and a thump is noise with the top taken off. Five
 * separate generators would be five copies of this loop.
 *
 * Generated rather than shipped as samples: nothing has to be on the card
 * and nothing is read on a UI event. A user who wants a sound of their
 * own drops a WAV in place of one - see beep_play_wav() below. */
void beep_play_fx(const struct beep_fx *fx)
{
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);

    if (!fx || fx->duration == 0 || fx->amplitude <= 0)
        return;

    beep_fx = *fx;
    if (beep_fx.amplitude > INT16_MAX)
        beep_fx.amplitude = INT16_MAX;

    beep_noise = true;
    fx_done = 0;
    fx_lp = 0;
    fx_tone_phase = 0;
    fx_total = beep_count = BEEP_COUNT(mixer_get_frequency(), fx->duration);

    const void *start;
    size_t size;

    beep_get_more(&start, &size);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP,
                            beep_count ? beep_get_more : NULL,
                            start, size);
}

static void beep_generate_fx(int16_t *buf, int count)
{
    const uint32_t fout = mixer_get_frequency();
    int i;

    for (i = 0; i < count; i++, fx_done++)
    {
        /* Where we are through the sound, 0..256. Everything that changes
         * over the sound is a straight line in this. */
        int pos = fx_total > 0 ? (int)((int64_t)fx_done * 256 / fx_total) : 0;
        int env;

        switch (beep_fx.shape)
        {
            case FX_FLAT:   env = 256;                        break;
            case FX_FADEIN: env = pos;                        break;
            case FX_FADEOUT:env = 256 - pos;                  break;
            /* Out and back: what a station sounds like passing behind
             * something. Loudest at the ends, gone in the middle. */
            case FX_DIP:    env = pos < 128 ? 256 - pos * 2
                                            : (pos - 128) * 2; break;
            /* In and out: a swell, the shape of tuning past a station. */
            default:        env = pos < 128 ? pos * 2
                                            : (256 - pos) * 2; break;
        }

        int amp = beep_fx.amplitude * env / 256;
        int s = (rand() % (2 * amp + 1)) - amp;

        /* One-pole lowpass. 0 leaves white noise alone; higher numbers take
         * the hiss off until what is left is a rumble. */
        if (beep_fx.lowpass > 0)
        {
            fx_lp += (s - fx_lp) >> beep_fx.lowpass;
            /* Smoothing costs level; half the shift back is about right and
             * the clamp catches the rest. */
            s = (int)(fx_lp << (beep_fx.lowpass / 2));
            if (s > INT16_MAX)  s = INT16_MAX;
            if (s < -INT16_MAX) s = -INT16_MAX;
        }

        /* A tone gliding across the noise: the heterodyne whistle you get
         * turning a dial past a carrier. */
        if (beep_fx.tone_start > 0)
        {
            int hz = beep_fx.tone_start +
                     (beep_fx.tone_end - beep_fx.tone_start) * pos / 256;
            fx_tone_phase += fp_div(hz, fout, 32);
            int t = ((int32_t)fx_tone_phase < 0 ? -amp : amp) / 2;
            s = (s + t) / 2;
        }

        /* Bursts: the sound cuts out for stretches of it. */
        if (beep_fx.gate_pct > 0 && (rand() % 100) < beep_fx.gate_pct)
            s = 0;

        *buf++ = (int16_t)s;
        *buf++ = (int16_t)s;
    }
}

/* White noise, both channels the same: it is meant to be heard as one
 * sound in the middle of the head rather than as stereo weather. rand() is
 * good enough - this is the hiss between stations, not a dither source. */
static void beep_generate_noise(int16_t *buf, int count, int amplitude)
{
    for (int i = 0; i < count; i++)
    {
        int16_t s = (int16_t)((rand() % (2 * amplitude + 1)) - amplitude);
        *buf++ = s;
        *buf++ = s;
    }
}

/* Generates a constant square wave sound with a given frequency in Hertz for
   a duration in milliseconds */
void beep_play(unsigned int frequency, unsigned int duration,
               unsigned int amplitude)
{
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    beep_noise = false;

    if (frequency == 0 || duration == 0 || amplitude == 0)
        return;

    if (amplitude > INT16_MAX)
        amplitude = INT16_MAX;

    /* Setup the parameters for the square wave generator */
    uint32_t fout = mixer_get_frequency();
    beep_phase = 0;
    beep_step = fp_div(frequency, fout, 32);
    beep_count = BEEP_COUNT(fout, duration);

#ifdef BEEP_GENERIC
    beep_amplitude = amplitude;
#else
    /* Optimized routines do XOR with phase sign bit in both channels at once */
    beep_amplitude = amplitude | (amplitude << 16); /* Word:|AMP16|AMP16| */
#endif

    /* If it fits - avoid cb overhead */
    const void *start;
    size_t size;

    /* Generate first frame here */
    beep_get_more(&start, &size);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP,
                            beep_count ? beep_get_more : NULL,
                            start, size);
}

/* Static. The same path as beep_play(), noise in place of the square wave:
 * the mixer channel, the buffer and the callback are all already here, and
 * a second one of each would buy nothing. */
void beep_play_noise(unsigned int duration, unsigned int amplitude)
{
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);

    if (duration == 0 || amplitude == 0)
        return;

    if (amplitude > INT16_MAX)
        amplitude = INT16_MAX;

    beep_noise = true;
    beep_amplitude = amplitude;
    beep_count = BEEP_COUNT(mixer_get_frequency(), duration);

    const void *start;
    size_t size;

    beep_get_more(&start, &size);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP,
                            beep_count ? beep_get_more : NULL,
                            start, size);
}

/** Sound files **/

/* Any radio noise can be a file instead (#64): a 16-bit PCM WAV, mono or
 * stereo, at any rate - resampled here, nearest sample, to the mixer's.
 * At most WAV_FRAMES frames, about three seconds; a longer file is cut.
 * Read whole, on the UI thread, when the sound is played: these are short
 * and played rarely, and a static buffer means nothing is allocated. */
#define WAV_FRAMES (3 * 48000)
static int16_t wav_buf[WAV_FRAMES * 2] __attribute__((aligned(4)));

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Fill wav_buf from the data chunk; returns the frames written. */
static uint32_t wav_decode(int fd, uint32_t len, int channels, uint32_t rate,
                           int percent)
{
    static int16_t chunk[256 * 2];
    const uint32_t fout = mixer_get_frequency();
    const uint32_t in_frames = len / (2 * channels);
    uint32_t out_frames = (uint64_t)in_frames * fout / rate;
    uint32_t start = 0, have = 0, j;

    if (out_frames > WAV_FRAMES)
        out_frames = WAV_FRAMES;

    for (j = 0; j < out_frames; j++)
    {
        uint32_t s = (uint64_t)j * rate / fout;
        while (s >= start + have)
        {
            uint32_t want = MIN(256, in_frames - (start + have));
            start += have;
            have = 0;
            if (want == 0)
                return j;
            ssize_t n = read(fd, chunk, want * 2 * channels);
            if (n <= 0)
                return j;
            have = n / (2 * channels);
        }
        const int16_t *f = &chunk[(s - start) * channels];
        int32_t l = (int16_t)letoh16(f[0]) * percent / 100;
        int32_t r = channels == 2 ? (int16_t)letoh16(f[1]) * percent / 100 : l;
        wav_buf[2 * j]     = (int16_t)MAX(INT16_MIN, MIN(INT16_MAX, l));
        wav_buf[2 * j + 1] = (int16_t)MAX(INT16_MIN, MIN(INT16_MAX, r));
    }
    return j;
}

/* Play a WAV on the beep channel at percent of its level. Returns its
 * length in ms, or 0 - nothing played - when there is no such file or it
 * is not one this reads. */
int beep_play_wav(const char *path, int percent)
{
    uint8_t hdr[12], ck[8], fmt[16];
    int channels = 0, bits = 0;
    uint32_t rate = 0, frames = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;

    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    beep_fx.duration = 0;

    if (read(fd, hdr, 12) == 12 && !memcmp(hdr, "RIFF", 4) &&
        !memcmp(hdr + 8, "WAVE", 4))
    {
        while (read(fd, ck, 8) == 8)
        {
            uint32_t len = le32(ck + 4);
            if (!memcmp(ck, "fmt ", 4) && len >= 16 &&
                read(fd, fmt, 16) == 16)
            {
                if ((fmt[0] | fmt[1] << 8) != 1)      /* not plain PCM */
                    break;
                channels = fmt[2] | fmt[3] << 8;
                rate = le32(fmt + 4);
                bits = fmt[14] | fmt[15] << 8;
                lseek(fd, len - 16 + (len & 1), SEEK_CUR);
            }
            else if (!memcmp(ck, "data", 4))
            {
                if (bits == 16 && (channels == 1 || channels == 2) && rate)
                    frames = wav_decode(fd, len, channels, rate, percent);
                break;
            }
            else
                lseek(fd, len + (len & 1), SEEK_CUR);
        }
    }
    close(fd);

    if (frames == 0)
        return 0;

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, NULL, wav_buf,
                            frames * 2 * sizeof(int16_t));
    return MAX(1, (int)((uint64_t)frames * 1000 / mixer_get_frequency()));
}
