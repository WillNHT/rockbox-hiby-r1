/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - relative virtual stick for touchscreen targets.
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
#ifndef _STICK_H_
#define _STICK_H_

/* This header and stick.c deliberately depend on nothing but the C library
 * so the engine can be built and tested on a host machine. Everything that
 * touches Rockbox lives in stick_glue.c. */

#include <stdint.h>
#include <stdbool.h>

#define STICK_MAX_SECTORS 8
#define STICK_MAX_DEAD    4

/* Geometry --------------------------------------------------------------- */

enum stick_shape
{
    STICK_SHAPE_NONE = 0,
    STICK_SHAPE_BOX,      /* a=left  b=top    c=width  d=height */
    STICK_SHAPE_CIRCLE,   /* a=cx    b=cy     c=radius d=unused */
};

struct stick_zone
{
    int16_t shape;
    int16_t a, b, c, d;
};

/* Bindings --------------------------------------------------------------- */

enum stick_binding
{
    STICK_BIND_NONE = 0,
    STICK_BIND_SCROLL_UP,
    STICK_BIND_SCROLL_DOWN,
    STICK_BIND_SELECT,
    STICK_BIND_BACK,
    STICK_BIND_PLAY_PAUSE,
    STICK_BIND_NEXT,
    STICK_BIND_PREV,
    STICK_BIND_VOL_UP,
    STICK_BIND_VOL_DOWN,
    /* These five have a key equivalent on the R1 after all - a POWER
     * combination - so the guarantee that the engine can only reach what a
     * physical key reaches still holds (spec 9). They are what makes the
     * WPS escapable without touching a physical button. */
    STICK_BIND_MENU,          /* the main menu                           */
    STICK_BIND_QUICKSCREEN,   /* shuffle / repeat                        */
    STICK_BIND_STOP,
    STICK_BIND_SEEK_FWD,      /* seeks until the gesture ends            */
    STICK_BIND_SEEK_BACK,
    STICK_BIND_COUNT
};

/* True for bindings that fire continuously while held (spec 4). */
bool stick_binding_repeats(int binding);

/* Human readable name, used by the config parser and the lab plugin. */
const char *stick_binding_name(int binding);
int stick_binding_from_name(const char *name);

enum stick_dial_kind
{
    STICK_DIAL_OFF = 0,
    STICK_DIAL_VOLUME,
    STICK_DIAL_SCROLL,
    STICK_DIAL_CHAR,
};

/* Configuration ---------------------------------------------------------- */

/* Firmware defaults, in display pixels for a 480x800 panel. Scaled off
 * LCD_DPI by the glue layer so a second target inherits sane numbers. */
#define STICK_DEF_DETENT_PX      18   /* no sector selected inside this   */
#define STICK_DEF_DIAL_MIN_PX    34   /* angle is not read below this     */
/* The dial's pivot floats rather than being nailed to the point the thumb
 * armed at.
 *
 * Anchoring on the arming point meant the dial could only be drawn around
 * wherever the gesture happened to begin, and arming near an edge left no
 * room to draw anything at all. Instead the pivot is dragged along behind
 * the thumb whenever the thumb gets further away than this: a straight
 * slide pushes the pivot directly ahead of itself, so the bearing never
 * changes and no detents come out of it, while a curve leaves the pivot
 * where it is and turns into rotation. So the user can arm anywhere, slide
 * anywhere, and circle there. */
#define STICK_DEF_DIAL_MAX_PX   110
#define STICK_DEF_SCROLL_PX     140   /* vertical travel per scroll step  */
/* Scrolling is velocity-accelerated. STICK_DEF_SCROLL_PX is the *slow*
 * baseline, deliberately coarse - a tenth of the old sensitivity - so a
 * careful thumb lands on the row it meant. Speed shrinks the effective
 * step, up to accel_max_q8, so a long list still crosses in a throw and a
 * fast drag feels roughly like the old linear one.
 *
 * This is one scheme on every screen, not a per-page mode: the engine
 * never learns how long the list is, which is what keeps it testable on
 * the host and honest about the output rule. */
#define STICK_DEF_ACCEL_V0      250   /* px/s worth one extra unit of gain */
/* The ceiling moves with the baseline on purpose: halving scroll_px while
 * halving this leaves the fast end of the range exactly where it was and
 * only makes the slow end twice as willing. */
#define STICK_DEF_ACCEL_MAX_Q8  (6*256) /* gain ceiling: 6x               */
#define STICK_MIN_EFF_STEP_PX     4   /* never finer than this            */
/* A drag that has committed to a direction keeps it until the thumb comes
 * back this far. A thumb decelerating at the end of a throw rocks back a
 * few pixels, and at the fast end of the acceleration range that was
 * enough to emit a step the wrong way - which is what made scrolling
 * intermittently reverse itself. */
#define STICK_DEF_SCROLL_REVERSE_PX 56
/* A sector keeps the gesture until another one wins by this much. Thumbs
 * travel in an arc, not a line, so a vertical drag drifts sideways; without
 * a margin that drift crosses into the left or right sector and fires
 * back/forward in the middle of a scroll. */
#define STICK_DEF_SECTOR_HYST_DEG  14
/* Coasting. A release above this speed keeps the list moving and lets it
 * run down, the way the ordinary touch scroll does. */
#define STICK_DEF_EDGE_PX         28   /* strip either side of the panel */
#define STICK_DEF_EDGE_TRAVEL_PX  64   /* inward travel that commits it  */
#define STICK_DEF_COAST_MIN_PX_S 120
#define STICK_COAST_STOP_PX_S      3   /* below this the coast is over   */
/* Deceleration in Rockbox's own shape: a = a0 + a1*|v|, with a0 in px/s^2
 * and a1 a Q8 multiplier. These are only the fallbacks - the glue fills
 * them from the list's kinetic settings so both schemes decelerate alike. */
#define STICK_DEF_DECEL_A0      2000
#define STICK_DEF_DECEL_A1       128   /* 0.5 in Q8 */
#define STICK_DEF_TRAVEL_PX      78   /* deflection reaches 1.0 here      */
#define STICK_DEF_ARM_MS        110
#define STICK_DEF_HOLD_MS       420   /* sector held this long: hold bind */
#define STICK_DEF_REPEAT_MS     260
#define STICK_DEF_DEG_PER_DETENT 14
/* Arming the dial by holding still. A dial that is simply on from the
 * moment a gesture starts takes the sectors away entirely - the engine
 * reads angle and nothing else - which is how a theme with the dial
 * enabled ended up with no working left or right at all. Armed by a slow
 * tap instead, both can exist on one screen: travel resolves a sector,
 * staying put resolves the dial. */
#define STICK_DEF_DIAL_ARM_MS  1500
#define STICK_DIAL_TICK_MS      250   /* cue while the hold is building */
#define STICK_DEF_TAP_SLOP_PX    10
/* Sustained scrolling - the long library.
 *
 * Casual scrolling is a drag, and a drag can only cover as much list as
 * the thumb can cover screen. For a library of thousands that is not
 * enough, and the honest fix is not a new zone with a meaning of its own:
 * it is noticing that the user has stopped asking for a distance and
 * started asking for a rate.
 *
 * So: park the thumb - at full deflection, or pressed against the rim in
 * the direction already committed, because a drag that begins near the top
 * of the panel cannot reach full deflection upward and that is exactly the
 * drag a long list produces - and the list keeps going, accelerating from
 * a walk to a run. The rim only ever qualifies a gesture that is already
 * running; it never decides a direction, so "up" always means what it
 * meant during the drag. sustain_ms of 0 turns the whole thing off. */
#define STICK_DEF_SUSTAIN_MS       450  /* still this long before it runs */
#define STICK_DEF_SUSTAIN_SLOP_PX   12  /* "still" is within this          */
#define STICK_DEF_SUSTAIN_EDGE_PX   40  /* pinned this close to the rim    */
#define STICK_DEF_SUSTAIN_V0         6  /* rows per second at the start    */
#define STICK_DEF_SUSTAIN_VMAX      45  /* rows per second at the cap      */
#define STICK_DEF_SUSTAIN_RAMP_MS 2500  /* v0 -> vmax over this            */
#define STICK_TIMEOUT_MS       2000   /* lost release is reaped as cancel */

struct stick_config
{
    struct stick_zone arm;
    struct stick_zone work;
    struct stick_zone dead[STICK_MAX_DEAD];
    uint8_t  n_dead;

    uint8_t  sectors;                    /* 1..8                          */
    int16_t  rotation;                   /* 0..359, sector 0 centred here */
    uint8_t  bind[STICK_MAX_SECTORS];
    /* Fired instead of bind[] when the sector is held past hold_ms. NONE
     * leaves a sector behaving exactly as it did before holds existed. */
    uint8_t  hold[STICK_MAX_SECTORS];
    uint8_t  centre;                     /* fired on release, no travel   */

    uint8_t  dial;                       /* enum stick_dial_kind          */
    uint8_t  deg_per_detent;             /* 8..30                         */
    /* The dial waits for a slow tap rather than owning the gesture from
     * the start. Zero keeps the old behaviour, where setting .dial at all
     * meant every gesture on that screen was a dial. */
    uint8_t  dial_on_hold;
    uint16_t dial_arm_ms;

    uint16_t arm_ms;
    uint16_t hold_ms;
    uint16_t repeat_delay_ms;
    int16_t  detent_px;
    int16_t  dial_min_px;
    /* How far the thumb may get from the dial's pivot before the pivot is
     * dragged after it. See STICK_DEF_DIAL_MAX_PX. */
    int16_t  dial_max_px;
    /* Pixels of vertical travel per scroll step. Scrolling is a drag, not
     * a repeat: the list follows the thumb and stops when it stops. */
    int16_t  scroll_px;
    /* Velocity acceleration on the scroll step. accel_v0 is the vertical
     * speed worth one extra unit of gain; accel_max_q8 caps the gain.
     * accel_max_q8 == 256 (gain 1.0) turns acceleration off and restores
     * the purely linear drag. */
    int16_t  accel_v0;
    int32_t  accel_max_q8;
    /* Travel back before a committed scroll will change direction. Zero
     * restores the old behaviour, where every pixel of reversal counted. */
    int16_t  scroll_reverse_px;
    /* Degrees another sector must win by before the gesture moves to it.
     * Zero is the old hard boundary. */
    uint8_t  sector_hyst_deg;
    int16_t  travel_px;
    int16_t  tap_slop_px;

    /* Edge swipes. A press inside edge_px of the left or right edge is not
     * a stick gesture at all: drag inward past edge_travel_px and the
     * matching binding fires once. Zero edge_px turns them off.
     *
     * These live in the engine rather than being handed back to ordinary
     * touch handling on purpose - an edge swipe is a deliberate exception
     * to "one scheme owns the panel", not a hole in it. */
    /* Panel size, so the engine can find the right-hand edge without being
     * told the screen dimensions on every call. Filled by
     * stick_config_default() and re-checked by stick_config_validate(). */
    int16_t  panel_w, panel_h;

    int16_t  edge_px;
    int16_t  edge_travel_px;
    uint8_t  edge_left;      /* fired by a swipe inward from the left  */
    uint8_t  edge_right;

    /* Sustained scrolling. Zero sustain_ms turns it off. */
    uint16_t sustain_ms;
    int16_t  sustain_slop_px;
    int16_t  sustain_edge_px;
    int16_t  sustain_v0;        /* rows per second               */
    int16_t  sustain_vmax;
    uint16_t sustain_ramp_ms;

    /* Kinetic coast. Zero coast_min_px_s turns coasting off entirely. */
    int16_t  coast_min_px_s;
    int32_t  decel_a0;      /* px/s^2                                   */
    int32_t  decel_a1;      /* Q8 multiplier on the current speed       */
};

/* Fill cfg with the firmware defaults (spec 11, theme "Default"). */
void stick_config_default(struct stick_config *cfg, int lcd_w, int lcd_h);

/* Pixels of travel per scroll step for Rockbox's own Scroll Speed setting
 * (0..17, 9 by default). The stick has no speed knob of its own: one
 * scrolling speed for the device is the whole point. */
int16_t stick_scroll_px_for_speed(int scroll_speed);

/* Clamp anything out of range back to the firmware default. Returns false
 * if something had to be corrected, so the caller can log it. Never fails
 * in a way that prevents use - spec 9. */
bool stick_config_validate(struct stick_config *cfg, int lcd_w, int lcd_h);

/* True if the table would leave an armed screen with nothing bound at all,
 * which the theme validator must reject - spec 10.3. */
bool stick_config_is_inert(const struct stick_config *cfg);

/* Engine ----------------------------------------------------------------- */

enum stick_touch_type
{
    STICK_TOUCH_IDLE = 0,   /* no contact; used to reap a lost release */
    STICK_TOUCH_PRESS,
    STICK_TOUCH_CONTACT,
    STICK_TOUCH_RELEASE,
};

enum stick_phase
{
    STICK_PHASE_IDLE = 0,
    STICK_PHASE_ABSOLUTE,   /* pressed outside ARM: never becomes a stick */
    STICK_PHASE_ARMING,
    STICK_PHASE_ARMED,
    STICK_PHASE_DIAL,
    STICK_PHASE_CANCELLED,
    STICK_PHASE_DEAD,
    /* The thumb is gone but the list is still moving. Fed by idle ticks,
     * ended by its own deceleration or by the next press. */
    STICK_PHASE_COAST,
    /* Started on a screen edge: a swipe inward, not a stick gesture. */
    STICK_PHASE_EDGE,
};

enum stick_action
{
    STICK_ACT_NONE = 0,  /* engine owns the gesture, nothing to hand over */
    STICK_ACT_PASS,      /* not ours: hand the event to ordinary touch    */
    STICK_ACT_DROP,      /* dead zone: swallow the press entirely         */
    STICK_ACT_FIRE,      /* emit .binding                                 */
    STICK_ACT_DIAL,      /* .dial_steps signed detents this tick          */
    STICK_ACT_TAP,       /* replay a press+release at .tap_x/.tap_y       */
};

enum stick_cue
{
    STICK_CUE_NONE = 0,
    STICK_CUE_ARM,
    STICK_CUE_SECTOR,
    STICK_CUE_DETENT,
    STICK_CUE_COMMIT,
    STICK_CUE_REJECT,
};

struct stick_output
{
    uint8_t  action;
    uint8_t  binding;
    uint8_t  cue;
    bool     repeating;   /* the fire came from a repeating binding */
    /* Steps owed: the dial's detents, or the number of scroll steps a
     * single drag event crossed. Always 1 for an ordinary fire. */
    int16_t  dial_steps;
    int16_t  tap_x, tap_y;
    /* The binding that has just stopped being held, or STICK_BIND_NONE.
     * The caller emits whatever terminates it - a seek has to be told to
     * stop. Separate from .binding because a gesture can end one hold and
     * start something else on the same event. */
    uint8_t  hold_end;
};

struct stick_state
{
    const struct stick_config *cfg;

    int16_t ox, oy;        /* touch origin                        */
    int16_t x, y;          /* latest point                        */
    int16_t cx, cy;        /* arm zone centre: the dial pivot      */
    int16_t max_travel;    /* furthest distance from the origin    */
    int16_t scroll_ref_y;  /* y at which the last scroll step fired */
    int16_t detents;       /* signed detents committed this dial   */

    int32_t acc_angle;     /* 1/64 degree, unwrapped               */
    int32_t last_angle;    /* 1/64 degree, STICK_ANGLE_UNSET if none */
    int32_t repeat_acc;    /* Q8 fractional repeats owed           */

    long    t0;            /* press time, ms                       */
    long    t_last;        /* last event time, ms                  */
    long    t_sector;      /* time the current sector was entered  */

    int8_t  phase;
    int8_t  sector;        /* -1 when inside the detent radius     */
    uint8_t deflection;    /* 0..255                               */
    bool    dial;
    bool    fired;         /* a repeating binding already fired    */
    int32_t vel_px_s;      /* smoothed vertical speed of the thumb */
    int32_t coast_sub_px;  /* sub-step pixels owed by the coast     */
    int8_t  scroll_dir;    /* -1, 0 or +1: the direction it committed to */
    long    sustain_t0;    /* when the thumb parked, 0 while it has not  */
    int16_t sustain_ref_y; /* the point it parked at                     */
    int32_t sustain_acc_q8;/* fractional rows the ramp owes              */
    int8_t  scroll_turn;   /* a reversal seen once, waiting to be agreed */
    bool    held_fired;    /* this sector's hold binding has fired */
    uint8_t held_bind;     /* which one, so cancel can end it too  */
};

#define STICK_ANGLE_UNSET INT32_MIN

void stick_reset(struct stick_state *st, const struct stick_config *cfg);

/* Feed one touch event (or STICK_TOUCH_IDLE on a quiet tick). now_ms must be
 * monotonic. Fills *out; never allocates, never blocks. */
void stick_process(struct stick_state *st, int type, int x, int y,
                   long now_ms, struct stick_output *out);

/* Introspection for the lab plugin and the presentation layer (spec 1,
 * layer 3 reads this bundle and paints it). */
static inline int stick_phase(const struct stick_state *st) { return st->phase; }
static inline int stick_sector(const struct stick_state *st) { return st->sector; }
static inline int stick_deflection(const struct stick_state *st) { return st->deflection; }

/* Pixels of travel currently worth one scroll step, after velocity
 * acceleration. The overlay reads this so the rate it prints is the rate
 * the engine is actually using. */
int stick_effective_scroll_px(const struct stick_state *st);

/* Angle of (dx, dy) in 1/64 degree, 0 = up, clockwise positive. Exposed for
 * the unit tests. */
int32_t stick_atan2_deg64(int32_t dx, int32_t dy);

/* Integer hypotenuse, exposed for the unit tests. */
int32_t stick_hypot(int32_t dx, int32_t dy);

/* Sector index for a displacement, or -1 if N < 1. */
int stick_sector_at(const struct stick_config *cfg, int dx, int dy);

bool stick_zone_contains(const struct stick_zone *z, int x, int y);

#endif /* _STICK_H_ */
