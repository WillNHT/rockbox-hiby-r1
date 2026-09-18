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
#include "misc.h"
#include "fixedpoint.h"

/** Beep generation, CPU optimized **/
#include "asm/beep.c"

static bool beep_noise;         /* Generating noise, not a square wave */
static void beep_generate_noise(int16_t *buf, int count, int amplitude);
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
        if (beep_noise)
            beep_generate_noise(beep_buf, count, beep_amplitude);
        else
            beep_generate((void *)beep_buf, count, &beep_phase,
                          beep_step, beep_amplitude);
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
