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
 * Deliberately not sample playback. The beep channel takes raw PCM with no
 * decoder behind it, so shipping five WAVs would mean a WAV reader, five
 * files that have to be on the card, and a card read on a UI event.
 * ponytail: that is also what "user-replaceable sound files" (issue #64
 * item 6) would need - the knobs below are the customisation there is
 * until someone wants it enough to pay for the reader. */
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
