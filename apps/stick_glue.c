/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - the Rockbox side of the engine.
 *
 * Everything that knows about Rockbox lives here: turning a resolved
 * binding into the same button code a physical key would have produced,
 * replaying a tap that fell through, the click track, and the hardware
 * escape hatch. The engine itself (stick.c) knows none of it.
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

#ifdef HAVE_TOUCHSCREEN

#include <stdio.h>
#include <string.h>

#include "action.h"
#include "appevents.h"
#include "button.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "misc.h"
#include "rbpaths.h"
#include "settings.h"
#include "skin_engine/skin_engine.h"
#include "splash.h"
#include "list.h"
#include "stick.h"
#include "rpkeys.h"
#include "stick_glue.h"
#include "touchscreen.h"

#define LOGF_ENABLE
#include "logf.h"

/* The engine can only emit what a physical key emits, so it needs a table
 * of this target's key codes. Where no such table exists the feature
 * compiles but stays permanently off, which keeps other touch targets
 * exactly as they were. */
#if (CONFIG_KEYPAD == HIBY_R1_PAD)

struct binding_button
{
    int button;      /* the code to look up                       */
    int prebutton;   /* what last->button must be for a prereq    */
    /* For bindings that run until told to stop - the seeks. Emitted when
     * the hold that started them ends. BUTTON_NONE for everything else. */
    int release;
    int release_pre;
};

/* Deliberately the codes from keymap-hibyr1.c, not actions. What a binding
 * means is then whatever the current screen makes of that key, which is
 * spec 4's "meaning depends on the current screen" for free, and it keeps
 * working when the user remaps their keys.
 *
 * Scroll bindings emit a bare press, never BUTTON_REPEAT. That is what
 * keeps list acceleration out of the path so the stick alone owns the
 * repeat rate. */
static const struct binding_button binding_buttons[STICK_BIND_COUNT] =
{
    [STICK_BIND_NONE]        = { BUTTON_NONE,                 BUTTON_NONE   },
    [STICK_BIND_SCROLL_UP]   = { BUTTON_UP,                   BUTTON_NONE   },
    [STICK_BIND_SCROLL_DOWN] = { BUTTON_DOWN,                 BUTTON_NONE   },
    [STICK_BIND_SELECT]      = { BUTTON_RIGHT | BUTTON_REL,   BUTTON_RIGHT  },
    [STICK_BIND_BACK]        = { BUTTON_LEFT  | BUTTON_REL,   BUTTON_LEFT   },
    [STICK_BIND_PLAY_PAUSE]  = { BUTTON_PLAY  | BUTTON_REL,   BUTTON_PLAY   },
    [STICK_BIND_NEXT]        = { BUTTON_NEXT  | BUTTON_REL,   BUTTON_NEXT   },
    [STICK_BIND_PREV]        = { BUTTON_PREV  | BUTTON_REL,   BUTTON_PREV   },
    /* Volume only exists behind a repeat in this keymap, so these two are
     * the exception to the rule above. They are not list actions, so
     * nothing compounds. */
    [STICK_BIND_VOL_UP]      = { BUTTON_UP   | BUTTON_REPEAT, BUTTON_NONE   },
    [STICK_BIND_VOL_DOWN]    = { BUTTON_DOWN | BUTTON_REPEAT, BUTTON_NONE   },
    /* The POWER combinations. They are keys this keypad really has, so the
     * rule that the stick can only reach what a finger can reach holds. */
    [STICK_BIND_MENU]        = { BUTTON_POWER | BUTTON_LEFT,  BUTTON_NONE   },
    [STICK_BIND_QUICKSCREEN] = { BUTTON_POWER | BUTTON_DOWN,  BUTTON_NONE   },
    /* A held Next key, which is what the standard context maps
     * ACTION_STD_CONTEXT to. The prebutton matters: the keymap entry lists
     * BUTTON_RIGHT as the prerequisite, so the repeat alone does not
     * match. */
    [STICK_BIND_CONTEXT]     = { BUTTON_RIGHT | BUTTON_REPEAT, BUTTON_RIGHT },
    [STICK_BIND_STOP]        = { BUTTON_PLAY | BUTTON_REPEAT, BUTTON_PLAY   },
    /* A seek is the one thing Rockbox runs in a loop of its own until it is
     * told to stop, so these carry the code that stops them. */
    [STICK_BIND_SEEK_FWD]    = { BUTTON_NEXT | BUTTON_REPEAT, BUTTON_NONE,
                                 BUTTON_NEXT | BUTTON_REL,
                                 BUTTON_NEXT | BUTTON_REPEAT },
    [STICK_BIND_SEEK_BACK]   = { BUTTON_PREV | BUTTON_REPEAT, BUTTON_NONE,
                                 BUTTON_PREV | BUTTON_REL,
                                 BUTTON_PREV | BUTTON_REPEAT },
};

/* Both volume keys together: a combination the keymap does not use, so it
 * cannot collide with anything the user might mean. */
#define STICK_KILL_COMBO   (BUTTON_UP | BUTTON_DOWN)
#define STICK_HAVE_KEYMAP

#endif /* CONFIG_KEYPAD == HIBY_R1_PAD */

#define KILL_HOLD_TICKS    (2 * HZ)
#define BOOT_SETTLE_TICKS  (10 * HZ)

/* Written when the stick is first used and removed once it has survived
 * ten seconds. Finding it at startup means the last run did not get that
 * far, so we start with the stick off and say so. */
#define STICK_MARKER_FILE  ROCKBOX_DIR "/stick_running.tmp"

static struct stick_config live_cfg;
static struct stick_state  live_state;

static bool  initialised;
static bool  session_disabled;      /* kill switch, or a crash-loop */
static bool  kill_switched;         /* it was the kill switch       */
static bool  marker_present;
static int   cached_context = -1;
static long  first_use_tick;

/* When both volume keys went down together, or 0 if they are not held. */
static long  kill_held_since;

/* ------------------------------------------------------------------ state */

static long now_ms(void)
{
    return (long)current_tick * (1000 / HZ);
}

static void marker_write(void)
{
    int fd;

    if (marker_present)
        return;

    fd = open(STICK_MARKER_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        close(fd);
        marker_present = true;
        first_use_tick = current_tick;
    }
}

static void marker_clear(void)
{
    if (!marker_present)
        return;
    remove(STICK_MARKER_FILE);
    marker_present = false;
}

static void stick_lazy_init(void)
{
    if (initialised)
        return;
    initialised = true;

    if (file_exists(STICK_MARKER_FILE))
    {
        /* The previous run enabled the stick and never settled. Start
         * without it rather than repeating whatever went wrong. */
        logf("stick: previous run did not settle, starting disabled");
        splashf(HZ * 3, "Joystick paused: re-pick Touchscreen Mode to resume");
        remove(STICK_MARKER_FILE);
        session_disabled = true;
    }

    stick_reset(&live_state, &live_cfg);
    cached_context = -1;
}

bool stick_enabled(void)
{
#ifndef STICK_HAVE_KEYMAP
    return false;
#else
    stick_lazy_init();

    if (session_disabled)
        return false;

    /* One scheme owns the panel at a time. Absolute touch and a relative
     * stick both want to interpret the same contact, and running them
     * together made every gesture a guess about which one meant it.
     *
     * Both conditions matter. The setting is what the user chose; the
     * driver's mode is what the screen in front of them is asking for, and
     * a few screens ask for absolute touch explicitly - the Yes/No prompt
     * and the USB screen set TOUCHSCREEN_POINT/BUTTON for themselves. The
     * stick used to ignore that and keep eating the contact, which left
     * the Yes/No prompt with no working touch at all.
     *
     * Only POINT stands the stick down, deliberately. TOUCHSCREEN_BUTTON
     * is what the WPS sets for itself whenever the skin has no
     * touchregions, which is most skins - treating that as "a screen wants
     * absolute touch" would turn the stick off in the WPS for nearly every
     * theme. POINT is only ever asked for by a screen that really does
     * want a finger on a spot: the Yes/No prompt, and plugins. */
    if (touchscreen_get_mode() == TOUCHSCREEN_POINT)
        return false;

    return global_settings.touch_mode == TOUCHSCREEN_STICK;
#endif
}

bool stick_is_scrolling(void)
{
#ifndef STICK_HAVE_KEYMAP
    return false;
#else
    int phase;

    if (!stick_enabled())
        return false;

    phase = stick_phase(&live_state);
    return phase == STICK_PHASE_ARMED || phase == STICK_PHASE_COAST;
#endif
}

void stick_settings_apply(void)
{
    /* Leaving the settings screen is the user saying what they want, so an
     * automatic pause from the unclean-run check is lifted here. Only the
     * hardware kill switch stays latched for the session: that one was
     * deliberate, and it has to survive for someone escaping a stick they
     * cannot drive. */
    if (session_disabled && !kill_switched)
        session_disabled = false;

    stick_config_invalidate();
    cached_context = -1;        /* force a rebuild on the next event */
    stick_reset(&live_state, &live_cfg);
}

const struct stick_state *stick_get_state(void)   { return &live_state; }
const struct stick_config *stick_get_config(void) { return &live_cfg; }

/* ---------------------------------------------------------------- overlay */

/* A deliberately crude picture of the live gesture: a box the size of the
 * detent radius at the origin, a line out to the thumb, and a marker at
 * the end of it. Enough to see where the stick thinks it is; not a
 * finished piece of chrome.
 *
 * Everything is drawn in COMPLEMENT mode, so drawing the identical shape a
 * second time restores the pixels underneath exactly. That is what makes
 * this safe to scribble over a list without owning the redraw path or
 * keeping a copy of the framebuffer. If the screen repaints underneath a
 * live gesture the erase misses and a mark is left behind until the next
 * full redraw - acceptable for something whose whole job is to be looked
 * at while deciding whether the geometry feels right.
 *
 * This runs on the action path, which is the UI thread, so it cannot race
 * the list's own drawing. */
struct overlay_shape
{
    int  ox, oy;      /* the origin: where the thumb went down      */
    int  x, y;        /* where the thumb is now                     */
    int  r;           /* detent radius: the dead spot at the centre */
    int  R;           /* travel radius: the ring                    */
    int  sectors;
    int  rotation;
    int  style;
    /* What the gesture currently means, for the styles that say it out
     * loud. The study's floor: "mode and rate must be legible in words
     * somewhere while the stick is engaged." */
    int  mode;        /* a STICK_BIND_*, or NONE inside the dead spot   */
    int  rate;        /* rows per second, rounded                       */
    int  lit;         /* 0..255, how hard the edge lights burn          */
    /* The dial has been armed by a slow tap: a different picture
     * entirely, because the gesture now means rotation. */
    bool dial;
    int  dial_deg;    /* where the thumb sits on the ring, degrees      */
    /* Whether a skin is the thing underneath. A skin can say "the volume
     * is what your thumb means now" in its own language and in its own
     * place - on the volume bar, where the user is already looking - so
     * the overlay draws no ring there. A list has no such language, so it
     * still gets one. */
    bool on_skin;
};

/* How often a moving overlay may ask the screen underneath to repaint. */
/* How often a moving overlay may ask the screen underneath to put itself
 * back. Two rates, because the two kinds of screen cost wildly different
 * amounts to repaint: a list redraws its rows, a skin clears the whole
 * panel and rebuilds every viewport, album art and all. Sixteen of those a
 * second is most of what a now playing screen has, and it is why the WPS
 * felt heavier under the thumb than any list did. At 150 ms the trail is
 * a frame or two longer and the gesture is answered immediately. */
#define OVERLAY_REPAINT_MS      60
#define OVERLAY_REPAINT_SKIN_MS 150
#define MS_TO_TICKS(ms)        (HZ * (ms) / 1000 > 0 ? HZ * (ms) / 1000 : 1)

static struct
{
    long last_repaint;            /* tick of the last repaint request   */
    bool want;                    /* a gesture is live right now        */
    bool drawn;                   /* the shape below is on the screen   */
    bool repair_owed;             /* a shape was abandoned un-erased    */
    struct overlay_shape want_s;  /* where it should be                 */
    struct overlay_shape have_s;  /* where it currently is              */
} overlay;

/* sin(i * 11.25 degrees) * 256, one full turn in 32 steps. cos comes off
 * the same table a quarter turn along. A 32-gon is indistinguishable from a
 * circle at these radii and costs no divides. */
#define ROSE_STEPS 32
static const int16_t rose_sin[ROSE_STEPS] =
{
       0,   50,   98,  142,  181,  213,  237,  251,
     256,  251,  237,  213,  181,  142,   98,   50,
       0,  -50,  -98, -142, -181, -213, -237, -251,
    -256, -251, -237, -213, -181, -142,  -98,  -50,
};

#define ROSE_COS(i) (rose_sin[((i) + 8) & (ROSE_STEPS - 1)])

/* Screen position at radius rad, angle i/32 of a turn, 0 = straight up and
 * clockwise positive - the same convention the engine's sectors use. */
static void rose_point(const struct overlay_shape *s, int i, int rad,
                       int *px, int *py)
{
    *px = s->ox + (rad * rose_sin[i & (ROSE_STEPS - 1)]) / 256;
    *py = s->oy - (rad * ROSE_COS(i)) / 256;
}

/* Same idea for an arbitrary angle in degrees, used for the sector edges. */
static void rose_point_deg(const struct overlay_shape *s, int deg, int rad,
                           int *px, int *py)
{
    /* Round to the nearest 1/32 turn. The edges land within 6 degrees of
     * true, which is close enough to read as "this is where the sector
     * stops" and keeps the whole thing table-driven. */
    int i = ((deg * ROSE_STEPS) + 180) / 360;
    rose_point(s, i, rad, px, py);
}

static void rose_ring(const struct overlay_shape *s, int rad)
{
    int i, x0, y0, x1, y1;

    rose_point(s, 0, rad, &x0, &y0);
    for (i = 1; i <= ROSE_STEPS; i++)
    {
        rose_point(s, i, rad, &x1, &y1);
        lcd_drawline(x0, y0, x1, y1);
        x0 = x1;
        y0 = y1;
    }
}

/* Every painter reports what it touched, and the update at the end pushes
 * exactly that. A whole-panel update looked simpler and was worse: it takes
 * long enough that the list's own redraw lands in the middle of it, and the
 * overlay came out half-drawn or not at all. */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

static void dirty_reset(void)
{
    dirty_x0 = dirty_y0 = 0x7fff;
    dirty_x1 = dirty_y1 = -0x7fff;
}

static void dirty_add(int x0, int y0, int x1, int y1)
{
    if (x0 < dirty_x0) dirty_x0 = x0;
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (x1 > dirty_x1) dirty_x1 = x1;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

static void dirty_point(int x, int y, int pad)
{
    dirty_add(x - pad, y - pad, x + pad, y + pad);
}

/* --------------------------------------------------------- backing store */

/* The flicker, and what it actually was.
 *
 * Solid ink cannot erase itself, so until now the only thing that took a
 * frame of the overlay off again was the screen underneath repainting -
 * which the overlay asked for itself, every time the shape moved. On a
 * list that is a row redraw and cheap. On the WPS it is
 * skin_request_full_update(), which is the whole 480x800 panel torn down
 * and rebuilt: backdrop, album art, every bitmap. Several times a second,
 * racing the repaint the volume change was already causing. That is the
 * flicker, in the dial and in the edge lights both, and every previous
 * attempt at it was a throttle - 60 ms, then 150 ms on skins, then
 * suppressing it mid-dial. A throttle on the wrong mechanism.
 *
 * So stop asking. The compositor (apps/canvas_glue.c) has had exactly the
 * right facility since it was written and nothing had adopted it: save the
 * pixels the overlay is about to cover, put them back before the next
 * frame. The overlay then erases itself, the screen underneath is never
 * asked to repaint on the overlay's account, and there is nothing left to
 * flicker.
 *
 * The regions are kept apart rather than unioned into one rectangle
 * because they are naturally far apart - a ring under the thumb, a readout
 * at the top, a bar down one edge - and their bounding box is most of the
 * panel, which is more memory than the pool has.
 *
 * Slots are claimed on demand, and grown if a style needs more than the
 * last one did, so only the style in use costs anything. If the pool
 * cannot provide them every call here reports failure and the old
 * ask-for-a-repaint path runs unchanged. */

static void dirty_flush(void);

#if defined(HAVE_LCD_LAYERS)
/* The overlay has a layer of its own (firmware/drivers/lcd-layers.c), which
 * is what all of the above was working around. It never touches the
 * framebuffer, so there is nothing to save, nothing to put back, nothing a
 * repaint underneath can make stale, and nothing to ask the screen to
 * repaint for. A capture is only the painter saying where it will draw;
 * the erase is clearing the layer and pushing where it was. */
#include "lcd-layers.h"
#ifdef HAVE_COMPOSITOR
#include "canvas.h"
#include "canvas_glue.h"
#endif

extern struct frame_buffer_t lcd_framebuffer_default;

enum { BD_MAIN, BD_BAND, BD_EDGE_T, BD_EDGE_B, BD_EDGE_L, BD_EDGE_R };

static bool layer_drawing;
/* Set while the Canvas HUD paints: it blends, and a keyed layer has nothing
 * under it to blend with. Its rectangles start as a copy of the screen
 * beneath, so the translucency looks as it always did. The copy is as old
 * as the frame, which is the staleness the old backing store had too, and
 * the HUD repaints on every move. */
static bool layer_snapshot;

static inline bool bd_active(void) { return true; }

static bool bd_capture(int slot, int x, int y, int w, int h)
{
    (void)slot;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return false;

    lcd_layer_mark(LCD_LAYER_OVERLAY, x, y, w, h);

    if (layer_snapshot)
    {
        const size_t stride =
            LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride);
        fb_data *dst = lcd_layer_fb(LCD_LAYER_OVERLAY)->fb_ptr;
        const fb_data *src = lcd_framebuffer_default.fb_ptr;
        int row;
        for (row = 0; row < h; row++)
            memcpy(dst + (size_t)(y + row) * stride + x,
                   src + (size_t)(y + row) * stride + x,
                   w * sizeof(fb_data));
    }
    return true;
}

static void dirty_flush(void);

static void bd_cleared(int x, int y, int w, int h)
{
    dirty_add(x, y, x + w - 1, y + h - 1);
}

static void bd_restore_all(void)
{
    lcd_layer_clear(LCD_LAYER_OVERLAY, bd_cleared);
}

static inline void bd_forget_all(void) { }
static inline void bd_forget_within(int rx, int ry, int rw, int rh)
{ (void)rx; (void)ry; (void)rw; (void)rh; }
static inline void bd_gesture_reset(void) { }

/* A full-screen viewport whose pixels are the layer's. The transparent key
 * is its background, so text drawn solid leaves its box see-through, and
 * the LCD backdrop is set aside while drawing: a solid fill copies from
 * the backdrop at an offset that is only valid inside the framebuffer. */
static struct viewport layer_vp;
static fb_data *layer_saved_backdrop;

static struct viewport *layer_begin(void)
{
    layer_vp.x = 0;
    layer_vp.y = 0;
    layer_vp.width = LCD_WIDTH;
    layer_vp.height = LCD_HEIGHT;
    layer_vp.flags = 0;
    layer_vp.font = FONT_UI;
    layer_vp.drawmode = DRMODE_SOLID;
    layer_vp.buffer = lcd_layer_fb(LCD_LAYER_OVERLAY);
    layer_vp.fg_pattern = global_settings.fg_color;
    layer_vp.bg_pattern = LCD_LAYER_KEY;

    layer_saved_backdrop = lcd_get_backdrop();
    lcd_set_backdrop(NULL);
    layer_drawing = true;
    return lcd_set_viewport_ex(&layer_vp, 0);
}

static void layer_end(struct viewport *oldvp)
{
    lcd_set_viewport_ex(oldvp, 0);
    lcd_set_backdrop(layer_saved_backdrop);
    layer_drawing = false;
}

#elif defined(HAVE_COMPOSITOR)
#include "canvas.h"
#include "canvas_glue.h"

extern struct frame_buffer_t lcd_framebuffer_default;

/* Fixed sizes, claimed once. The pool is a bump allocator by design -
 * releasing an overlay frees the slot and not the bytes - so a slot that
 * grew to fit whatever this frame needed would leak the pool over a few
 * gestures. Each slot is therefore claimed at its worst case for any
 * style, and a capture that does not fit is refused rather than clipped:
 * a partly-saved region puts back a partly-erased overlay, which is worse
 * than the flicker it was meant to cure. */
enum {
    BD_MAIN,        /* the ring and cap, the plate, the HUD panel */
    BD_BAND,        /* the readout pill, across the top          */
    BD_EDGE_T,      /* the four edge lights, which are far apart  */
    BD_EDGE_B,
    BD_EDGE_L,
    BD_EDGE_R,
    BD_COUNT
};

#define BD_MAIN_H   320     /* the HUD panel is 300; the plate 150 */
#define BD_BAND_H    56
#define BD_EDGE_TH   16
#define BD_EDGE_LW   16

static const struct { short w, h; } bd_size[BD_COUNT] =
{
    [BD_MAIN]   = { LCD_WIDTH,   BD_MAIN_H   },
    [BD_BAND]   = { LCD_WIDTH,   BD_BAND_H   },
    [BD_EDGE_T] = { LCD_WIDTH,   BD_EDGE_TH  },
    [BD_EDGE_B] = { LCD_WIDTH,   BD_EDGE_TH  },
    [BD_EDGE_L] = { BD_EDGE_LW,  LCD_HEIGHT  },
    [BD_EDGE_R] = { BD_EDGE_LW,  LCD_HEIGHT  },
};

static struct canvas_overlay *bd_ov[BD_COUNT];
/* What each slot is currently covering. Kept here as well as inside the
 * compositor because the sticky-edge fix has to ask "did the repaint that
 * just happened cover this one?", and only the caller knows the answer. */
static short bd_rx[BD_COUNT], bd_ry[BD_COUNT], bd_rw[BD_COUNT], bd_rh[BD_COUNT];
static bool bd_held[BD_COUNT];
static bool bd_holding;     /* at least one slot is covering something */
static bool bd_off;         /* the pool said no; do not keep asking    */
static bool bd_short;       /* something would not fit this frame      */

static bool bd_slot(int slot)
{
    if (bd_off)
        return false;

    if (!bd_ov[slot])
    {
        /* The claim is what brings the pool up, so this must not be
         * guarded by canvas_available(): before the first claim there is
         * no pool and there never would be one. */
        bd_ov[slot] = canvas_overlay_claim_backing(bd_size[slot].w,
                                                   bd_size[slot].h);
        if (!bd_ov[slot])
        {
            bd_off = true;
            return false;
        }
    }
    return true;
}

/* Save what is about to be covered. Must be called *before* the region is
 * drawn - that is the whole contract, and the reason the painters below
 * report their rectangle first and draw second rather than the other way
 * round as they used to. */
static bool bd_capture(int slot, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return false;

    if (w > bd_size[slot].w || h > bd_size[slot].h || !bd_slot(slot) ||
        !canvas_overlay_capture(bd_ov[slot], x, y, w, h))
    {
        bd_short = true;
        return false;
    }

    bd_rx[slot] = (short)x;
    bd_ry[slot] = (short)y;
    bd_rw[slot] = (short)w;
    bd_rh[slot] = (short)h;
    bd_held[slot] = true;
    bd_holding = true;
    return true;
}

/* Whether the backing store is carrying this gesture. False means every
 * caller keeps the old ask-the-screen-to-repaint path, which still works
 * and still flickers. bd_short latches for the life of the gesture: a
 * single frame that could not be saved leaves ink nothing will take off,
 * and only a repaint can clear that. */
static bool bd_active(void)
{
    return !bd_off && !bd_short;
}

/* Put every slot back and push the result. This is the erase. */
static void bd_restore_all(void)
{
    int i;
    if (!bd_holding)
        return;
    for (i = 0; i < BD_COUNT; i++)
        if (bd_ov[i] && bd_held[i])
        {
            canvas_overlay_restore(bd_ov[i]);
            /* What was put back has to be pushed, and only the caller
             * knows the rectangle: canvas_overlay_restore() writes into
             * the framebuffer and adds to the compositor's pending box,
             * and nothing here was calling canvas_present(). On a list
             * that went unnoticed because the list's own repaint pushed
             * the same pixels a moment later. On a skin nothing repaints
             * on its own, so the last frame of the overlay stayed lit on
             * the panel while the framebuffer underneath was already
             * correct - which is the edge lights that would not clear. */
            dirty_add(bd_rx[i], bd_ry[i],
                      bd_rx[i] + bd_rw[i] - 1, bd_ry[i] + bd_rh[i] - 1);
            bd_held[i] = false;
        }
    canvas_present();
    bd_holding = false;
}

/* The screen underneath repainted on its own account, so what we saved is
 * a frame out of date. Drop it rather than putting it back over content
 * that is newer than it is. */
static void bd_forget_all(void)
{
    int i;
    for (i = 0; i < BD_COUNT; i++)
        if (bd_ov[i])
        {
            canvas_overlay_invalidate(bd_ov[i]);
            bd_held[i] = false;
        }
    bd_holding = false;
}

/* Only the slots the given rectangle actually covered.
 *
 * A list repaint is not a screen repaint: it puts its rows back inside its
 * own viewport and touches nothing else. Forgetting every slot after one
 * meant the edge lights - which live at the rim, outside any list viewport
 * - had their saved pixels dropped while their ink stayed on the panel,
 * and nothing in the system would ever paint over it again. That is the
 * bar that stuck to the top and bottom of the screen during a scroll.
 *
 * So: slots the repaint covered are stale and are dropped; slots it did
 * not touch are still good and stay, to be put back on the next frame. */
static void bd_forget_within(int rx, int ry, int rw, int rh)
{
    int i;
    bool any = false;

    for (i = 0; i < BD_COUNT; i++)
    {
        if (!bd_ov[i] || !bd_held[i])
            continue;

        if (bd_rx[i] < rx + rw && bd_rx[i] + bd_rw[i] > rx &&
            bd_ry[i] < ry + rh && bd_ry[i] + bd_rh[i] > ry)
        {
            canvas_overlay_invalidate(bd_ov[i]);
            bd_held[i] = false;
        }
        else
            any = true;
    }
    bd_holding = any;
}

/* A gesture is over: whatever went wrong during it should not condemn the
 * next one. */
static void bd_gesture_reset(void)
{
    bd_short = false;
}
#endif /* HAVE_LCD_LAYERS / HAVE_COMPOSITOR */

#if !defined(HAVE_LCD_LAYERS) && !defined(HAVE_COMPOSITOR)
#define BD_MAIN   0
#define BD_BAND   1
#define BD_EDGE_T 2
#define BD_EDGE_B 3
#define BD_EDGE_L 4
#define BD_EDGE_R 5
static inline bool bd_active(void) { return false; }
static inline bool bd_capture(int slot, int x, int y, int w, int h)
{ (void)slot; (void)x; (void)y; (void)w; (void)h; return false; }
static inline void bd_restore_all(void) { }
static inline void bd_forget_all(void) { }
static inline void bd_forget_within(int rx, int ry, int rw, int rh)
{ (void)rx; (void)ry; (void)rw; (void)rh; }
static inline void bd_gesture_reset(void) { }
#endif /* HAVE_COMPOSITOR */

/* The plate: the study's 1c/2a "permanent control at the bottom" - a groove
 * with a ring in it and a cap that actually moves. Anchored to the panel
 * rather than to the thumb, so it is a place on the device instead of a
 * thing that appears wherever you happened to touch.
 *
 * It is still only drawn while a gesture is live. A plate that is there at
 * rest needs a hook into the list's own drawing, and there is no such hook
 * to call - see the note above STICK_OVERLAY_* in settings.h. */
#define PLATE_H   150
#define PLATE_PAD  14

static void paint_plate(const struct overlay_shape *s)
{
    struct overlay_shape p = *s;
    int cx = LCD_WIDTH / 2;
    int cy = LCD_HEIGHT - PLATE_H / 2;
    int rad = (PLATE_H / 2) - PLATE_PAD;
    int reach = s->R > 0 ? s->R : 1;
    int capx, capy;

    /* The cap goes where the thumb has pushed to, scaled so that full
     * travel puts it exactly on the ring. */
    capx = cx + ((s->x - s->ox) * rad) / reach;
    capy = cy + ((s->y - s->oy) * rad) / reach;

    if (capx < cx - rad) capx = cx - rad;
    if (capx > cx + rad) capx = cx + rad;
    if (capy < cy - rad) capy = cy - rad;
    if (capy > cy + rad) capy = cy + rad;

    /* Saved before a single pixel of it is drawn - see bd_capture(). */
    bd_capture(BD_MAIN, 0, LCD_HEIGHT - PLATE_H, LCD_WIDTH, PLATE_H);
    dirty_add(0, LCD_HEIGHT - PLATE_H, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    /* The groove the plate sits in. */
    lcd_drawrect(PLATE_PAD / 2, LCD_HEIGHT - PLATE_H + PLATE_PAD / 2,
                 LCD_WIDTH - PLATE_PAD, PLATE_H - PLATE_PAD);

    p.ox = cx;
    p.oy = cy;
    rose_ring(&p, rad);
    rose_ring(&p, rad / 4);

    lcd_drawline(cx, cy, capx, capy);
    lcd_fillrect(capx - 7, capy - 7, 15, 15);
}

/* 2c: no widget at all. The screen edge nearest the direction of travel
 * lights in proportion to the rate, and the content carries the rest. */
static void paint_edges(const struct overlay_shape *s)
{
    int dx = s->x - s->ox;
    int dy = s->y - s->oy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    /* Four pixels was a hairline on a 480x800 panel held at arm's length
     * - visible in a screenshot, easy to miss on the device. Twelve reads
     * as a deliberate bar without becoming a second UI element. */
    int thick = 12;

    if (s->lit <= 0)
        return;

    if (ady >= adx)
    {
        int w = (LCD_WIDTH * s->lit) / 255;
        if (w < 16)
            w = 16;
        int y = dy < 0 ? 0 : LCD_HEIGHT - thick;
        /* Both edge slots, and always the same slot for the same edge:
         * a bar that grows and shrinks on every frame must have its own
         * saved strip or the previous length is never put back. */
        bd_capture(dy < 0 ? BD_EDGE_T : BD_EDGE_B,
                   (LCD_WIDTH - w) / 2, y, w, thick);
        dirty_add((LCD_WIDTH - w) / 2, y,
                  (LCD_WIDTH - w) / 2 + w, y + thick);
        lcd_fillrect((LCD_WIDTH - w) / 2, y, w, thick);
    }
    else
    {
        int len = (LCD_HEIGHT * s->lit) / 255;
        if (len < 16)
            len = 16;
        int x = dx < 0 ? 0 : LCD_WIDTH - thick;
        bd_capture(dx < 0 ? BD_EDGE_L : BD_EDGE_R,
                   x, (LCD_HEIGHT - len) / 2, thick, len);
        dirty_add(x, (LCD_HEIGHT - len) / 2,
                  x + thick, (LCD_HEIGHT - len) / 2 + len);
        lcd_fillrect(x, (LCD_HEIGHT - len) / 2, thick, len);
    }
}

#ifdef HAVE_COMPOSITOR
/* The openness study's coach HUD, drawn to the study's own geometry.
 *
 * The numbers below are the prototype's, not invented here: a 300 px panel
 * centred on the press, rings at 14, 40 and 70 px radius, a 104 px cap that
 * travels 70 px from the centre, a readout pill 42 px down, and 6x120 px
 * edge bars at half height. The engine's own travel radius is mapped onto
 * that 70 px rather than the picture being scaled to the engine, so the HUD
 * looks the same whatever detent and travel the user has set - which is
 * what "follow the design" has to mean when the design was drawn once, at
 * one size.
 *
 * Colours come from the theme: ink is the foreground, ground the
 * background, accent the line selector colour. A .cfg theme can only set
 * settings, so those three fields are the whole channel it has to the
 * stick's looks (see settings.h) - which is why the "Canvas - " themes
 * carry the study's palette in them.
 *
 * It draws straight at the framebuffer, like every other style, but the
 * pixels it is about to cover are saved first through the compositor
 * (apps/canvas_glue.c) and put back before the next frame. That is what
 * lets the cap carry a caption at all: solid ink cannot erase itself, and
 * a word that rides a moving thumb smeared into "sselect" without it. */
/* The study's measurements. */
#define HUD_PANEL_R   150
#define HUD_RING_DEAD  14
#define HUD_RING_FINE  40
#define HUD_RING_RATE  70
#define HUD_CAP_R      52
#define HUD_TRAVEL     70      /* full deflection, in HUD pixels */
#define HUD_PILL_TOP   42
#define HUD_EDGE_W      6
#define HUD_EDGE_H    120

static void canvas_screen(struct canvas_surface *fb)
{
#ifdef HAVE_LCD_LAYERS
    /* Into the layer. What the HUD blends against there is the layer's
     * near-black key rather than the screen, so its translucency reads as
     * a dark panel - the same thing on a dark theme. */
    if (layer_drawing)
    {
        canvas_surface_init(fb, lcd_layer_fb(LCD_LAYER_OVERLAY)->fb_ptr, NULL,
                            LCD_WIDTH, LCD_HEIGHT,
                            (int)LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride));
        return;
    }
#endif
    canvas_surface_init(fb, lcd_framebuffer_default.fb_ptr, NULL,
                        LCD_WIDTH, LCD_HEIGHT,
                        (int)LCD_NATIVE_STRIDE(lcd_framebuffer_default.stride));
}

static struct canvas_rect disc_rect(int cx, int cy, int rad)
{
    struct canvas_rect r = { cx - rad, cy - rad, 2 * rad, 2 * rad };
    return r;
}

/* A colour mixed towards another one, for the shades the study names but a
 * theme has no field for: the panel fill, the rule, the dimmed ink. */
static canvas_px mix(canvas_px a, canvas_px b, unsigned amount)
{
    return canvas_blend_px(a, b, amount);
}

static void hud_text(int x, int y, const char *s, canvas_px colour)
{
    lcd_set_foreground(colour);
    lcd_putsxy(x, y, s);
}

static void hud_text_centred(int cx, int y, const char *s, canvas_px colour)
{
    int w;
    lcd_getstringsize(s, &w, NULL);
    hud_text(cx - w / 2, y, s, colour);
}

static void paint_canvas(const struct overlay_shape *s, bool minimal)
{
    struct canvas_surface fb;
    canvas_px ink    = (canvas_px)global_settings.fg_color;
    canvas_px ground = (canvas_px)global_settings.bg_color;
    canvas_px accent = (canvas_px)global_settings.lss_color;
    /* The study's hudLine / hudInk / capIdle, derived rather than themed. */
    canvas_px rule    = mix(ground, ink, 90);
    canvas_px dim_ink = mix(ground, ink, 200);
    canvas_px cap_idle = mix(ground, ink, 30);
    bool armed = s->mode != STICK_BIND_NONE;
    bool sideways;
    struct canvas_rect r;
    char line[32];
    int cx, cy, nx, ny, reach, dx, dy;
    int tw, th;

    if (!bd_active())
        return;

    canvas_screen(&fb);
    lcd_setfont(FONT_SYSFIXED);

    /* Where the panel sits: on the press, pulled inside the screen so it
     * is never half off the edge. The study does exactly this. */
    cx = s->ox;
    cy = s->oy;
    if (cx < HUD_PANEL_R) cx = HUD_PANEL_R;
    if (cy < HUD_PANEL_R) cy = HUD_PANEL_R;
    if (cx > LCD_WIDTH - HUD_PANEL_R)  cx = LCD_WIDTH - HUD_PANEL_R;
    if (cy > LCD_HEIGHT - HUD_PANEL_R) cy = LCD_HEIGHT - HUD_PANEL_R;

    /* Deflection, mapped from the engine's travel onto the study's 70 px. */
    reach = s->R > 0 ? s->R : 1;
    dx = s->x - s->ox;
    dy = s->y - s->oy;
    nx = (dx * HUD_TRAVEL) / reach;
    ny = (dy * HUD_TRAVEL) / reach;
    if (nx >  HUD_TRAVEL) nx =  HUD_TRAVEL;
    if (nx < -HUD_TRAVEL) nx = -HUD_TRAVEL;
    if (ny >  HUD_TRAVEL) ny =  HUD_TRAVEL;
    if (ny < -HUD_TRAVEL) ny = -HUD_TRAVEL;

    sideways = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy);

    /* ---- the panel ------------------------------------------------- */
    bd_capture(BD_MAIN, cx - HUD_PANEL_R, cy - HUD_PANEL_R,
                           2 * HUD_PANEL_R, 2 * HUD_PANEL_R);

    if (!minimal)
    {
        r = disc_rect(cx, cy, HUD_PANEL_R);
        canvas_fill_round_rect(&fb, &r, HUD_PANEL_R, ground, 228);
        canvas_stroke_round_rect(&fb, &r, HUD_PANEL_R, 1, rule, 255);
    }

    /* ---- the three rings ------------------------------------------- */
    r = disc_rect(cx, cy, HUD_RING_RATE);
    canvas_stroke_round_rect(&fb, &r, HUD_RING_RATE, 1,
                             armed && !s->dial ? accent : rule, 255);

    r = disc_rect(cx, cy, HUD_RING_FINE);
    canvas_stroke_round_rect(&fb, &r, HUD_RING_FINE, 1, rule, 200);

    r = disc_rect(cx, cy, HUD_RING_DEAD);
    canvas_stroke_round_rect(&fb, &r, HUD_RING_DEAD, 1, rule, 255);

    /* ---- what each direction does, around the edge ----------------- */
    /* The study's coaching layer, and the one thing its middle level
     * takes away: once the gesture is known, the labels are furniture. */
    if (!minimal)
    {
        const char *up    = stick_binding_name(live_cfg.bind[0]);
        const char *right = stick_binding_name(live_cfg.bind[1]);
        const char *down  = stick_binding_name(live_cfg.bind[2]);
        const char *left  = stick_binding_name(live_cfg.bind[3]);

        lcd_getstringsize("X", &tw, &th);

        hud_text_centred(cx, cy - HUD_PANEL_R + 12, up, dim_ink);
        hud_text_centred(cx, cy + HUD_PANEL_R - 12 - th, down, dim_ink);

        lcd_getstringsize(left, &tw, &th);
        hud_text(cx - HUD_PANEL_R + 14, cy - th / 2, left, dim_ink);

        lcd_getstringsize(right, &tw, &th);
        hud_text(cx + HUD_PANEL_R - 14 - tw, cy - th / 2, right, dim_ink);
    }

    /* ---- the cap --------------------------------------------------- */
    r = disc_rect(cx + nx, cy + ny, HUD_CAP_R);
    canvas_shadow(&fb, &r, HUD_CAP_R, 8, 0, 4, 0 /* black */, 120);
    canvas_fill_round_rect(&fb, &r, HUD_CAP_R, armed ? accent : cap_idle, 255);
    canvas_stroke_round_rect(&fb, &r, HUD_CAP_R, 1, rule, 255);

    /* The word for what the gesture means, and the rate under it - the
     * study's two lines, in the cap, where the thumb is. */
    {
        canvas_px cap_fg  = armed ? ground : dim_ink;
        canvas_px cap_sub = armed ? mix(accent, ground, 170) : rule;

        snprintf(line, sizeof(line), "%s",
                 s->dial ? "VOLUME" : armed ? stick_binding_name(s->mode)
                                            : "HOLD");
        lcd_getstringsize(line, &tw, &th);
        hud_text(cx + nx - tw / 2, cy + ny - th, line, cap_fg);

        if (s->dial)
            snprintf(line, sizeof(line), "%d deg", s->dial_deg);
        else if (armed && s->rate > 0)
            snprintf(line, sizeof(line), "%d/s", s->rate);
        else
            line[0] = '\0';

        if (line[0])
        {
            lcd_getstringsize(line, &tw, &th);
            hud_text(cx + nx - tw / 2, cy + ny + 2, line, cap_sub);
        }
    }

    /* ---- the readout pill ------------------------------------------ */
    snprintf(line, sizeof(line), " %s   %d/s ",
             s->dial ? "volume" : stick_binding_name(s->mode), s->rate);
    lcd_getstringsize(line, &tw, &th);

    r.w = tw + 16;
    r.h = th + 12;
    r.x = (LCD_WIDTH - r.w) / 2;
    r.y = HUD_PILL_TOP;

    bd_capture(BD_BAND, r.x - 2, r.y - 2, r.w + 4, r.h + 4);
    canvas_fill_round_rect(&fb, &r, 3, ground, 228);
    canvas_stroke_round_rect(&fb, &r, 3, 1, rule, 255);
    hud_text_centred(LCD_WIDTH / 2, r.y + 6, line, dim_ink);

    /* ---- the edge bars --------------------------------------------- */
    {
        int ey = (LCD_HEIGHT - HUD_EDGE_H) / 2;
        int side;

        for (side = 0; side < 2; side++)
        {
            int ex = side ? LCD_WIDTH - HUD_EDGE_W : 0;
            bool lit = sideways && armed &&
                       ((side && dx > 0) || (!side && dx < 0));

            /* An unlit bar is simply not captured this frame: the
             * restore at the top of overlay_paint() has already put back
             * whatever it covered last time. */
            if (!lit)
                continue;

            bd_capture(side ? BD_EDGE_R : BD_EDGE_L, ex, ey,
                       HUD_EDGE_W, HUD_EDGE_H);
            r.x = ex; r.y = ey; r.w = HUD_EDGE_W; r.h = HUD_EDGE_H;
            canvas_fill_round_rect(&fb, &r, 2, accent, 235);
        }
    }

    lcd_setfont(FONT_UI);

    /* The compositor knows every rectangle that changed, so the panel
     * update is its job rather than dirty_add()'s. */
    canvas_present();
}
#endif /* HAVE_COMPOSITOR */

/* 2b's floor, and the study is explicit that it is a floor: the mode and
 * the rate, in words, while the stick is engaged. */
static void paint_readout(const struct overlay_shape *s)
{
    char line[32];
    int w, h;

    snprintf(line, sizeof(line), " %s  %d/s ",
             stick_binding_name(s->mode), s->rate);

    lcd_setfont(FONT_SYSFIXED);
    lcd_getstringsize(line, &w, &h);

    /* The pill's width tracks the rate, so it is a different rectangle
     * every frame; a band the full width of the panel is saved instead,
     * which puts back the frame before it whatever size that one was. */
    bd_capture(BD_BAND, 0, 2, LCD_WIDTH, h + 10);
    dirty_add((LCD_WIDTH - w) / 2 - 4, 2,
              (LCD_WIDTH + w) / 2 + 4, h + 10);

    lcd_putsxy((LCD_WIDTH - w) / 2, 6, line);
    lcd_drawrect((LCD_WIDTH - w) / 2 - 3, 3, w + 6, h + 6);
    lcd_setfont(FONT_UI);
}

static void overlay_paint(const struct overlay_shape *s)
{
    int x0, y0, x1, y1, i;
    int w, h;
    struct viewport *oldvp;

    dirty_reset();

    /* Put the previous frame's pixels back before anything else happens.
     * This is the erase, and it is why nothing below asks the screen
     * underneath to repaint any more. */
    bd_restore_all();

#ifdef HAVE_LCD_LAYERS
    oldvp = layer_begin();
#else

    /* Paint into the default, full-screen viewport. The overlay's
     * coordinates are absolute, and whoever called us may well have a
     * viewport of their own set - the list does.
     *
     * Marking the default viewport dirty on the way out is the half that
     * matters. The skin engine only clears the whole screen on a full
     * update when that flag is set (skin_render.c), and drawing straight
     * at the LCD never set it. So ink that landed outside every viewport
     * the skin declares had nothing that would ever repaint over it: the
     * overlay simply stayed. That is the stuck volume ring, and the stuck
     * edge light. */
    /* The dirty flag is only wanted when there is no backing store. It
     * exists so the skin engine's next full update clears the whole
     * screen and paints over ink that landed outside every viewport the
     * skin declares - the stuck volume ring, the stuck edge light. With a
     * backing store that ink is taken back off by the overlay itself, and
     * forcing a whole-screen clear buys nothing but a flash. */
    oldvp = lcd_set_viewport_ex(NULL, bd_active() ? 0 : VP_FLAG_VP_DIRTY);
#endif

    /* Solid, in the theme's foreground colour, not DRMODE_COMPLEMENT.
     * Inverting the pixels underneath is self-erasing, which is why it was
     * the first choice - but a theme whose background sits near mid-grey
     * inverts to another mid-grey, and the whole overlay became invisible.
     * Nothing is erased here: the list repaints its rows and calls us back
     * afterwards, so every frame is drawn over fresh content. */
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(global_settings.fg_color);

    /* An armed dial looks nothing like a stick gesture and has to say so:
     * the thumb is about to mean rotation, and there is no deflection or
     * sector to read. A thick ring with a marker running round it, and the
     * word for what it controls in the middle. */
    if (s->dial)
    {
        int mx, my;

        /* On a skin the dial has no picture of its own. %sd tells the skin
         * the dial is armed and the skin styles its own volume bar, which
         * is both where the user is looking and a place that holds still -
         * a ring drawn round a thumb going in circles was a second thing
         * to watch, over the top of the one that mattered. */
        if (s->on_skin)
            goto flush;

        bd_capture(BD_MAIN, s->ox - s->R - 8, s->oy - s->R - 8,
                   2 * (s->R + 8), 2 * (s->R + 8));
        dirty_point(s->ox, s->oy, s->R + 4);

        rose_ring(s, s->R);
        rose_ring(s, s->R - 3);
        rose_ring(s, s->r);

        rose_point_deg(s, s->dial_deg, (s->R + s->r) / 2, &mx, &my);
        lcd_fillrect(mx - 6, my - 6, 13, 13);

        lcd_getstringsize("VOLUME", &w, &h);
        lcd_putsxy(s->ox - w / 2, s->oy - h / 2, "VOLUME");
        goto flush;
    }

    switch (s->style)
    {
    case STICK_OVERLAY_PLATE:
        paint_plate(s);
        break;

    case STICK_OVERLAY_EDGES:
        paint_edges(s);
        break;

#ifdef HAVE_COMPOSITOR
    case STICK_OVERLAY_CANVAS:
    case STICK_OVERLAY_CANVAS_MIN:
#ifdef HAVE_LCD_LAYERS
        layer_snapshot = true;
#endif
        paint_canvas(s, s->style == STICK_OVERLAY_CANVAS_MIN);
#ifdef HAVE_LCD_LAYERS
        layer_snapshot = false;
#endif
        break;
#endif

    case STICK_OVERLAY_READOUT:
    case STICK_OVERLAY_ROSE:
    default:
    {
        /* One rectangle for the ring and the knob together: the knob is
         * pinned inside travel, so the ring's box already contains it,
         * and two overlapping captures would save each other's ink. */
        int bx = s->ox - s->R - 8, by = s->oy - s->R - 8;
        int bs = 2 * (s->R + 8);
        if (s->x - 8 < bx) { bs += bx - (s->x - 8); bx = s->x - 8; }
        if (s->y - 8 < by) { bs += by - (s->y - 8); by = s->y - 8; }
        if (s->x + 8 > bx + bs) bs = s->x + 8 - bx;
        if (s->y + 8 > by + bs) bs = s->y + 8 - by;
        bd_capture(BD_MAIN, bx, by, bs, bs);
        dirty_add(bx, by, bx + bs, by + bs);
    }

        /* The travel ring: deflection reaches full at its edge, so it is
         * also "as far as pushing gets you". */
        rose_ring(s, s->R);
        /* The dead spot in the middle, where no direction is chosen. */
        rose_ring(s, s->r);

        if (s->sectors > 1)
        {
            /* Spokes on the sector boundaries, not the sector centres:
             * what the thumb needs to know is where one direction stops
             * being the next. */
            int half = 360 / (2 * s->sectors);
            for (i = 0; i < s->sectors; i++)
            {
                int deg = s->rotation + half + (i * 360) / s->sectors;
                rose_point_deg(s, deg, s->r, &x0, &y0);
                rose_point_deg(s, deg, s->R, &x1, &y1);
                lcd_drawline(x0, y0, x1, y1);
            }
        }

        lcd_drawline(s->ox, s->oy, s->x, s->y);
        lcd_fillrect(s->x - 5, s->y - 5, 11, 11);

        if (s->style == STICK_OVERLAY_READOUT)
            paint_readout(s);
        break;
    }

flush:
#ifdef HAVE_LCD_LAYERS
    /* Back on the framebuffer before the push: the update composes the
     * layer over it. */
    layer_end(oldvp);
    dirty_flush();
#else
    dirty_flush();

    /* Restores the caller's viewport and, in doing so, leaves the default
     * one flagged dirty - see the note at the top. */
    lcd_set_viewport_ex(oldvp, bd_active() ? 0 : VP_FLAG_VP_DIRTY);
#endif
}

/* Ask whatever is underneath to put itself back. GUI_EVENT_NEED_UI_UPDATE
 * reaches the lists; the WPS does not listen to it and will not repaint on
 * its own while nothing about the track has changed, which is why an
 * overlay drawn over a skin used to sit there until the track did
 * something. The skin engine has its own way of being told. */
static bool on_a_skin(void)
{
    return (cached_context & 0xff) == CONTEXT_WPS;
}

/* Edge detection for the dial, so %sd reaches the screen. */
static bool dial_was_armed;

static long repaint_interval_ticks(void)
{
    return on_a_skin() ? MS_TO_TICKS(OVERLAY_REPAINT_SKIN_MS)
                       : MS_TO_TICKS(OVERLAY_REPAINT_MS);
}

static void request_repaint(void)
{
    send_event(GUI_EVENT_NEED_UI_UPDATE, NULL);
    /* Only where there is a skin to repaint. Asking for a full WPS update
     * from a list is a whole screen rebuilt for nothing. */
    if (on_a_skin())
        skin_request_full_update(WPS);
}

/* There is no erase any more. Solid ink cannot undo itself the way an XOR
 * could, so clearing the overlay means asking the screen to repaint and
 * letting the list's callback decline to draw anything the next time
 * round. */
/* Push whatever has been added to the dirty box since the last reset. */
static void dirty_flush(void)
{
    int w, h;

    if (dirty_x0 < 0)          dirty_x0 = 0;
    if (dirty_y0 < 0)          dirty_y0 = 0;
    if (dirty_x1 > LCD_WIDTH - 1)  dirty_x1 = LCD_WIDTH - 1;
    if (dirty_y1 > LCD_HEIGHT - 1) dirty_y1 = LCD_HEIGHT - 1;

    w = dirty_x1 - dirty_x0 + 1;
    h = dirty_y1 - dirty_y0 + 1;
    if (w > 0 && h > 0)
        lcd_update_rect(dirty_x0, dirty_y0, w, h);
}

static void overlay_clear(void)
{
    if (!overlay.drawn && !overlay.repair_owed)
        return;
    overlay.drawn = false;
    overlay.repair_owed = false;

    /* The backing store saved what the overlay covered, so it takes
     * itself off without asking the screen underneath to repaint. That is
     * the whole point: no full skin update, so no flash. Every style, not
     * only the Canvas HUD - the flicker was never style-specific. */
    if (bd_active())
    {
        dirty_reset();
        bd_restore_all();
        dirty_flush();
        bd_gesture_reset();
        return;
    }

    /* Either there is no compositor, or a frame this gesture could not
     * save left ink behind. Only a repaint clears that. */
    dirty_reset();
    bd_restore_all();
    dirty_flush();
    bd_gesture_reset();
    request_repaint();
}

/* Records where the overlay should be. The drawing itself happens later,
 * from stick_draw_overlay(), because anything drawn from here is painted
 * over the moment the screen underneath redraws in response to whatever
 * the gesture just fired. */
static void overlay_update(const struct stick_state *st)
{
    int phase = stick_phase(st);

    overlay.want = global_settings.stick_overlay &&
                   (phase == STICK_PHASE_ARMED || phase == STICK_PHASE_DIAL);

    if (!overlay.want)
        return;

    overlay.want_s.ox = st->ox;
    overlay.want_s.oy = st->oy;
    overlay.want_s.x  = st->x;
    overlay.want_s.y  = st->y;
    overlay.want_s.r  = live_cfg.detent_px > 0 ? live_cfg.detent_px
                                               : STICK_DEF_DETENT_PX;
    overlay.want_s.R  = live_cfg.travel_px > 0 ? live_cfg.travel_px
                                               : STICK_DEF_TRAVEL_PX;
    overlay.want_s.sectors  = live_cfg.sectors;
    overlay.want_s.rotation = live_cfg.rotation;
    overlay.want_s.style    = global_settings.stick_overlay_style;

    /* What the gesture currently means, and how fast it is going. Both are
     * read straight off the engine rather than guessed at from pixels. */
    {
        int sector = stick_sector(st);
        /* The accelerated step, not the baseline: the rate printed has to
         * be the rate the engine is actually running at. */
        int step = stick_effective_scroll_px(st);
        int32_t v = st->vel_px_s < 0 ? -st->vel_px_s : st->vel_px_s;

        overlay.want_s.mode = (sector >= 0) ? live_cfg.bind[sector]
                                            : STICK_BIND_NONE;
        overlay.want_s.rate = (int)(v / step);
        overlay.want_s.lit  = stick_deflection(st);

        /* An armed dial gets its own picture: the sector and the
         * deflection mean nothing once the gesture is rotation. */
        overlay.want_s.dial = (phase == STICK_PHASE_DIAL);
        overlay.want_s.on_skin = on_a_skin();

        /* The moment the dial arms or disarms, ask the skin to repaint.
         * A skin with no peak meter and no %an redraws only when something
         * happens to the track, and this is a change the skin is now
         * responsible for showing - %sd is no use if nothing asks for the
         * frame that would show it. Edge-triggered: mid-dial the volume is
         * changing anyway and that repaints on its own. */
        if (overlay.want_s.dial != dial_was_armed)
        {
            dial_was_armed = overlay.want_s.dial;
            request_repaint();
        }
        overlay.want_s.dial_deg =
            (int)(stick_atan2_deg64(st->x - st->cx, st->y - st->cy) / 64);
    }
}

bool stick_dial_armed(void)
{
#ifdef STICK_HAVE_KEYMAP
    return stick_phase(&live_state) == STICK_PHASE_DIAL;
#else
    return false;
#endif
}

void stick_redraw_overlay(int rx, int ry, int rw, int rh)
{
#ifdef HAVE_LCD_LAYERS
    /* The list's own update already put the layer over its new rows. A
     * shape that has not moved needs nothing more - unless it is the
     * Canvas HUD, whose layer holds a copy of the rows it covers. */
    if (overlay.want && overlay.drawn &&
        overlay.have_s.style != STICK_OVERLAY_CANVAS &&
        overlay.have_s.style != STICK_OVERLAY_CANVAS_MIN &&
        !memcmp(&overlay.have_s, &overlay.want_s, sizeof(overlay.have_s)))
        return;
#endif
    /* Called from the list once it has finished putting its rows down.
     * Whatever we had drawn is underneath those rows now, so the shape is
     * gone: forget it without trying to XOR it off (that would scribble on
     * the new content) and paint a fresh one on top. */
    overlay.drawn = false;
    overlay.repair_owed = false;

    /* Only what the list actually repainted is a frame out of date. See
     * bd_forget_within(): forgetting the rest is what made the edge lights
     * stick to the screen during a scroll. */
    bd_forget_within(rx, ry, rw, rh);

    if (!overlay.want)
        return;

    overlay.have_s = overlay.want_s;
    overlay.drawn = true;
    overlay_paint(&overlay.have_s);
}

void stick_draw_overlay(void)
{
    bool moved;

    if (!overlay.want)
    {
        overlay_clear();
        return;
    }

    moved = !overlay.drawn ||
            memcmp(&overlay.have_s, &overlay.want_s,
                   sizeof(overlay.have_s)) != 0;

    /* A parked thumb can still lose part of its overlay: the screen
     * underneath goes on repainting - a sustained scroll changes the
     * selected row several times a second - and each row paints over
     * whatever part of the shape it covers, with nothing to put it back
     * because the shape has not moved. Repainting on a timer from here
     * was tried and is not the fix: this function only runs when the
     * action loop polls, and a parked thumb produces no events to poll.
     * The fix is the compositor's backing store (apps/canvas_glue.c),
     * which needs a device to confirm. */
    if (!moved)
        return;

    /* With a backing store there is nothing to ask for: overlay_paint()
     * puts the last frame's pixels back before it draws the next one, so
     * the shape erases itself and the screen underneath is never disturbed
     * on the overlay's account. That is the whole of the flicker fix - see
     * the note above bd_capture().
     *
     * Without one, solid ink cannot erase itself, so a shape that moves
     * leaves the old one behind and the only thing that takes it off is
     * the screen repainting. The list only repaints when the gesture fires
     * something; a slow drag or a hold travels less than one scroll step,
     * fires nothing, and every frame adds ink until the overlay smears and
     * sticks. So the repaint is asked for whenever the shape moves, and
     * throttled, because one per motion event is more than the screen is
     * worth. */
    if (!bd_active() && overlay.drawn &&
        TIME_AFTER(current_tick, overlay.last_repaint + repaint_interval_ticks()))
    {
        overlay.last_repaint = current_tick;
        overlay.drawn = false;
        overlay.repair_owed = false;
        request_repaint();
        /* Fall through and paint anyway. A screen that ignores the event
         * still gets a live overlay; one that honours it repaints and
         * stick_redraw_overlay() puts this shape down again on top. */
    }

    overlay.have_s = overlay.want_s;
    overlay.drawn = true;

    overlay_paint(&overlay.have_s);
}

/* Called when the gesture has just produced a button: the screen is about
 * to repaint itself over whatever we drew. Forget the shape so the next
 * draw puts a fresh one down, and remember that a repaint is owed if the
 * gesture ends before one arrives. */
static void overlay_invalidate(void)
{
    if (overlay.drawn)
        overlay.repair_owed = true;
    overlay.drawn = false;

    /* A repaint is coming, so the saved backdrop is about to be wrong -
     * but only where that repaint lands. A skin update is the whole panel,
     * so everything goes; a list repaints its own viewport and
     * stick_redraw_overlay() is called afterwards with the rectangle, so
     * leave that case to the side that knows it. */
    if (on_a_skin())
        bd_forget_all();
}

/* ------------------------------------------------------------------- cues */

/* The slow tap that arms the volume dial changes what the whole panel
 * means, which is more than a click is worth saying. The rising chirp is
 * the same one an unlock gets: something just opened. */
#define DIAL_ARM_NOTES 4

static void note_dial_arming(void)
{
    static int played;
    int phase = stick_phase(&live_state);
    long arm, held;
    int want;

    /* Nothing to say once the dial is up, or once the gesture has become
     * something else. */
    if (phase == STICK_PHASE_IDLE || live_state.dial ||
        !live_cfg.dial_on_hold || live_cfg.dial == STICK_DIAL_OFF ||
        (phase != STICK_PHASE_ARMING && phase != STICK_PHASE_ARMED))
    {
        played = 0;
        return;
    }

    /* The slow tap only counts while the thumb stays inside the detent
     * radius; travel means the user is aiming a sector instead, and the
     * run has to stop rather than finish. */
    if (stick_hypot(live_state.x - live_state.ox,
                    live_state.y - live_state.oy) >= live_cfg.detent_px)
    {
        played = 0;
        return;
    }

    arm = live_cfg.dial_arm_ms > 0 ? live_cfg.dial_arm_ms
                                   : STICK_DEF_DIAL_ARM_MS;
    held = now_ms() - live_state.t0;

    /* A rising run over the length of the hold, last note landing as the
     * dial arms. The first note is a quarter of the way in, so an ordinary
     * tap makes no sound at all. */
    want = (int)((held * DIAL_ARM_NOTES) / arm);
    if (want > DIAL_ARM_NOTES)
        want = DIAL_ARM_NOTES;

    while (played < want)
    {
        if (keyclick_enabled(KEYCLICK_SRC_STICK_ARMING))
            rpkeys_chirp_step(played, DIAL_ARM_NOTES, true);
        played++;
    }
}

static enum keyclick_source cue_source(int cue)
{
    switch (cue)
    {
    case STICK_CUE_SECTOR:
        return KEYCLICK_SRC_STICK_ARMING;
    case STICK_CUE_ARM:
        return KEYCLICK_SRC_STICK_ARMED;
    case STICK_CUE_DETENT:
        /* The same cue marks a dial detent and a scroll step; the phase
         * says which. */
        return live_state.phase == STICK_PHASE_DIAL
            ? KEYCLICK_SRC_STICK_DIAL : KEYCLICK_SRC_STICK_SCROLL;
    case STICK_CUE_COMMIT:
    case STICK_CUE_REJECT:
    default:
        return KEYCLICK_SRC_STICK_ACTION;
    }
}

static void play_cue(int cue)
{
    if (cue == STICK_CUE_NONE)
        return;
    if (!global_settings.keyclick)
        return;
    if (!keyclick_enabled(cue_source(cue)))
        return;

    /* One short click for every cue point is the default (spec 7). The
     * mixer drops a click rather than queueing it, so this can never add
     * latency to the gesture. */
    system_sound_play(SOUND_KEYCLICK);
}

/* ---------------------------------------------------------- the kill switch */

void stick_check_killswitch(int button)
{
#ifdef STICK_HAVE_KEYMAP
    int held = button & ~(BUTTON_REL | BUTTON_REPEAT);

    /* Deliberately no lazy init here. This runs on every action poll,
     * including early in the boot before the card is mounted and before
     * there is a screen to splash on; the marker check belongs on the
     * touch path, where both are guaranteed to be up. */

    /* Once the stick has run for a while without taking the device down
     * with it, drop the crash marker. */
    if (marker_present && TIME_AFTER(current_tick,
                                     first_use_tick + BOOT_SETTLE_TICKS))
        marker_clear();

    if (session_disabled)
        return;

    if ((held & STICK_KILL_COMBO) == STICK_KILL_COMBO)
    {
        if (kill_held_since == 0)
            kill_held_since = current_tick;
        else if (TIME_AFTER(current_tick, kill_held_since + KILL_HOLD_TICKS))
        {
            session_disabled = true;
            kill_switched = true;
            kill_held_since = 0;
            marker_clear();
            stick_reset(&live_state, &live_cfg);
            if (global_settings.keyclick)
                system_sound_play(SOUND_KEYCLICK);
            splashf(HZ * 2, "Stick off for this session");
            logf("stick: disabled by the hardware kill switch");
        }
    }
    else
    {
        kill_held_since = 0;
    }
#else
    (void)button;
#endif
}

/* --------------------------------------------------------------- emitting */

#ifdef STICK_HAVE_KEYMAP

static int binding_to_button(int binding, int *prebutton)
{
    if (binding <= STICK_BIND_NONE || binding >= STICK_BIND_COUNT)
    {
        *prebutton = BUTTON_NONE;
        return BUTTON_NONE;
    }
    *prebutton = binding_buttons[binding].prebutton;
    return binding_buttons[binding].button;
}

/* The queue is the one path where a synthesised button is indistinguishable
 * from a real key press.
 *
 * The inline path is safe by position: get_action_touchscreen() runs after
 * the five physical keys have already been resolved, so a button it returns
 * never reaches rpkeys. A queued one comes back round through
 * action_poll_button() on a later poll and arrives at the top of the
 * pipeline like any other press - which is how a scroll binding's
 * BUTTON_UP was being read as the volume key and why dragging a list
 * changed the volume. BUTTON_SYNTH marks them; action.c strips it before
 * the keymaps, so the impersonation still works everywhere it should. */
#ifdef BUTTON_SYNTH
#define SYNTH_TAG(b)  ((b) | BUTTON_SYNTH)
#else
#define SYNTH_TAG(b)  (b)
#endif

/* Extra emissions that will not fit into this call go through the button
 * queue, where they drain on their own. */
static void post_binding(int binding)
{
    int pre;
    int btn = binding_to_button(binding, &pre);

    if (btn == BUTTON_NONE)
        return;

    if (pre != BUTTON_NONE)
        button_queue_post(SYNTH_TAG(pre), 0);
    button_queue_post(SYNTH_TAG(btn), 0);
}

/* Stops whatever a hold started. Only the seeks need it; for everything
 * else this does nothing. Goes through the queue rather than inline because
 * the gesture that ended may also have an ordinary button to deliver. */
static void post_hold_end(int binding)
{
    const struct binding_button *bb;

    if (binding <= STICK_BIND_NONE || binding >= STICK_BIND_COUNT)
        return;

    bb = &binding_buttons[binding];
    if (bb->release == BUTTON_NONE)
        return;

    if (bb->release_pre != BUTTON_NONE)
        button_queue_post(SYNTH_TAG(bb->release_pre), 0);
    button_queue_post(SYNTH_TAG(bb->release), 0);
}

static int dial_binding(int kind, int step)
{
    switch (kind)
    {
    case STICK_DIAL_VOLUME:
        return step > 0 ? STICK_BIND_VOL_UP : STICK_BIND_VOL_DOWN;
    case STICK_DIAL_SCROLL:
        /* Clockwise walks down the list, the way a wheel does. */
        return step > 0 ? STICK_BIND_SCROLL_DOWN : STICK_BIND_SCROLL_UP;
    default:
        return STICK_BIND_NONE;
    }
}

#endif /* STICK_HAVE_KEYMAP */

/* ------------------------------------------------------------------ tick */

void stick_tick(int context)
{
#ifdef STICK_HAVE_KEYMAP
    struct stick_output out;
    int i, n;

    if (stick_phase(&live_state) != STICK_PHASE_COAST)
        return;
    if (!stick_enabled())
    {
        stick_reset(&live_state, &live_cfg);
        return;
    }
    /* The screen the throw belonged to has gone. Same reason as above. */
    if (cached_context != -1 && cached_context != context)
    {
        stick_reset(&live_state, &live_cfg);
        cached_context = -1;
        return;
    }

    stick_process(&live_state, STICK_TOUCH_IDLE, live_state.x, live_state.y,
                  now_ms(), &out);

    play_cue(out.cue);
    overlay_update(&live_state);
#ifdef HAVE_LCD_LAYERS
    if (!overlay.want)
        overlay_clear();        /* the coast ended; see the touch path */
#endif

    if (out.action != STICK_ACT_FIRE)
        return;

    /* Everything goes through the queue: this is not running on the back of
     * a touch event, so there is no button to hand back to the caller. */
    n = out.dial_steps > 0 ? out.dial_steps : 1;
    for (i = 0; i < n; i++)
        post_binding(out.binding);

    overlay_invalidate();
#endif
}

/* ------------------------------------------------------------ event entry */

int stick_handle_touch(const struct touchevent *ev, int context,
                       int *button, int *prebutton)
{
#ifndef STICK_HAVE_KEYMAP
    (void)ev; (void)context; (void)button; (void)prebutton;
    return STICK_RESULT_PASS;
#else
    struct stick_output out;
    int type;

    *button = BUTTON_NONE;
    *prebutton = BUTTON_NONE;

    /* Locked means locked. The POWER hold swallows every physical key but
     * itself, and a panel that still scrolled and selected underneath that
     * would be a lock in name only - the touchscreen is the input this
     * device is most likely to get in a pocket. Consumed rather than
     * passed, so nothing downstream sees the contact either. */
    if (rpkeys_locked())
    {
        stick_reset(&live_state, &live_cfg);
        return STICK_RESULT_CONSUMED;
    }

    /* Lists scroll under the thumb and answer sideways on their own (see
     * gui_synclist_do_touchscreen); the stick is for everything else.
     * The context is checked too: a list that hands over to one of these
     * screens without handling its last button still reads as active,
     * and the quickscreen then lost its swipes to the list's handler. */
    if (gui_synclist_is_active() &&
        (context & 0xff) != CONTEXT_WPS &&
        (context & 0xff) != CONTEXT_QUICKSCREEN &&
        (context & 0xff) != CONTEXT_PITCHSCREEN &&
        (context & 0xff) != CONTEXT_YESNOSCREEN &&
        (context & 0xff) != CONTEXT_KEYBOARD &&
        (context & 0xff) != CONTEXT_FM)
    {
        stick_reset(&live_state, &live_cfg);
        return STICK_RESULT_PASS;
    }

    if (context != cached_context)
    {
        stick_build_config(&live_cfg, context);
        cached_context = context;
        /* The screen changed under the thumb, so whatever is in flight
         * belonged to the screen that has gone. A gesture that opened the
         * context menu and then carried on scrolling landed the selection
         * a row or two into the new list before the user had seen it.
         *
         * Reset unconditionally: IDLE swallows contact and release alike
         * and only a fresh press arms again, which is exactly "lift off
         * and start over". A coast is dropped for the older reason - it
         * would scroll a list nobody threw. */
        stick_reset(&live_state, &live_cfg);
    }

    switch (ev->type)
    {
    case TOUCHEVENT_PRESS:   type = STICK_TOUCH_PRESS;   break;
    case TOUCHEVENT_CONTACT: type = STICK_TOUCH_CONTACT; break;
    case TOUCHEVENT_RELEASE: type = STICK_TOUCH_RELEASE; break;
    default:                 type = STICK_TOUCH_IDLE;    break;
    }

    if (type == STICK_TOUCH_PRESS)
        marker_write();

    stick_process(&live_state, type, ev->x, ev->y, now_ms(), &out);

    /* One complete gesture is the thing the marker was guarding: if we got
     * here the touch path did not take the device down. Clearing it now
     * rather than ten seconds later is what stops an ordinary power-off
     * shortly after a gesture from looking like a crash on the next boot. */
    if (type == STICK_TOUCH_RELEASE)
        marker_clear();

    play_cue(out.cue);
    note_dial_arming();
    overlay_update(&live_state);

#ifdef HAVE_LCD_LAYERS
    /* The gesture is over: take the overlay off now. Leaving it to the next
     * action poll kept the last frame on the panel for as long as the
     * screen sat blocked waiting for input - the edge light that stayed lit
     * after a release. In its own layer the clear cannot scribble on
     * anything, so there is no reason to wait. */
    if (!overlay.want)
        overlay_clear();
#endif

    if (out.hold_end != STICK_BIND_NONE)
    {
        post_hold_end(out.hold_end);
        overlay_invalidate();
    }

    switch (out.action)
    {
    case STICK_ACT_FIRE:
    {
        int i;

        /* A drag that crossed several scroll steps in one event owes more
         * than one press. The first goes out inline, the rest through the
         * queue, exactly as a fast dial spin does. */
        for (i = 1; i < out.dial_steps; i++)
            post_binding(out.binding);

        *button = binding_to_button(out.binding, prebutton);
        if (*button == BUTTON_NONE)
            return STICK_RESULT_CONSUMED;
        overlay_invalidate();
        return STICK_RESULT_BUTTON;
    }

    case STICK_ACT_DIAL:
    {
        int binding = dial_binding(live_cfg.dial, out.dial_steps);
        int n = out.dial_steps < 0 ? -out.dial_steps : out.dial_steps;
        int i;

        if (binding == STICK_BIND_NONE || n == 0)
            return STICK_RESULT_CONSUMED;

        /* The first step goes out inline; a fast spin's remainder drains
         * through the queue. */
        for (i = 1; i < n; i++)
            post_binding(binding);

        *button = binding_to_button(binding, prebutton);
        if (*button == BUTTON_NONE)
            return STICK_RESULT_CONSUMED;
        overlay_invalidate();
        return STICK_RESULT_BUTTON;
    }

    case STICK_ACT_TAP:
        /* Deliberately dropped. Replaying the tap as a real touch was an
         * absolute hit test smuggled into a relative scheme: in a list it
         * selected whatever row the thumb happened to land on, which is the
         * behaviour the stick exists to replace. A tap that overran the arm
         * window still resolves through the centre binding. */
        return STICK_RESULT_CONSUMED;

    case STICK_ACT_PASS:
        /* A press that started outside the arm zone. The engine offers it
         * back for ordinary absolute handling, and we decline: one scheme
         * owns the panel, and letting some presses through by position is
         * precisely the mixture that made navigation feel random. An
         * unarmed area does nothing, which is at least predictable.
         *
         * The edge strips are the one deliberate exception, and they are
         * handled inside the engine rather than by handing the panel back. */
        return STICK_RESULT_CONSUMED;

    case STICK_ACT_DROP:
    case STICK_ACT_NONE:
    default:
        return STICK_RESULT_CONSUMED;
    }
#endif
}

#endif /* HAVE_TOUCHSCREEN */
