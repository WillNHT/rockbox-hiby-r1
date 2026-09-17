/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket global keys.
 *
 * The R1 has five physical buttons and they mean the same thing on every
 * screen. That is the whole design here, and it is a deliberate break with
 * how Rockbox normally works: ordinarily a key's meaning comes from the
 * keymap table for the current context, so UP is volume in the WPS and a
 * scroll in a list. With the stick owning navigation, that variability
 * bought nothing and cost a user having to remember where they were.
 *
 * So these five are resolved here, before any context lookup:
 *
 *   POWER    tap            play / pause
 *            hold 1 s       lock / unlock input
 *            hold 4 s       shut down
 *   VOL+/-   tap            one step
 *            hold           continuous, linear in the volume value
 *   Next key press          back five seconds, at once
 *            hold           previous track
 *   Play key press          forward five seconds, at once
 *            hold           next track
 *
 * The R1 has no prev key, so the two transport keys are prev and next,
 * and play/pause moves to the stick's centre tap. They are that way round
 * because that is how they fall under the thumb, not because of what the
 * silkscreen says. BUTTON_PREV and BUTTON_NEXT exist on this target for
 * Bluetooth remotes only.
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

#if (CONFIG_KEYPAD == HIBY_R1_PAD)

#include <stdio.h>

#include "appevents.h"
#include "audio.h"
#include "backlight.h"
#include "button.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "misc.h"
#include "scroll_engine.h"
#include "timeout.h"
#include "powermgmt.h"
#include "rpkeys.h"
#include "settings.h"
#include "skin_engine/skin_engine.h"
#include "sound.h"
#include "system.h"
#include "viewport.h"

/* The three thresholds, in ticks.
 *
 * Four seconds for the shutdown, not ten, because the AXP2101 PMU has a
 * long-press force-off of its own at around six and we do not get a vote
 * on it. Above that the hardware wins and Rockbox never gets to save
 * settings or the resume position; below it we shut down cleanly and the
 * PMU stays what it should be - the way out of a wedged device, not the
 * thing that answers an ordinary hold. It also makes the countdown
 * honest, which counting to ten was not. */
#define LOCK_HOLD_TICKS      (HZ)
#define SHUTDOWN_HOLD_TICKS  (4 * HZ)

/* A held next/prev changes track once it has been down this long - the
 * button driver's first repeat, so the first skip lands as soon as a hold
 * can be told from a press. */
#define SKIP_HOLD_TICKS      (HZ * 3 / 10)

/* POWER is the one key where a tap and a hold mean unrelated things, so the
 * two are made mutually exclusive rather than merely different: a press
 * shorter than this is a tap and nothing else, a press longer than it is a
 * lock attempt and nothing else. Letting go at 600 ms - a lock the user
 * thought better of - therefore does nothing at all, where it used to fall
 * through to the screen toggle and turn the display off mid-thought.
 *
 * The same instant is when the countdown appears. It has to be after the
 * tap window closes or every screen-off tap flashes it on the way past. */
#define POWER_TAP_MAX_TICKS  (HZ * 300 / 1000)
#define LOCK_ARM_TICKS       POWER_TAP_MAX_TICKS

/* Continuous volume: one step this often while the key is held. Linear in
 * the volume value on purpose - an accelerating volume key overshoots, and
 * the range here is small enough that it does not need the help. */
#define VOL_REPEAT_TICKS     (HZ / 20)
/* How long a press waits before it starts repeating. */
#define VOL_REPEAT_DELAY     (HZ / 4)

/* Seeking. */
#define SEEK_STEP_MS         5000

static bool locked;
static bool lock_cue_done;      /* the lock has fired for this press      */
static int  lock_chirp_step;    /* how far the arming run has got         */
static long power_down_tick;    /* 0 when POWER is not held               */
static bool power_consumed;     /* this press already did something       */
static long vol_next_tick;
static bool countdown_drawn;

bool rpkeys_locked(void)
{
    return locked;
}

/* ------------------------------------------------------------------ cues */

/* Audible and visible, both, every time. The R1 has no speaker, so the
 * click is only there for someone wearing headphones - which is most of
 * the time this device is in use, and costs nothing when it is not. The
 * countdown on screen is what makes the hold legible without them. */
static void cue(void)
{
    if (keyclick_enabled(KEYCLICK_SRC_BUTTON))
        system_sound_play(SOUND_KEYCLICK);
}

/* Two chirps, one rising and one falling, for the state changes that are
 * worth telling apart without looking: something armed, or something shut.
 *
 * beep_play() stops whatever beep is already sounding rather than queueing
 * behind it, so a chirp cannot be three calls in a row - the last one would
 * be the only one heard. A timeout walks the steps instead, which also
 * keeps the UI thread free; blocking it for the length of a sound during a
 * button press is exactly when the device must not feel stuck. */
#define CHIRP_STEPS     3
#define CHIRP_STEP_MS   55
#define CHIRP_AMPLITUDE 2500

static const unsigned short chirp_up_hz[CHIRP_STEPS]   = { 700, 1050, 1500 };
static const unsigned short chirp_down_hz[CHIRP_STEPS] = { 1500, 1050, 700 };

static struct timeout chirp_tmo;
static const unsigned short *chirp_seq;
static int chirp_step;

static int chirp_tick(struct timeout *tmo)
{
    (void)tmo;

    if (!chirp_seq || chirp_step >= CHIRP_STEPS)
    {
        chirp_seq = NULL;
        return 0;                /* <= 0 unregisters */
    }

    beep_play(chirp_seq[chirp_step], CHIRP_STEP_MS, CHIRP_AMPLITUDE);
    chirp_step++;
    return HZ * CHIRP_STEP_MS / 1000;
}

static void chirp(const unsigned short *seq)
{
    timeout_cancel(&chirp_tmo);
    chirp_seq = seq;
    chirp_step = 0;
    /* The first note now, so the sound starts on the gesture rather than a
     * step later. */
    chirp_tick(&chirp_tmo);
    if (chirp_seq)
        timeout_register(&chirp_tmo, chirp_tick,
                         HZ * CHIRP_STEP_MS / 1000, 0);
}

void rpkeys_chirp_up(void)   { chirp(chirp_up_hz); }
void rpkeys_chirp_down(void) { chirp(chirp_down_hz); }

/* One note of a run, for a hold that is still building.
 *
 * A sound that only arrives once the hold has completed tells the user
 * something they already know - they are the one who kept holding. What
 * they cannot see, with the device in a pocket, is how far along they are.
 * So the run is played *during* the arming, one note per step, pitched by
 * progress, with the last note landing on the instant the thing fires.
 * Rising for something opening, falling for something shutting. */
void rpkeys_chirp_step(int step, int total, bool rising)
{
    int lo = chirp_up_hz[0];
    int hi = chirp_up_hz[CHIRP_STEPS - 1];
    int hz;

    if (total < 2)
        total = 2;
    if (step < 0)
        step = 0;
    if (step > total - 1)
        step = total - 1;

    hz = lo + (hi - lo) * step / (total - 1);
    if (!rising)
        hz = lo + hi - hz;

    /* Not the timeout-driven chirp(): one note is one beep_play, and a run
     * that is paced by the hold itself needs no sequencer. */
    timeout_cancel(&chirp_tmo);
    chirp_seq = NULL;
    beep_play(hz, CHIRP_STEP_MS, CHIRP_AMPLITUDE);
}

/* How many notes an arming run has. Four is enough to hear a direction
 * without becoming a tune. */
#define CHIRP_RUN_NOTES 4

/* --------------------------------------------------------- the countdown */

/* Full screen, because a hold that can end in a shutdown should not be
 * something the user discovers by accident. Milliseconds are shown because
 * the user asked for them and because they make it obvious the device has
 * not simply frozen. */
/* The countdown repaints a few times a second for as long as the hold
 * lasts. Clearing and pushing the whole 480x800 panel that often is what
 * made the screen underneath strobe - the update is long enough that the
 * eye catches it half-done, and on a skin the backdrop gets repainted
 * behind it every time. So: clear the whole screen exactly once, when the
 * countdown appears, then repaint only the band the two lines occupy, and
 * only every COUNTDOWN_FRAME_TICKS. */
#define COUNTDOWN_FRAME_TICKS  (HZ / 30)

/* The border is the countdown made spatial: a rectangle drawn round the
 * edge of the screen, clockwise from the top-left corner, closing exactly
 * as the hold fires. It costs almost nothing to push because it is only
 * ever *added* to - each frame inks the span since the last one and updates
 * that span's bounding box, never the whole frame. */
#define BORDER_PX              8

static long countdown_next_tick;
static long border_done;        /* perimeter pixels already inked */
static int  countdown_stage;    /* 0 = heading for the lock, 1 = shutdown */

/* Union a rectangle into an accumulating bounding box. */
static void bbox_add(int *bx, int *by, int *bw, int *bh,
                     int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (*bw == 0 || *bh == 0)
    {
        *bx = x; *by = y; *bw = w; *bh = h;
        return;
    }
    if (x < *bx)          { *bw += *bx - x; *bx = x; }
    if (y < *by)          { *bh += *by - y; *by = y; }
    if (x + w > *bx + *bw) *bw = x + w - *bx;
    if (y + h > *by + *bh) *bh = y + h - *by;
}

/* Ink the part of the perimeter between `from` and `to`, measured clockwise
 * from the top-left corner, and report what it touched. */
static void border_span(int vw, int vh, long from, long to,
                        int *bx, int *by, int *bw, int *bh)
{
    /* Side n runs [base, base + len). Lengths are the full edge, so the
     * corners get inked twice - harmless, and it keeps the arithmetic to
     * one expression per side. */
    const long len[4] = { vw, vh, vw, vh };
    long base = 0;
    int i;

    for (i = 0; i < 4; i++)
    {
        long a = from > base ? from - base : 0;
        long b = to - base;
        long n;

        if (b > len[i])
            b = len[i];
        n = b - a;

        if (n > 0)
        {
            int x, y, w, h;

            switch (i)
            {
            case 0:  /* top, left to right */
                x = (int)a; y = 0; w = (int)n; h = BORDER_PX; break;
            case 1:  /* right, top to bottom */
                x = vw - BORDER_PX; y = (int)a; w = BORDER_PX; h = (int)n; break;
            case 2:  /* bottom, right to left */
                x = vw - (int)b; y = vh - BORDER_PX; w = (int)n; h = BORDER_PX; break;
            default: /* left, bottom to top */
                x = 0; y = vh - (int)b; w = BORDER_PX; h = (int)n; break;
            }

            lcd_fillrect(x, y, w, h);
            bbox_add(bx, by, bw, bh, x, y, w, h);
        }

        base += len[i];
    }
}

static void countdown_draw(long held_ticks)
{
    struct viewport vp;
    char line[32];
    const char *what;
    long target, start, perim, want;
    long left_ms;
    int w, h, y;
    int band_y, band_h;
    int bx = 0, by = 0, bw = 0, bh = 0;
    int stage;
    bool first;

    if (held_ticks >= LOCK_HOLD_TICKS)
    {
        what = locked ? "Unlocking - hold for shutdown" : "Locking - hold for shutdown";
        target = SHUTDOWN_HOLD_TICKS;
        start = LOCK_HOLD_TICKS;
        stage = 1;
    }
    else
    {
        what = locked ? "Hold to unlock" : "Hold to lock";
        target = LOCK_HOLD_TICKS;
        start = LOCK_ARM_TICKS;
        stage = 0;
    }

    /* Crossing into the shutdown stage restarts the border from nothing, so
     * it has to restart the screen too - the first stage's rectangle is
     * closed by then and would otherwise stay up behind the second. */
    if (countdown_drawn && stage != countdown_stage)
        countdown_drawn = false;

    first = !countdown_drawn;

    if (!first && !TIME_AFTER(current_tick, countdown_next_tick))
        return;

    countdown_next_tick = current_tick + COUNTDOWN_FRAME_TICKS;
    countdown_stage = stage;

    left_ms = (target - held_ticks) * 1000 / HZ;
    if (left_ms < 0)
        left_ms = 0;

    viewport_set_defaults(&vp, SCREEN_MAIN);
    lcd_set_viewport(&vp);

    h = font_get(vp.font)->height;
    y = vp.height / 2 - h;

    /* The band the two lines live in, with a line of margin either side. */
    band_y = y - h / 2;
    band_h = 3 * h + h;
    if (band_y < 0)
        band_y = 0;
    if (band_y + band_h > vp.height)
        band_h = vp.height - band_y;

    if (first)
    {
        /* Once: take the whole screen, so nothing of what was underneath
         * shows around the countdown.
         *
         * A scrolling line does not stop for a cleared screen. The WPS
         * title is scrolled by the scroll thread, which owns its own
         * rectangle and repaints it on its own timer, so the track name
         * kept reappearing across the middle of the lock screen - not a
         * repaint race, just a second writer nobody told. */
        lcd_scroll_stop();
        lcd_clear_viewport();
        border_done = 0;
        countdown_drawn = true;
    }
    else
    {
        int mode = lcd_get_drawmode();
        lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        lcd_fillrect(0, band_y, vp.width, band_h);
        lcd_set_drawmode(mode);
    }

    bbox_add(&bx, &by, &bw, &bh, 0, band_y, vp.width, band_h);

    /* The border, as far as this frame has got. */
    perim = 2L * (vp.width + vp.height);
    want = (target > start)
         ? perim * (held_ticks - start) / (target - start)
         : perim;
    if (want < 0)
        want = 0;
    if (want > perim)
        want = perim;
    if (want > border_done)
    {
        border_span(vp.width, vp.height, border_done, want, &bx, &by, &bw, &bh);
        border_done = want;
    }

    lcd_getstringsize(what, &w, NULL);
    lcd_putsxy((vp.width - w) / 2, y, what);

    /* Hundredths. Thousandths were three digits of noise churning too fast
     * to read; two move at about the speed the eye can follow and still say
     * plainly that the device has not frozen. */
    snprintf(line, sizeof(line), "%ld.%02ld s", left_ms / 1000,
             (left_ms % 1000) / 10);
    lcd_getstringsize(line, &w, NULL);
    lcd_putsxy((vp.width - w) / 2, y + h + h / 2, line);

    if (first)
        lcd_update_viewport();
    else if (bw > 0 && bh > 0)
        lcd_update_viewport_rect(bx, by, bw, bh);

    lcd_set_viewport(NULL);
}

static void countdown_clear(void)
{
    if (!countdown_drawn)
        return;
    countdown_drawn = false;
    countdown_next_tick = 0;
    border_done = 0;
    /* Whatever was underneath owns the screen again. The lists redraw on
     * this event; the WPS does not listen to it and has to be told in its
     * own language, or the countdown stays on the skin until the track
     * changes. */
    send_event(GUI_EVENT_NEED_UI_UPDATE, NULL);
    skin_request_full_update(WPS);
    /* And the status bar, which is a skin of its own and was the half that
     * stayed: a list repaints its own viewport, so the band above it kept
     * the countdown's ink until something else happened to touch it. */
    skin_request_full_update(CUSTOM_STATUSBAR);
}

/* True while the full-screen countdown owns the display.
 *
 * A WPS with a peak meter or a visualiser repaints several times a second
 * on its own timer, and it does not know the countdown is there: the two
 * took turns clearing each other and the screen strobed for the whole
 * hold. The countdown wins, because it is the thing the user is looking
 * at, and it wins by the skin engine standing down rather than by drawing
 * faster than it. */
bool rpkeys_countdown_active(void)
{
    return countdown_drawn;
}

/* ------------------------------------------------------------- the screen */

/* --------------------------------------------------------------- playback */

/* Two taps in quick succession have to add up, and reading id3->elapsed
 * every time is what stopped them. The reported position only advances
 * when the codec gets round to updating it, so a second tap arriving
 * before that read the same elapsed as the first and seeked to the same
 * place - the skip that "sometimes works and sometimes does not". Chain
 * from the previous target instead, for as long as the burst lasts. */
#define SEEK_CHAIN_TICKS     (2 * HZ)

static long seek_target_ms;
static long seek_target_tick;

static void seek_forget(void)
{
    seek_target_tick = 0;
}

static void seek_by(long ms)
{
    struct mp3entry *id3;
    long pos;

    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return;

    id3 = audio_current_track();
    if (!id3)
        return;

    if (seek_target_tick &&
        TIME_BEFORE(current_tick, seek_target_tick + SEEK_CHAIN_TICKS))
        pos = seek_target_ms + ms;
    else
        pos = (long)id3->elapsed + ms;

    if (pos < 0)
        pos = 0;
    if (id3->length > 0 && pos > (long)id3->length)
        pos = id3->length;

    seek_target_ms = pos;
    seek_target_tick = current_tick;
    audio_ff_rewind(pos);
}

/* -------------------------------------------------------------- the keys */

static bool handle_power(int held, bool repeat, bool release)
{
    long now = current_tick;

    if (release)
    {
        long held_ticks = power_down_tick ? now - power_down_tick : 0;
        /* Strictly a tap, not merely "not a completed lock". A hold the
         * user abandoned halfway is a cancelled lock and means nothing;
         * letting it fall through to the screen toggle is what made
         * thinking better of a lock turn the display off. */
        bool was_tap = !power_consumed && held_ticks < POWER_TAP_MAX_TICKS;

        countdown_clear();
        power_down_tick = 0;
        lock_cue_done = false;
        lock_chirp_step = 0;
        power_consumed = false;

        if (was_tap)
        {
            /* Play/pause, directly. It used to be the screen toggle, with
             * play/pause on a double tap and the first tap taken back when
             * the second arrived - a blink on every double tap, and a
             * single tap that had to be read twice before it meant
             * anything.
             *
             * Two things made that unnecessary. The screen no longer goes
             * dark on its own (HAVE_BACKLIGHT_DIM_IDLE - it dims and stays
             * readable), so there is nothing to toggle; and one press with
             * one meaning is what makes the rest of the key legible, since
             * everything else on it is a hold. */
            cue();
            /* PAUSE first, and not PLAY. audio_status() keeps
             * AUDIO_STATUS_PLAY set while paused - paused is a *kind* of
             * playing, with AUDIO_STATUS_PAUSE on top - so testing PLAY
             * meant the second tap paused an already-paused track and
             * resume was unreachable. */
            if (audio_status() & AUDIO_STATUS_PAUSE)
                audio_resume();
            else if (audio_status() & AUDIO_STATUS_PLAY)
                audio_pause();
        }
        return true;
    }

    if (!power_down_tick)
    {
        power_down_tick = now;
        power_consumed = false;
        lock_cue_done = false;
        lock_chirp_step = 0;
        return true;
    }

    if (repeat)
    {
        long held_ticks = now - power_down_tick;

        /* Ten seconds is a shutdown, and it happens the moment it is
         * reached rather than on release: at that point the user is
         * holding a button down waiting for exactly this. */
        if (held_ticks >= SHUTDOWN_HOLD_TICKS)
        {
            countdown_clear();
            cue();
            sys_poweroff();
            power_consumed = true;
            return true;
        }

        /* The arming run. Rising if this hold will open something (an
         * unlock), falling if it will shut one (a lock), paced so the last
         * note lands on the instant the lock flips rather than after it -
         * the sound is the progress, not the receipt. */
        if (!lock_cue_done && held_ticks >= LOCK_ARM_TICKS)
        {
            long span = LOCK_HOLD_TICKS - LOCK_ARM_TICKS;
            int want = (int)(((held_ticks - LOCK_ARM_TICKS) * CHIRP_RUN_NOTES)
                             / (span > 0 ? span : 1));

            if (want > CHIRP_RUN_NOTES)
                want = CHIRP_RUN_NOTES;
            while (lock_chirp_step < want)
            {
                if (keyclick_enabled(KEYCLICK_SRC_LOCK))
                    rpkeys_chirp_step(lock_chirp_step, CHIRP_RUN_NOTES, locked);
                lock_chirp_step++;
            }
        }

        if (held_ticks >= LOCK_HOLD_TICKS && !lock_cue_done)
        {
            lock_cue_done = true;
            power_consumed = true;
            locked = !locked;
#if defined(BUTTON_TOUCH_WAKES) && !defined(SIMULATOR)
            button_set_touch_wake(!locked);
#endif
        }

        /* Not until the press has outlived a tap's opening moments: the
         * two gestures are identical until then, and flashing a lock
         * countdown over every screen-off tap is noise. */
        if (held_ticks >= LOCK_ARM_TICKS)
            countdown_draw(held_ticks);
        return true;
    }

    (void)held;
    return true;
}

/* One step, and one step means one percent.
 *
 * adjust_volume() honours the Volume Adjustment Mode setting, and in
 * perceptual mode a single step is a step of a *loudness* curve, which
 * lands on two of this target's raw steps over most of the range - so the
 * number on screen went up in twos. The scale here is already the one
 * people read (0-100% of -70..-20 dB, one raw step per percent), so the
 * curve has nothing left to correct and direct is the honest mode. */
static void volume_step(int steps)
{
    adjust_volume_ex(steps, VOLUME_ADJUST_DIRECT);
}

static bool handle_volume(int button, bool repeat, bool release)
{
    int steps = (button & BUTTON_UP) ? 1 : -1;

    if (release)
    {
        vol_next_tick = 0;
        return true;
    }

    if (!repeat)
    {
        /* The tap: exactly one step, and the repeat below is timed from
         * here so a hold does not double-count it. */
        volume_step(steps);
        vol_next_tick = current_tick + VOL_REPEAT_DELAY;
        return true;
    }

    if (vol_next_tick && TIME_AFTER(current_tick, vol_next_tick))
    {
        volume_step(steps);
        vol_next_tick = current_tick + VOL_REPEAT_TICKS;
    }
    return true;
}

/* next/prev: the press seeks, the hold changes track.
 *
 * With Hold Prev/Next = Continuous Skip the hold keeps changing track: one
 * track per hold_skip_delay seconds for as long as the key stays down, the
 * one in progress playing on in between. With Seek it is the single
 * change. Either way the first change comes after SKIP_HOLD_TICKS: waiting
 * the whole hold_skip_delay before the first one made a hold feel dead.
 *
 * The seek happens on the press, not on the release. Waiting for the finger
 * to come up so the two could be told apart put a visible delay in front of
 * every skip - the one thing a transport key must not have. Holding still
 * changes the track when it crosses the threshold; the five seconds it
 * already seeked are simply part of the track being left behind. */
static struct
{
    long down_tick;
    bool consumed;
    bool skipped;       /* this hold has changed track at least once */
} skipper[2];

static bool handle_skip(int idx, int dir, bool repeat, bool release)
{
    long now = current_tick;

    if (release)
    {
        skipper[idx].down_tick = 0;
        skipper[idx].consumed = false;
        skipper[idx].skipped = false;
        return true;
    }

    /* Keyed off "this is not a repeat" rather than off a down_tick that is
     * only cleared by a release. A release that never arrives - swallowed
     * by a lock that toggled mid-press, say - used to leave the key stuck
     * down and every later press ignored. */
    if (!repeat)
    {
        skipper[idx].down_tick = now;
        skipper[idx].consumed = false;
        skipper[idx].skipped = false;
        seek_by(dir * SEEK_STEP_MS);
        return true;
    }

    bool continuous = global_settings.hold_skip == HOLD_SKIP_CONTINUOUS;
    long threshold = (continuous && skipper[idx].skipped)
                     ? global_settings.hold_skip_delay * HZ
                     : SKIP_HOLD_TICKS;

    if (skipper[idx].down_tick && !skipper[idx].consumed &&
        now - skipper[idx].down_tick >= threshold)
    {
        skipper[idx].skipped = true;
        if (continuous)
            skipper[idx].down_tick = now;   /* the next one counts from here */
        else
            skipper[idx].consumed = true;
        cue();
        seek_forget();          /* a new track: nothing to chain from */
        if (dir > 0)
            audio_next();
        else
            audio_prev();
    }
    return true;
}

bool rpkeys_handle(int button)
{
    int bare = button & ~(BUTTON_REL | BUTTON_REPEAT);
    bool repeat = (button & BUTTON_REPEAT) != 0;
    bool release = (button & BUTTON_REL) != 0;

    if (button == BUTTON_NONE)
        return false;

    /* Both volume keys together is the stick's kill switch, which is
     * checked before this and must not also change the volume. */
    if ((bare & BUTTON_UP) && (bare & BUTTON_DOWN))
        return true;

    if (bare & BUTTON_POWER)
        return handle_power(bare, repeat, release);

    /* Locked: everything but POWER is swallowed, which is what locked
     * means. POWER is handled above, so a 3 s hold still unlocks. */
    if (locked)
        return true;

    if (bare & (BUTTON_UP | BUTTON_DOWN))
        return handle_volume(bare, repeat, release);

    /* The physical Next key, and the physical Play/Pause key standing in
     * for a prev key the device does not have. BUTTON_NEXT/BUTTON_PREV are
     * the Bluetooth remote's, and get the same treatment. */
    /* The physical "Next" key is the one under the thumb going backwards
     * on this body, so it is prev; Play/Pause is next. Named for where
     * they are, not for what the silkscreen says. */
    if (bare & (BUTTON_RIGHT | BUTTON_PREV))
        return handle_skip(0, -1, repeat, release);

    if (bare & (BUTTON_LEFT | BUTTON_NEXT))
        return handle_skip(1, +1, repeat, release);

    return false;
}

#endif /* CONFIG_KEYPAD == HIBY_R1_PAD */
