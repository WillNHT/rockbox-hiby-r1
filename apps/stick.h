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
    /* Literal "go:<screen>" bindings from spec 4 are deliberately absent.
     * The R1 keymap has no unambiguous key equivalent for them, and
     * inventing one would break the guarantee that the engine can only
     * ever reach what a physical key can reach (spec 9). They arrive with
     * the per-context tables. */
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
#define STICK_DEF_TRAVEL_PX      78   /* deflection reaches 1.0 here      */
#define STICK_DEF_ARM_MS        110
#define STICK_DEF_REPEAT_MS     260
#define STICK_DEF_DEG_PER_DETENT 14
#define STICK_DEF_TAP_SLOP_PX    10
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
    uint8_t  centre;                     /* fired on release, no travel   */

    uint8_t  dial;                       /* enum stick_dial_kind          */
    uint8_t  deg_per_detent;             /* 8..30                         */

    uint16_t arm_ms;
    uint16_t repeat_delay_ms;
    int16_t  detent_px;
    int16_t  dial_min_px;
    int16_t  travel_px;
    int16_t  tap_slop_px;
};

/* Fill cfg with the firmware defaults (spec 11, theme "Default"). */
void stick_config_default(struct stick_config *cfg, int lcd_w, int lcd_h);

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
    int16_t  dial_steps;
    int16_t  tap_x, tap_y;
};

struct stick_state
{
    const struct stick_config *cfg;

    int16_t ox, oy;        /* touch origin                        */
    int16_t x, y;          /* latest point                        */
    int16_t cx, cy;        /* arm zone centre: the dial pivot      */
    int16_t max_travel;    /* furthest distance from the origin    */
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

/* Angle of (dx, dy) in 1/64 degree, 0 = up, clockwise positive. Exposed for
 * the unit tests. */
int32_t stick_atan2_deg64(int32_t dx, int32_t dy);

/* Integer hypotenuse, exposed for the unit tests. */
int32_t stick_hypot(int32_t dx, int32_t dy);

/* Sector index for a displacement, or -1 if N < 1. */
int stick_sector_at(const struct stick_config *cfg, int dx, int dy);

bool stick_zone_contains(const struct stick_zone *z, int x, int y);

#endif /* _STICK_H_ */
