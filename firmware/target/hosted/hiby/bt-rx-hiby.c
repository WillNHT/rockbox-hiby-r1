/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026
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

/* Bluetooth receive: a PC or phone plays to the R1 over A2DP.
 *
 * bluealsa, run with the a2dp-sink profile, offers the sender's audio as an
 * ALSA capture stream. A pump thread reads it, through "plug" so it arrives
 * at the mixer's rate, into a small ring, and a mixer channel of its own
 * plays the ring - the same shape as usb-dac-hiby.c. It is mixed in with
 * whatever Rockbox plays, so it comes out of the jack, or the headset.
 *
 * The pump also notes when the stream last carried sound, so the music can
 * duck under it (hiby_bluetooth.c). */

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "kernel.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "system.h"

void hiby_debug_log(const char *format, ...);

/* Ring of S16 stereo frames. A power of two so head/tail wrap cleanly. */
#define RX_RING_FRAMES   8192  /* ~186 ms at 44.1 kHz */
#define RX_CHUNK_FRAMES  512   /* mixer buffer granularity */
/* a peak over this (about -36 dBFS) is the sender playing, not its hiss */
#define RX_LOUD          512

static int16_t rx_ring[RX_RING_FRAMES * 2];
static volatile unsigned int rx_head;   /* frames produced; pump only */
static volatile unsigned int rx_tail;   /* frames consumed; mixer only */
static volatile long rx_loud_tick;
static pthread_t rx_thread;
static volatile bool rx_running;
static char rx_dev[96];

/* Mixer callback: the next chunk, silence where the ring ran dry */
static void rx_get_more(const void **start, size_t *size)
{
    static int16_t out[2][RX_CHUNK_FRAMES * 2];
    static int which;
    int16_t *buf = out[which ^= 1];
    unsigned int tail = rx_tail;
    unsigned int avail = MIN(rx_head - tail, RX_CHUNK_FRAMES);

    for (unsigned int i = 0; i < avail; i++)
    {
        unsigned int idx = (tail + i) & (RX_RING_FRAMES - 1);
        buf[2 * i]     = rx_ring[2 * idx];
        buf[2 * i + 1] = rx_ring[2 * idx + 1];
    }
    memset(&buf[2 * avail], 0, (RX_CHUNK_FRAMES - avail) * 2 * sizeof(int16_t));
    rx_tail = tail + avail;

    *start = buf;
    *size = RX_CHUNK_FRAMES * 2 * sizeof(int16_t);
}

static snd_pcm_t *rx_open(unsigned int rate)
{
    snd_pcm_t *h;

    if (snd_pcm_open(&h, rx_dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0)
        return NULL;
    if (snd_pcm_set_params(h, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, rate, 1, 200000) < 0)
    {
        snd_pcm_close(h);
        return NULL;
    }
    snd_pcm_start(h);
    hiby_debug_log("bt rx: open %s at %u Hz", rx_dev, rate);
    return h;
}

/* A raw pthread: usleep, never Rockbox's sleep() */
static void *rx_pump(void *arg)
{
    int16_t buf[RX_CHUNK_FRAMES * 2];
    snd_pcm_t *h = NULL;
    unsigned int rate = 0;
    (void)arg;

    while (rx_running)
    {
        unsigned int head = rx_head;
        snd_pcm_sframes_t n;
        int peak = 0;

        /* the mixer follows the track's rate: the capture follows it */
        if (h && rate != mixer_get_frequency())
        {
            snd_pcm_close(h);
            h = NULL;
        }
        if (!h)
        {
            rate = mixer_get_frequency();
            /* the sender's PCM is not up yet: retry, but stay quick to stop */
            for (int i = 0; !(h = rx_open(rate)) && i < 10 && rx_running; i++)
                usleep(50000);
            if (!h)
                continue;
        }
        if (head - rx_tail > RX_RING_FRAMES - RX_CHUNK_FRAMES)
        {
            usleep(2000);
            continue;
        }
        if (snd_pcm_wait(h, 100) <= 0 && rx_running)
        {
            /* nothing for 100 ms: the sender paused, or an overrun */
            if (snd_pcm_state(h) == SND_PCM_STATE_XRUN)
            {
                snd_pcm_prepare(h);
                snd_pcm_start(h);
            }
            continue;
        }
        n = snd_pcm_readi(h, buf, RX_CHUNK_FRAMES);
        if (n == -EAGAIN)
            continue;
        if (n < 0 && snd_pcm_recover(h, n, 1) == 0)
        {
            snd_pcm_start(h);
            continue;
        }
        if (n < 0)
        {
            /* the sender went: open afresh once it is back */
            hiby_debug_log("bt rx: read failed (%s)", snd_strerror(n));
            snd_pcm_close(h);
            h = NULL;
            continue;
        }
        for (snd_pcm_sframes_t i = 0; i < n; i++)
        {
            unsigned int idx = (head + i) & (RX_RING_FRAMES - 1);
            rx_ring[2 * idx]     = buf[2 * i];
            rx_ring[2 * idx + 1] = buf[2 * i + 1];
            peak = MAX(peak, abs(buf[2 * i]));
            peak = MAX(peak, abs(buf[2 * i + 1]));
        }
        rx_head = head + n;
        if (peak > RX_LOUD)
            rx_loud_tick = current_tick;
    }
    if (h)
        snd_pcm_close(h);
    return NULL;
}

/* The channel is not playing though the pump runs: a new track rate
 * resets the mixer, and a switch of output stops the PCM */
bool hiby_bt_rx_stalled(void)
{
    return rx_running && (!pcm_is_playing()
               || mixer_channel_status(PCM_MIXER_CHAN_BTRX) == CHANNEL_STOPPED);
}

/* Rockbox thread only, like usb_dac_start(). Running already, it only
 * starts the channel again. */
void hiby_bt_rx_start(const char *mac)
{
    if (!rx_running)
    {
        snprintf(rx_dev, sizeof(rx_dev), "plug:'bluealsa:DEV=%s,PROFILE=a2dp'", mac);
        rx_head = rx_tail = 0;
        rx_loud_tick = current_tick - HZ*10;
        rx_running = true;
        if (pthread_create(&rx_thread, NULL, rx_pump, NULL) != 0)
        {
            rx_running = false;
            return;
        }
        hiby_debug_log("bt rx: started %s", mac);
    }
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BTRX, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BTRX, rx_get_more, NULL, 0);
}

void hiby_bt_rx_stop(void)
{
    if (!rx_running)
        return;
    rx_running = false;
    pthread_join(rx_thread, NULL);
    mixer_channel_stop(PCM_MIXER_CHAN_BTRX);
    hiby_debug_log("bt rx: stopped");
}

/* The sender played something audible in the last 'within' ticks */
bool hiby_bt_rx_loud(long within)
{
    return rx_running && TIME_BEFORE(current_tick, rx_loud_tick + within);
}
