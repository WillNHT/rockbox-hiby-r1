/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - the grammar layer (spec 1, layer 1).
 *
 * press -> classify by zone -> arm -> resolve to a sector -> arbitrate at
 * release. Integer arithmetic only, no allocation, no timer of its own: the
 * caller drives it from touch events plus the tick it already has.
 *
 * This file must not include any Rockbox header. It is compiled both into
 * the firmware and into the host test harness in tools/stick_test.
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

#include "stick.h"

/* No libc, not even string.h: the engine is linked both into the core and
 * into the lab plugin, and plugins reach libc through the plugin API rather
 * than by linking it. Two short helpers are cheaper than caring which. */

static void stick_zero(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--)
        *b++ = 0;
}

static bool stick_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

#define DEG64_FULL   23040    /* 360 * 64 */
#define DEG64_RIGHT   5760    /*  90 * 64 */

/* Longest gap we will integrate over in one step. Protects the repeat
 * accumulator if the UI thread stalls. */
#define MAX_DT_MS      100

/* ------------------------------------------------------------------ math */

/* atan(i/64) in 1/64 degree, i = 0..64. Two extra entries so the linear
 * interpolation below can always read lut[idx + 1]. */
static const uint16_t atan_lut[66] =
{
        0,    57,   115,   172,   229,   286,   343,   399,
      456,   512,   568,   624,   680,   735,   790,   844,
      898,   952,  1005,  1058,  1111,  1163,  1214,  1265,
     1316,  1366,  1415,  1464,  1512,  1560,  1607,  1654,
     1700,  1746,  1791,  1835,  1879,  1922,  1965,  2007,
     2048,  2089,  2130,  2169,  2209,  2247,  2285,  2323,
     2360,  2396,  2432,  2467,  2502,  2536,  2570,  2603,
     2636,  2668,  2700,  2731,  2762,  2792,  2822,  2851,
     2880,  2880,
};

/* Angle from the vertical axis, 0..90 degrees in 1/64 degree units, for a
 * displacement whose components have already been made non-negative. */
static int32_t atan_from_vertical(int32_t across, int32_t along)
{
    int32_t q, idx, frac, lo, hi;

    if (across == 0)
        return 0;
    if (along == 0)
        return DEG64_RIGHT;

    if (across <= along)
    {
        q = (across << 14) / along;         /* 0 .. 16384 == 64 << 8 */
        idx = q >> 8;
        frac = q & 0xff;
        lo = atan_lut[idx];
        hi = atan_lut[idx + 1];
        return lo + (((hi - lo) * frac) >> 8);
    }
    else
    {
        q = (along << 14) / across;
        idx = q >> 8;
        frac = q & 0xff;
        lo = atan_lut[idx];
        hi = atan_lut[idx + 1];
        return DEG64_RIGHT - (lo + (((hi - lo) * frac) >> 8));
    }
}

/* 0 = straight up, clockwise positive, in 1/64 degree. dy is in screen
 * coordinates, so up is negative - the same convention the spec uses when
 * it writes atan2(dx, -dy). */
int32_t stick_atan2_deg64(int32_t dx, int32_t dy)
{
    int32_t up = -dy;
    int32_t across = dx < 0 ? -dx : dx;
    int32_t along = up < 0 ? -up : up;
    int32_t t = atan_from_vertical(across, along);

    if (dx >= 0)
        return (up >= 0) ? t : (DEG64_FULL / 2) - t;
    else
        return (up >= 0) ? (DEG64_FULL - t) % DEG64_FULL
                         : (DEG64_FULL / 2) + t;
}

int32_t stick_hypot(int32_t dx, int32_t dy)
{
    uint32_t v = (uint32_t)(dx * dx + dy * dy);
    uint32_t rem = 0, root = 0;
    int i;

    /* Plain integer square root; the inputs here are at most a few
     * hundred thousand, so this is a handful of iterations. */
    for (i = 0; i < 16; i++)
    {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        if (root < rem)
        {
            root++;
            rem -= root;
            root++;
        }
    }
    return (int32_t)(root >> 1);
}

/* ----------------------------------------------------------------- zones */

bool stick_zone_contains(const struct stick_zone *z, int x, int y)
{
    if (!z)
        return false;

    if (z->shape == STICK_SHAPE_CIRCLE)
    {
        int dx = x - z->a;
        int dy = y - z->b;
        return (dx * dx + dy * dy) <= ((int)z->c * (int)z->c);
    }
    if (z->shape == STICK_SHAPE_BOX)
        return x >= z->a && x < z->a + z->c &&
               y >= z->b && y < z->b + z->d;

    return false;
}

static void zone_centre(const struct stick_zone *z, int16_t *cx, int16_t *cy)
{
    if (z->shape == STICK_SHAPE_CIRCLE)
    {
        *cx = z->a;
        *cy = z->b;
    }
    else
    {
        *cx = z->a + z->c / 2;
        *cy = z->b + z->d / 2;
    }
}

static bool in_dead(const struct stick_config *cfg, int x, int y)
{
    int i;
    for (i = 0; i < cfg->n_dead && i < STICK_MAX_DEAD; i++)
        if (stick_zone_contains(&cfg->dead[i], x, y))
            return true;
    return false;
}

/* --------------------------------------------------------------- bindings */

static const char * const binding_names[STICK_BIND_COUNT] =
{
    "none", "scrollUp", "scrollDown", "select", "back", "playPause",
    "next", "prev", "volUp", "volDown",
    "menu", "quickScreen", "stop", "seekFwd", "seekBack",
};

const char *stick_binding_name(int binding)
{
    if (binding < 0 || binding >= STICK_BIND_COUNT)
        return "none";
    return binding_names[binding];
}

int stick_binding_from_name(const char *name)
{
    int i;
    if (!name)
        return STICK_BIND_NONE;
    for (i = 0; i < STICK_BIND_COUNT; i++)
        if (stick_streq(name, binding_names[i]))
            return i;
    return -1;
}

bool stick_binding_repeats(int binding)
{
    switch (binding)
    {
    case STICK_BIND_SCROLL_UP:
    case STICK_BIND_SCROLL_DOWN:
    case STICK_BIND_VOL_UP:
    case STICK_BIND_VOL_DOWN:
        return true;
    default:
        return false;
    }
}

/* ----------------------------------------------------------------- config */

void stick_config_default(struct stick_config *cfg, int lcd_w, int lcd_h)
{
    stick_zero(cfg, sizeof(*cfg));

    /* Theme "Default" from the reference set: arm and work are the whole
     * screen so the thumb can never accidentally cancel, with the status
     * bar dead. */
    cfg->arm.shape = STICK_SHAPE_BOX;
    cfg->arm.a = 0;
    cfg->arm.b = 0;
    cfg->arm.c = lcd_w;
    cfg->arm.d = lcd_h;
    cfg->work = cfg->arm;

    cfg->n_dead = 0;

    cfg->sectors = 4;
    cfg->rotation = 0;
    cfg->bind[0] = STICK_BIND_SCROLL_UP;
    cfg->bind[1] = STICK_BIND_SELECT;
    cfg->bind[2] = STICK_BIND_SCROLL_DOWN;
    cfg->bind[3] = STICK_BIND_BACK;
    cfg->centre = STICK_BIND_PLAY_PAUSE;
    /* No holds by default: a sector the user has not been told about must
     * not change meaning because their thumb lingered. */

    cfg->dial = STICK_DIAL_OFF;
    cfg->deg_per_detent = STICK_DEF_DEG_PER_DETENT;
    /* Armed by a slow tap, never on from the start: the sectors and the
     * dial have to be able to share a screen. */
    cfg->dial_on_hold = 1;
    cfg->dial_arm_ms = STICK_DEF_DIAL_ARM_MS;
    cfg->arm_ms = STICK_DEF_ARM_MS;
    cfg->hold_ms = STICK_DEF_HOLD_MS;
    cfg->repeat_delay_ms = STICK_DEF_REPEAT_MS;
    cfg->detent_px = STICK_DEF_DETENT_PX;
    cfg->dial_min_px = STICK_DEF_DIAL_MIN_PX;
    cfg->dial_max_px = STICK_DEF_DIAL_MAX_PX;
    cfg->scroll_px = STICK_DEF_SCROLL_PX;
    cfg->accel_v0 = STICK_DEF_ACCEL_V0;
    cfg->accel_max_q8 = STICK_DEF_ACCEL_MAX_Q8;
    cfg->scroll_reverse_px = STICK_DEF_SCROLL_REVERSE_PX;
    cfg->sector_hyst_deg = STICK_DEF_SECTOR_HYST_DEG;
    cfg->travel_px = STICK_DEF_TRAVEL_PX;
    cfg->tap_slop_px = STICK_DEF_TAP_SLOP_PX;
    cfg->panel_w = (int16_t)lcd_w;
    cfg->panel_h = (int16_t)lcd_h;

    /* Off by default: an edge strip takes screen away from the stick, and
     * that is the user's call to make, not ours. */
    cfg->edge_px = 0;
    cfg->edge_travel_px = STICK_DEF_EDGE_TRAVEL_PX;
    cfg->edge_left = STICK_BIND_BACK;
    cfg->edge_right = STICK_BIND_MENU;
    cfg->sustain_ms       = STICK_DEF_SUSTAIN_MS;
    cfg->sustain_slop_px  = STICK_DEF_SUSTAIN_SLOP_PX;
    cfg->sustain_edge_px  = STICK_DEF_SUSTAIN_EDGE_PX;
    cfg->sustain_v0       = STICK_DEF_SUSTAIN_V0;
    cfg->sustain_vmax     = STICK_DEF_SUSTAIN_VMAX;
    cfg->sustain_ramp_ms  = STICK_DEF_SUSTAIN_RAMP_MS;
    cfg->coast_min_px_s = STICK_DEF_COAST_MIN_PX_S;
    cfg->decel_a0 = STICK_DEF_DECEL_A0;
    cfg->decel_a1 = STICK_DEF_DECEL_A1;
}

int16_t stick_scroll_px_for_speed(int scroll_speed)
{
    /* Rockbox's Scroll Speed runs 0..17 with 9 as the default, and 9 lands
     * exactly on STICK_DEF_SCROLL_PX, so adopting the setting changes
     * nothing for a device that has never touched it. Higher is faster,
     * which means fewer pixels per row.
     *
     * Proportional rather than the old +/-3px per notch: the baseline is
     * now ten times coarser, and a fixed 3px step would have made the
     * whole setting range invisible. */
    int px = (STICK_DEF_SCROLL_PX * (27 - scroll_speed)) / 18;

    if (px < 6)
        px = 6;
    if (px > 600)
        px = 600;
    return (int16_t)px;
}

static bool zone_is_sane(const struct stick_zone *z, int lcd_w, int lcd_h)
{
    if (z->shape == STICK_SHAPE_CIRCLE)
        return z->c > 0 && z->c <= 2 * (lcd_w + lcd_h);
    if (z->shape == STICK_SHAPE_BOX)
        return z->c > 0 && z->d > 0 &&
               z->c <= 2 * lcd_w && z->d <= 2 * lcd_h;
    return false;
}

bool stick_config_validate(struct stick_config *cfg, int lcd_w, int lcd_h)
{
    struct stick_config def;
    bool ok = true;
    int i;

    stick_config_default(&def, lcd_w, lcd_h);

    if (cfg->sectors < 1 || cfg->sectors > STICK_MAX_SECTORS)
    {
        cfg->sectors = def.sectors;
        ok = false;
    }

    /* Keep rotation in [0, 360) without assuming it started positive. */
    if (cfg->rotation < 0 || cfg->rotation >= 360)
    {
        cfg->rotation = ((cfg->rotation % 360) + 360) % 360;
        ok = false;
    }

    for (i = 0; i < STICK_MAX_SECTORS; i++)
    {
        if (cfg->bind[i] >= STICK_BIND_COUNT)
        {
            cfg->bind[i] = STICK_BIND_NONE;
            ok = false;
        }
        if (cfg->hold[i] >= STICK_BIND_COUNT)
        {
            cfg->hold[i] = STICK_BIND_NONE;
            ok = false;
        }
    }
    if (cfg->centre >= STICK_BIND_COUNT)
    {
        cfg->centre = STICK_BIND_NONE;
        ok = false;
    }

    if (!zone_is_sane(&cfg->arm, lcd_w, lcd_h))
    {
        cfg->arm = def.arm;
        ok = false;
    }
    /* An unset work zone defaults to the arm zone - spec 2. A work zone
     * smaller than arm is legal and only ever cancels, so it is allowed
     * through here; the theme validator is the one that warns. */
    if (!zone_is_sane(&cfg->work, lcd_w, lcd_h))
    {
        cfg->work = cfg->arm;
        ok = false;
    }
    if (cfg->n_dead > STICK_MAX_DEAD)
    {
        cfg->n_dead = STICK_MAX_DEAD;
        ok = false;
    }
    for (i = 0; i < cfg->n_dead; i++)
    {
        if (!zone_is_sane(&cfg->dead[i], lcd_w, lcd_h))
        {
            cfg->n_dead = i;
            ok = false;
            break;
        }
    }

    if (cfg->dial > STICK_DIAL_CHAR)
    {
        cfg->dial = STICK_DIAL_OFF;
        ok = false;
    }
    /* Sub-10 degree detents sit below the panel's angular noise at small
     * radii and produce phantom steps - spec 6. */
    if (cfg->deg_per_detent < 8 || cfg->deg_per_detent > 60)
    {
        cfg->deg_per_detent = def.deg_per_detent;
        ok = false;
    }
    if (cfg->arm_ms > 400)
    {
        cfg->arm_ms = def.arm_ms;
        ok = false;
    }
    /* A hold shorter than the arm window could never be reached, and one
     * past a couple of seconds is indistinguishable from a stuck panel. */
    if (cfg->hold_ms < cfg->arm_ms || cfg->hold_ms > 2000)
    {
        cfg->hold_ms = def.hold_ms;
        ok = false;
    }
    if (cfg->repeat_delay_ms > 2000)
    {
        cfg->repeat_delay_ms = def.repeat_delay_ms;
        ok = false;
    }
    if (cfg->detent_px < 1 || cfg->detent_px > lcd_w)
    {
        cfg->detent_px = def.detent_px;
        ok = false;
    }
    if (cfg->scroll_px < 4 || cfg->scroll_px > lcd_h)
    {
        ok = false;
        cfg->scroll_px = def.scroll_px;
    }
    if (cfg->accel_v0 < 1)
    {
        cfg->accel_v0 = def.accel_v0;
        ok = false;
    }
    /* Below 256 the gain would make the step *coarser* with speed, which
     * is the wrong way round for every scheme this imitates. */
    if (cfg->accel_max_q8 < 256 || cfg->accel_max_q8 > 64 * 256)
    {
        cfg->accel_max_q8 = def.accel_max_q8;
        ok = false;
    }
    if (cfg->dial_arm_ms < 300 || cfg->dial_arm_ms > 10000)
    {
        cfg->dial_arm_ms = def.dial_arm_ms;
        ok = false;
    }
    if (cfg->scroll_reverse_px < 0 || cfg->scroll_reverse_px > lcd_h)
    {
        cfg->scroll_reverse_px = def.scroll_reverse_px;
        ok = false;
    }
    /* Half a sector is the most a margin can be before the sector next
     * door becomes unreachable. */
    if (cfg->sector_hyst_deg > 45)
    {
        cfg->sector_hyst_deg = def.sector_hyst_deg;
        ok = false;
    }

    if (cfg->dial_min_px < 1 || cfg->dial_min_px > lcd_w)
    {
        cfg->dial_min_px = def.dial_min_px;
        ok = false;
    }
    if (cfg->dial_max_px <= cfg->dial_min_px || cfg->dial_max_px > lcd_w)
    {
        cfg->dial_max_px = def.dial_max_px;
        ok = false;
        if (cfg->dial_max_px <= cfg->dial_min_px)
            cfg->dial_min_px = def.dial_min_px;
    }
    if (cfg->travel_px <= cfg->detent_px || cfg->travel_px > lcd_h)
    {
        cfg->travel_px = def.travel_px;
        ok = false;
        if (cfg->travel_px <= cfg->detent_px)
            cfg->detent_px = def.detent_px;
    }
    if (cfg->tap_slop_px < 1 || cfg->tap_slop_px > cfg->detent_px)
    {
        cfg->tap_slop_px = def.tap_slop_px;
        ok = false;
    }
    /* The panel is whatever the caller says it is, always. */
    cfg->panel_w = (int16_t)lcd_w;
    cfg->panel_h = (int16_t)lcd_h;

    /* A strip wider than a third of the panel is not an edge any more. */
    if (cfg->edge_px < 0 || cfg->edge_px > lcd_w / 3)
    {
        cfg->edge_px = def.edge_px;
        ok = false;
    }
    if (cfg->edge_travel_px < 8 || cfg->edge_travel_px > lcd_w)
    {
        cfg->edge_travel_px = def.edge_travel_px;
        ok = false;
    }
    if (cfg->edge_left >= STICK_BIND_COUNT)
    {
        cfg->edge_left = STICK_BIND_NONE;
        ok = false;
    }
    if (cfg->edge_right >= STICK_BIND_COUNT)
    {
        cfg->edge_right = STICK_BIND_NONE;
        ok = false;
    }

    /* Sustained scrolling. Zero sustain_ms is the honest way to say off;
     * anything else has to be long enough not to fire during an ordinary
     * pause mid-drag, and the rate has to run upward. */
    if (cfg->sustain_ms != 0 && (cfg->sustain_ms < 150 || cfg->sustain_ms > 5000))
    {
        cfg->sustain_ms = def.sustain_ms;
        ok = false;
    }
    if (cfg->sustain_slop_px < 0 || cfg->sustain_slop_px > lcd_h / 8)
    {
        cfg->sustain_slop_px = def.sustain_slop_px;
        ok = false;
    }
    if (cfg->sustain_edge_px < 0 || cfg->sustain_edge_px > lcd_h / 4)
    {
        cfg->sustain_edge_px = def.sustain_edge_px;
        ok = false;
    }
    if (cfg->sustain_v0 < 1 || cfg->sustain_vmax < cfg->sustain_v0 ||
        cfg->sustain_vmax > 400)
    {
        cfg->sustain_v0 = def.sustain_v0;
        cfg->sustain_vmax = def.sustain_vmax;
        ok = false;
    }
    if (cfg->sustain_ramp_ms < 100 || cfg->sustain_ramp_ms > 20000)
    {
        cfg->sustain_ramp_ms = def.sustain_ramp_ms;
        ok = false;
    }

    /* Negative is nonsense; zero is the honest way to say "no coasting". */
    if (cfg->coast_min_px_s < 0 || cfg->coast_min_px_s > 4 * lcd_h)
    {
        cfg->coast_min_px_s = def.coast_min_px_s;
        ok = false;
    }
    /* A non-positive a0 with a zero a1 would never stop. */
    if (cfg->decel_a0 <= 0 || cfg->decel_a1 < 0)
    {
        cfg->decel_a0 = def.decel_a0;
        cfg->decel_a1 = def.decel_a1;
        ok = false;
    }

    return ok;
}

bool stick_config_is_inert(const struct stick_config *cfg)
{
    int i;

    if (cfg->dial != STICK_DIAL_OFF)
        return false;
    if (cfg->centre != STICK_BIND_NONE)
        return false;
    for (i = 0; i < cfg->sectors && i < STICK_MAX_SECTORS; i++)
        if (cfg->bind[i] != STICK_BIND_NONE || cfg->hold[i] != STICK_BIND_NONE)
            return false;

    return true;
}

/* ---------------------------------------------------------------- sectors */

int stick_sector_at(const struct stick_config *cfg, int dx, int dy)
{
    int32_t a, half;
    int n = cfg->sectors;

    if (n < 1)
        return -1;
    if (n == 1)
        return 0;   /* the whole disc is one sector: hold-only, no direction */

    a = stick_atan2_deg64(dx, dy) - (int32_t)cfg->rotation * 64;
    half = DEG64_FULL / (2 * n);
    a = ((a + half) % DEG64_FULL + DEG64_FULL) % DEG64_FULL;

    return (int)((a * n) / DEG64_FULL) % n;
}

/* ----------------------------------------------------------------- engine */

static void out_clear(struct stick_output *out)
{
    stick_zero(out, sizeof(*out));
    out->action = STICK_ACT_NONE;
}

void stick_reset(struct stick_state *st, const struct stick_config *cfg)
{
    stick_zero(st, sizeof(*st));
    st->cfg = cfg;
    st->phase = STICK_PHASE_IDLE;
    st->sector = -1;
    st->last_angle = STICK_ANGLE_UNSET;
}

/* Undo everything a live dial has applied, so a cancelled spin reverts to
 * the value it had at arm time rather than to the last detent - spec 6. */
static void dial_revert(struct stick_state *st, struct stick_output *out)
{
    if (st->detents != 0)
    {
        out->action = STICK_ACT_DIAL;
        out->dial_steps = -st->detents;
        st->detents = 0;
    }
}

static void end_gesture(struct stick_state *st)
{
    st->phase = STICK_PHASE_IDLE;
    st->sector = -1;
    st->deflection = 0;
    st->dial = false;
    st->fired = false;
    st->vel_px_s = 0;
    st->coast_sub_px = 0;
    st->held_fired = false;
    st->held_bind = STICK_BIND_NONE;
    st->detents = 0;
    st->acc_angle = 0;
    st->last_angle = STICK_ANGLE_UNSET;
    st->repeat_acc = 0;
    st->scroll_ref_y = 0;
    st->scroll_dir = 0;
    st->scroll_turn = 0;
    st->sustain_t0 = 0;
    st->sustain_ref_y = 0;
    st->sustain_acc_q8 = 0;
    st->max_travel = 0;
}

static bool phase_is_live(int phase)
{
    return phase == STICK_PHASE_ARMING || phase == STICK_PHASE_ARMED ||
           phase == STICK_PHASE_DIAL   || phase == STICK_PHASE_CANCELLED ||
           phase == STICK_PHASE_EDGE;
}

static void begin(struct stick_state *st, int x, int y, long now,
                  struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;

    end_gesture(st);

    st->ox = st->x = x;
    st->oy = st->y = y;
    st->scroll_ref_y = (int16_t)y;
    st->t0 = st->t_last = st->t_sector = now;

    /* An edge strip is decided before anything else. It is the one gesture
     * that is allowed to mean something other than the stick, so it has to
     * be claimed before the arm zone can claim the press. */
    if (cfg->edge_px > 0 &&
        (x < cfg->edge_px ||
         (cfg->panel_w > 0 && x >= cfg->panel_w - cfg->edge_px)))
    {
        st->phase = STICK_PHASE_EDGE;
        out->action = STICK_ACT_NONE;
        return;
    }

    /* Dead beats everything else - spec 2. */
    if (in_dead(cfg, x, y))
    {
        st->phase = STICK_PHASE_DEAD;
        out->action = STICK_ACT_DROP;
        out->cue = STICK_CUE_REJECT;
        return;
    }

    if (!stick_zone_contains(&cfg->arm, x, y))
    {
        /* Plain absolute touch for its whole life. It never becomes a
         * stick, however far it travels. */
        st->phase = STICK_PHASE_ABSOLUTE;
        out->action = STICK_ACT_PASS;
        out->tap_x = x;
        out->tap_y = y;
        return;
    }

    zone_centre(&cfg->arm, &st->cx, &st->cy);
    /* A dial armed by holding still does not own the gesture yet; it is
     * offered later, from the ARMED phase, if the thumb stays put. */
    st->dial = (cfg->dial != STICK_DIAL_OFF) && !cfg->dial_on_hold;
    st->phase = STICK_PHASE_ARMING;
    out->action = STICK_ACT_NONE;
}

/* Resolve the sector and deflection for the current point, updating the
 * cue when the sector changes. */
static void update_sector(struct stick_state *st, struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    int dx = st->x - st->ox;
    int dy = st->y - st->oy;
    int32_t r = stick_hypot(dx, dy);
    int s;

    if (r < cfg->detent_px)
    {
        if (st->sector != -1)
        {
            if (st->held_fired)
            {
                out->hold_end = st->held_bind;
                st->held_fired = false;
                st->held_bind = STICK_BIND_NONE;
            }
            st->sector = -1;
            st->repeat_acc = 0;
        }
        st->deflection = 0;
        return;
    }

    s = stick_sector_at(cfg, dx, dy);

    /* Sector hysteresis. A thumb sweeps in an arc, not a straight line, so
     * a drag meant as "up" drifts sideways as it goes; against a hard
     * boundary that drift crosses into the neighbouring sector and fires
     * back or forward in the middle of a scroll. Hold the sector the
     * gesture is already in until another wins by sector_hyst_deg. */
    if (s != st->sector && st->sector >= 0 && cfg->sector_hyst_deg > 0 &&
        cfg->sectors > 1)
    {
        int32_t centre = ((int32_t)cfg->rotation * 64) +
                         ((int32_t)st->sector * 360 * 64) / cfg->sectors;
        int32_t delta = stick_atan2_deg64(dx, dy) - centre;
        int32_t half = (360 * 64) / (2 * cfg->sectors);

        /* Shortest way round, in 1/64 degree. */
        while (delta >  180 * 64) delta -= 360 * 64;
        while (delta < -180 * 64) delta += 360 * 64;
        if (delta < 0)
            delta = -delta;

        if (delta <= half + (int32_t)cfg->sector_hyst_deg * 64)
            s = st->sector;
    }

    if (r >= cfg->travel_px)
        st->deflection = 255;
    else
        st->deflection = (uint8_t)(((r - cfg->detent_px) * 255) /
                                   (cfg->travel_px - cfg->detent_px));

    if (s != st->sector)
    {
        /* Leaving a sector mid-hold ends that hold: a thumb that slides
         * from rewind to fast-forward must stop rewinding first. */
        if (st->held_fired)
        {
            out->hold_end = st->held_bind;
            st->held_fired = false;
            st->held_bind = STICK_BIND_NONE;
        }
        st->sector = (int8_t)s;
        st->repeat_acc = 0;
        st->t_sector = st->t_last;
        if (out->cue == STICK_CUE_NONE)
            out->cue = STICK_CUE_SECTOR;
    }
}

/* ----------------------------------------------------------- the coast */

/* Pixels of travel worth one row right now.
 *
 * cfg->scroll_px is the slow baseline; the current vertical speed buys
 * gain on top of it, capped at accel_max_q8. This is the whole of the
 * "long list" answer: the same gesture is precise when the thumb creeps
 * and long-throw when it does not, with no knowledge of which screen is
 * up or how many rows it holds.
 *
 * The coast uses the same function with its own decaying velocity, so a
 * throw starts fast and lands precise without a second rule. */
int stick_effective_scroll_px(const struct stick_state *st)
{
    const struct stick_config *cfg = st->cfg;
    int base = cfg->scroll_px > 0 ? cfg->scroll_px : STICK_DEF_SCROLL_PX;
    int32_t v0 = cfg->accel_v0 > 0 ? cfg->accel_v0 : STICK_DEF_ACCEL_V0;
    int32_t cap = cfg->accel_max_q8 >= 256 ? cfg->accel_max_q8
                                           : STICK_DEF_ACCEL_MAX_Q8;
    int32_t v = st->vel_px_s < 0 ? -st->vel_px_s : st->vel_px_s;
    int32_t gain_q8 = 256 + (v * 256) / v0;
    int step;

    if (gain_q8 > cap)
        gain_q8 = cap;

    step = (int)(((int32_t)base * 256) / gain_q8);
    if (step < STICK_MIN_EFF_STEP_PX)
        step = STICK_MIN_EFF_STEP_PX;
    return step;
}

/* Emit the whole scroll steps a signed pixel displacement is worth, in the
 * inverted sense the drag uses: content follows the thumb, so pushing the
 * view up walks the selection down. Returns the pixels consumed, signed,
 * so the caller can keep its own reference point honest without having to
 * recompute the effective step. */
static int emit_scroll_px(struct stick_state *st, int dy_px,
                          struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    int step = stick_effective_scroll_px(st);
    int dir = (dy_px < 0) ? -1 : (dy_px > 0) ? 1 : 0;
    int n;

    /* The opening direction is confirmed the same way a reversal is.
     * Until the latch holds a direction, one sample decides it - and a
     * quick drag sometimes opens with a single large report the wrong way
     * before the contact settles, which became a step backwards with
     * nothing standing in its way. Waiting for the next event to agree
     * costs one event of latency at the start of a drag and nothing else:
     * unlike a reversal this does not re-anchor, so the travel is not
     * lost, only not spent yet. A lone spurious sample has no next event
     * to agree with it and dies here. */
    if (dir != 0 && st->scroll_dir == 0 && st->scroll_turn != dir)
    {
        st->scroll_turn = (int8_t)dir;
        return 0;
    }

    /* Direction latch. Once a scroll has committed to a way up, a small
     * reversal is the thumb settling, not an instruction: a decelerating
     * throw rocks back a few pixels, and at the fast end of the
     * acceleration range the effective step is small enough that those
     * pixels were emitting a step the wrong way. Coming back
     * scroll_reverse_px really is a change of mind. */
    if (dir != 0 && st->scroll_dir != 0 && dir != st->scroll_dir &&
        cfg->scroll_reverse_px > 0)
    {
        int mag = dy_px < 0 ? -dy_px : dy_px;

        if (mag < cfg->scroll_reverse_px)
            return 0;

        /* Past the threshold it looks like a real change of mind - but a
         * single bogus sample from the panel is also past it, and those
         * arrive without warning on the way to a release. So the first
         * such event only *arms* the turn: it re-anchors and emits
         * nothing. A genuine reversal keeps going and the next event
         * agrees, at a cost of one event of latency; a lone spurious
         * sample has no next event to agree with it and dies here.
         *
         * Note it deliberately does not touch scroll_dir, so a stray
         * sample cannot hand a backwards direction to the coast either. */
        if (st->scroll_turn != dir)
        {
            st->scroll_turn = (int8_t)dir;
            return dy_px;
        }
    }

    if (dy_px <= -step)
    {
        n = (-dy_px) / step;
        out->binding = STICK_BIND_SCROLL_DOWN;
    }
    else if (dy_px >= step)
    {
        n = dy_px / step;
        out->binding = STICK_BIND_SCROLL_UP;
    }
    else
        return 0;

    out->action = STICK_ACT_FIRE;
    out->dial_steps = (int16_t)n;
    out->repeating = true;
    out->cue = STICK_CUE_DETENT;
    st->fired = true;
    st->scroll_dir = (int8_t)dir;
    st->scroll_turn = 0;
    return (dy_px < 0) ? -(n * step) : (n * step);
}

/* Track how fast the thumb is moving vertically, so a release can hand that
 * speed to the coast. Smoothed the way the list's own gesture tracker is:
 * enough history that one noisy sample cannot define the throw. */
static void track_velocity(struct stick_state *st, int dy, long dt)
{
    int32_t inst;

    if (dt <= 0)
        return;

    inst = ((int32_t)dy * 1000) / (int32_t)dt;
    /* Three parts new to one part old: responsive, but a single jittery
     * sample cannot own the result. */
    st->vel_px_s = (st->vel_px_s + 3 * inst) / 4;
}

/* One tick of a list that is still moving after the thumb has gone. Same
 * shape as the list's kinetic scroll - a linear v = a*t decay with
 * a = a0 + a1*|v| - and fed from the same settings, so a coast started by
 * the stick winds down exactly like one started by a swipe. */
static void advance_coast(struct stick_state *st, long dt,
                          struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    int32_t v = st->vel_px_s;
    int32_t decel, moved;

    if (dt <= 0)
        return;
    if (dt > MAX_DT_MS)
        dt = MAX_DT_MS;

    /* Distance this tick, carrying the sub-pixel remainder forward so a
     * slow coast still creeps rather than stalling. */
    st->coast_sub_px += (v * (int32_t)dt) / 1000;
    moved = st->coast_sub_px;

    if (moved != 0)
        st->coast_sub_px -= emit_scroll_px(st, (int)moved, out);

    decel = cfg->decel_a0 + ((cfg->decel_a1 * (v < 0 ? -v : v)) >> 8);
    decel = (decel * (int32_t)dt) / 1000;

    if (v < 0)
        v = (-v <= decel) ? 0 : v + decel;
    else
        v = (v <= decel) ? 0 : v - decel;

    st->vel_px_s = v;

    if (v == 0 || (v > -STICK_COAST_STOP_PX_S && v < STICK_COAST_STOP_PX_S))
    {
        end_gesture(st);
        if (out->cue == STICK_CUE_NONE)
            out->cue = STICK_CUE_COMMIT;
    }
}

/* Scrolling is a drag, not a held key.
 *
 * The thumb pushes the list the way a finger pushes paper: dragging up
 * moves the content up, which walks the selection *down* the list, and the
 * moment the thumb stops moving the list stops with it. That is why this
 * counts pixels travelled rather than time held - a held-but-still thumb
 * emits nothing at all.
 *
 * Returns true if it handled the binding, so the time-based repeat below
 * is left for the things that really are held keys, like volume. */
/* Is the thumb asking for a rate rather than a distance? Two ways to say
 * so, and they are the same statement: the stick is as far over as it can
 * go. Usually that means full deflection; near the top or bottom of the
 * panel it means the glass ran out first, which is the case a long list
 * produces constantly and the case plain deflection cannot see.
 *
 * The rim only ever qualifies a direction the drag already committed to.
 * Reading a direction *from* the rim would make the same thumb position
 * mean opposite things in a drag and in a sustain, which is not something
 * anyone could learn. */
static bool sustain_pinned(const struct stick_state *st, int dir)
{
    const struct stick_config *cfg = st->cfg;

    if (stick_hypot(st->x - st->ox, st->y - st->oy) >= cfg->travel_px)
        return true;

    if (cfg->sustain_edge_px > 0 && cfg->panel_h > 0)
    {
        if (dir < 0 && st->y <= cfg->sustain_edge_px)
            return true;
        if (dir > 0 && st->y >= cfg->panel_h - cfg->sustain_edge_px)
            return true;
    }
    return false;
}

/* The ramp itself. Rows per second, from a walk to a run over
 * sustain_ramp_ms, and it emits the same scroll bindings the drag does -
 * so nothing downstream can tell the difference, and the engine still
 * never learns how long the list is. */
static bool advance_sustain(struct stick_state *st, long dt,
                            struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    long held;
    int32_t rate, span, rows;
    int moved, dir;

    /* Usually the latch already holds the direction. It will not when the
     * drag never travelled far enough to emit a step - which is exactly
     * the rim case, where the glass ran out after forty pixels and one
     * scroll step is a hundred and forty. Fall back to the displacement
     * from the touch-down point, which is what armed the sector in the
     * first place. That reading is unsafe at the opening of a gesture, a
     * single stray sample being enough to decide it; it is safe here,
     * because nothing reaches this line until the thumb has held the same
     * spot for sustain_ms. */
    dir = st->scroll_dir;
    if (dir == 0)
        dir = (st->y < st->oy) ? -1 : (st->y > st->oy) ? 1 : 0;

    if (cfg->sustain_ms == 0 || dir == 0 || !sustain_pinned(st, dir))
    {
        st->sustain_t0 = 0;
        st->sustain_acc_q8 = 0;
        return false;
    }

    /* Still means still. A thumb that is still travelling is scrolling by
     * the drag, and the two must never both count - that would be the
     * list moving at the sum of two rates for as long as the user keeps
     * pushing. */
    moved = st->y - st->sustain_ref_y;
    if (moved < 0)
        moved = -moved;
    if (st->sustain_t0 == 0 || moved > cfg->sustain_slop_px)
    {
        st->sustain_t0 = st->t_last;
        st->sustain_ref_y = st->y;
        st->sustain_acc_q8 = 0;
        return false;
    }

    held = st->t_last - st->sustain_t0;
    if (held < (long)cfg->sustain_ms)
        return false;

    held -= cfg->sustain_ms;
    span = cfg->sustain_ramp_ms > 0 ? cfg->sustain_ramp_ms : 1;
    if (held > span)
        held = span;
    rate = cfg->sustain_v0 +
           ((int32_t)(cfg->sustain_vmax - cfg->sustain_v0) * (int32_t)held) / span;

    st->sustain_acc_q8 += (rate * (int32_t)dt * 256) / 1000;
    rows = st->sustain_acc_q8 >> 8;
    if (rows <= 0)
        return true;              /* the ramp owns the tick regardless */
    st->sustain_acc_q8 -= rows << 8;

    out->action = STICK_ACT_FIRE;
    out->binding = dir < 0 ? STICK_BIND_SCROLL_DOWN : STICK_BIND_SCROLL_UP;
    out->dial_steps = (int16_t)rows;
    out->repeating = true;
    out->cue = STICK_CUE_DETENT;
    st->fired = true;
    return true;
}

static bool advance_scroll(struct stick_state *st, long dt, int binding,
                           struct stick_output *out)
{
    int dy;

    if (binding != STICK_BIND_SCROLL_UP && binding != STICK_BIND_SCROLL_DOWN)
        return false;

    dy = st->y - st->scroll_ref_y;

    /* Whole steps only, and all of them at once: leaving a backlog behind
     * would drain after the thumb had already stopped, which is the exact
     * run-on this is meant to remove. The coast uses the same emitter, so
     * the two can never disagree about which way up is, nor about how many
     * pixels a row is worth at the current speed. */
    st->scroll_ref_y += (int16_t)emit_scroll_px(st, dy, out);

    /* Only when the drag itself had nothing to say this tick. */
    if (out->action == STICK_ACT_NONE)
        advance_sustain(st, dt, out);
    return true;
}

/* The slow tap: a thumb that goes down and stays down, inside the detent
 * radius, for dial_arm_ms. That arms the dial for the rest of the gesture,
 * so the user can then circle to change the volume the way a click wheel
 * works. The cue ticks while the hold builds, because a three second wait
 * with no feedback is indistinguishable from nothing happening.
 *
 * Returns true when it owns this tick. The moment the thumb travels out of
 * the detent radius, the gesture is an ordinary sector gesture again and
 * this stays out of the way for good.
 */
static bool advance_dial_arm(struct stick_state *st, long now_ms,
                             struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    long held;
    int32_t r;

    if (!cfg->dial_on_hold || cfg->dial == STICK_DIAL_OFF || st->dial)
        return false;

    r = stick_hypot(st->x - st->ox, st->y - st->oy);
    if (r >= cfg->detent_px)
        return false;           /* travelled: this is a sector gesture */

    held = now_ms - st->t0;
    if (held < (long)cfg->dial_arm_ms)
    {
        /* Tick along, one cue per STICK_DIAL_TICK_MS, so the wait has a
         * shape to it. t_sector is free here - no sector is live during a
         * slow tap - so it carries the last tick. */
        if (now_ms - st->t_sector >= STICK_DIAL_TICK_MS)
        {
            st->t_sector = now_ms;
            if (out->cue == STICK_CUE_NONE)
                out->cue = STICK_CUE_DETENT;
        }
        return true;
    }

    st->dial = true;
    st->phase = STICK_PHASE_DIAL;
    /* The pivot starts under the thumb, not at the arm zone centre. From
     * here it floats (see advance_dial): the user can slide anywhere before
     * they start circling and the dial goes with them. */
    st->cx = st->x;
    st->cy = st->y;
    st->last_angle = STICK_ANGLE_UNSET;
    st->acc_angle = 0;
    st->detents = 0;
    out->cue = STICK_CUE_COMMIT;
    return true;
}

/* A sector carrying a hold binding is quiet until hold_ms has passed, then
 * fires that binding once. Returns true whenever the hold path owns the
 * sector, so the ordinary repeat stays out of it; the short-drag binding
 * for such a sector is left to arbitration on release. */
static bool advance_hold(struct stick_state *st, struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    int hold;

    if (st->sector < 0)
        return false;

    hold = cfg->hold[st->sector];
    if (hold == STICK_BIND_NONE)
        return false;

    if (st->held_fired)
        return true;
    if (st->t_last - st->t_sector < (long)cfg->hold_ms)
        return true;

    st->held_fired = true;
    st->held_bind = (uint8_t)hold;
    st->fired = true;            /* release adds nothing on top of it */
    out->action = STICK_ACT_FIRE;
    out->binding = (uint8_t)hold;
    out->cue = STICK_CUE_COMMIT;
    return true;
}

static void advance_repeat(struct stick_state *st, long dt,
                           struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    int binding;
    uint32_t d, rate_q8;

    if (st->sector < 0)
        return;

    binding = cfg->bind[st->sector];
    if (binding == STICK_BIND_NONE || !stick_binding_repeats(binding))
        return;

    if (advance_scroll(st, dt, binding, out))
        return;

    if (st->t_last - st->t_sector < (long)cfg->repeat_delay_ms)
        return;

    /* rate = 2 + 14 * deflection^2 per second, in Q8. */
    d = st->deflection;
    rate_q8 = (2u << 8) + (uint32_t)(((14u << 8) * d * d) / (255u * 255u));

    st->repeat_acc += (int32_t)((rate_q8 * (uint32_t)dt) / 1000u);

    if (st->repeat_acc >= 256)
    {
        st->repeat_acc -= 256;
        /* One emit per tick is always enough: the maximum rate is 16/s
         * against a 25 Hz tick. Do not let a backlog build up. */
        if (st->repeat_acc > 256)
            st->repeat_acc = 256;

        out->action = STICK_ACT_FIRE;
        out->binding = (uint8_t)binding;
        out->repeating = true;
        out->cue = STICK_CUE_DETENT;
        st->fired = true;
    }
}

static void advance_dial(struct stick_state *st, struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    int dx = st->x - st->cx;
    int dy = st->y - st->cy;
    int32_t per = (int32_t)cfg->deg_per_detent * 64;
    int32_t a, delta;
    int32_t r;
    int steps = 0;

    /* The floating pivot. Once the thumb is further out than dial_max_px,
     * the pivot is pulled straight along the line joining the two until it
     * is exactly that far behind - which leaves the bearing from pivot to
     * thumb unchanged, so dragging the pivot never manufactures a detent.
     *
     * What it buys is that a straight slide, however long, is all radius
     * and no angle: the pivot simply follows. Only turning changes the
     * bearing. The dial therefore belongs wherever the user starts circling
     * rather than wherever they happened to arm it, which is the whole
     * point - arming near an edge used to leave nowhere to draw. */
    r = stick_hypot(dx, dy);
    if (cfg->dial_max_px > 0 && r > cfg->dial_max_px)
    {
        int32_t pull = r - cfg->dial_max_px;

        st->cx = (int16_t)(st->cx + (int32_t)dx * pull / r);
        st->cy = (int16_t)(st->cy + (int32_t)dy * pull / r);
        dx = st->x - st->cx;
        dy = st->y - st->cy;
        r = stick_hypot(dx, dy);
    }

    /* The jitter floor: below this radius the angle is not read at all. */
    if (r < cfg->dial_min_px)
        return;

    a = stick_atan2_deg64(dx, dy);

    if (st->last_angle == STICK_ANGLE_UNSET)
    {
        st->last_angle = a;
        return;
    }

    delta = a - st->last_angle;
    if (delta > DEG64_FULL / 2)
        delta -= DEG64_FULL;
    if (delta < -DEG64_FULL / 2)
        delta += DEG64_FULL;

    st->last_angle = a;
    st->acc_angle += delta;

    /* Unlimited re-circling: the accumulator is never clamped to one
     * sweep, so three loops is a hundred-odd detents in one press. */
    while (st->acc_angle >= per)
    {
        st->acc_angle -= per;
        steps++;
    }
    while (st->acc_angle <= -per)
    {
        st->acc_angle += per;
        steps--;
    }

    if (steps)
    {
        st->detents += steps;
        out->action = STICK_ACT_DIAL;
        out->dial_steps = (int16_t)steps;
        out->cue = STICK_CUE_DETENT;
    }
}

/* Arbitration at release. */
static void arbitrate(struct stick_state *st, struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;
    bool in_window = (st->t_last - st->t0) < (long)cfg->arm_ms;
    bool travelled = st->max_travel >= cfg->tap_slop_px;
    int binding;

    /* A hold that was running has to be told to stop however the gesture
     * ended - cancel included, since a thumb leaving the work zone mid-seek
     * must not leave the track seeking forever. */
    bool had_hold = st->held_fired;

    if (had_hold)
    {
        out->hold_end = st->held_bind;
        st->held_fired = false;
    }

    if (st->phase == STICK_PHASE_CANCELLED)
    {
        /* Nothing else is committed, in every state, including mid-dial. */
        dial_revert(st, out);
        out->cue = STICK_CUE_REJECT;
        return;
    }

    if (st->dial)
    {
        if (st->detents == 0 && !travelled)
        {
            /* No spin at all: the centre binding, whether or not the
             * release beat the arm window. */
            binding = cfg->centre;
            if (binding != STICK_BIND_NONE)
            {
                out->action = STICK_ACT_FIRE;
                out->binding = (uint8_t)binding;
                out->cue = STICK_CUE_COMMIT;
            }
            return;
        }
        out->cue = STICK_CUE_COMMIT;
        return;
    }

    if (!travelled && in_window)
    {
        /* A release inside the window with no travel used to be replayed as
         * an absolute touch at the press point. It no longer is: an
         * absolute hit test inside a relative scheme meant a tap selected
         * whatever row sat under the thumb, which is what the stick exists
         * to replace. A tap is the centre binding, fast or slow. */
        if (cfg->centre != STICK_BIND_NONE)
        {
            out->action = STICK_ACT_FIRE;
            out->binding = cfg->centre;
            out->cue = STICK_CUE_COMMIT;
        }
        return;
    }

    if (st->sector >= 0)
    {
        /* The hold already spoke for this gesture. Firing the short-drag
         * binding as well would mean a held rewind also skipped a track. */
        if (had_hold)
        {
            out->cue = STICK_CUE_COMMIT;
            return;
        }

        binding = cfg->bind[st->sector];
        if (binding == STICK_BIND_NONE)
            return;
        if (stick_binding_repeats(binding))
        {
            /* Repeating bindings already fired while held; release adds
             * nothing - spec 4. */
            if (st->fired)
                out->cue = STICK_CUE_COMMIT;
            else
            {
                /* Released before the repeat delay elapsed: a flick still
                 * deserves its one step. */
                out->action = STICK_ACT_FIRE;
                out->binding = (uint8_t)binding;
                out->repeating = true;
                out->cue = STICK_CUE_COMMIT;
            }
            return;
        }
        out->action = STICK_ACT_FIRE;
        out->binding = (uint8_t)binding;
        out->cue = STICK_CUE_COMMIT;
        return;
    }

    /* Inside the detent radius after the window closed: the centre entry. */
    if (!travelled && !st->fired && cfg->centre != STICK_BIND_NONE)
    {
        out->action = STICK_ACT_FIRE;
        out->binding = cfg->centre;
        out->cue = STICK_CUE_COMMIT;
    }
}

void stick_process(struct stick_state *st, int type, int x, int y,
                   long now_ms, struct stick_output *out)
{
    const struct stick_config *cfg;
    long dt;
    bool outside;

    out_clear(out);

    if (!st || !st->cfg)
        return;
    cfg = st->cfg;

    /* A coast is the one state that outlives the thumb, so it is handled
     * before everything below, all of which assumes a contact. */
    if (st->phase == STICK_PHASE_COAST)
    {
        if (type == STICK_TOUCH_PRESS)
        {
            /* Catching a moving list stops it dead, which is the gesture
             * everyone already expects from a touchscreen. */
            end_gesture(st);
            begin(st, x, y, now_ms, out);
            return;
        }
        if (type == STICK_TOUCH_IDLE)
        {
            long cdt = now_ms - st->t_last;
            st->t_last = now_ms;
            advance_coast(st, cdt, out);
        }
        return;
    }

    /* A lost release - panel glitch, suspend - is reaped as a cancel,
     * never as a commit - spec 9. */
    if (phase_is_live(st->phase) && (now_ms - st->t_last) > STICK_TIMEOUT_MS)
    {
        dial_revert(st, out);
        out->cue = STICK_CUE_REJECT;
        end_gesture(st);
        if (type != STICK_TOUCH_PRESS)
            return;
    }

    switch (type)
    {
    case STICK_TOUCH_IDLE:
        if (st->phase == STICK_PHASE_ABSOLUTE)
            out->action = STICK_ACT_PASS;
        return;

    case STICK_TOUCH_PRESS:
        /* Second and subsequent contacts are ignored while a gesture is
         * live - spec 9. Not merged in as a move: that would drag the
         * first thumb's gesture over to the second thumb's coordinates. */
        if (st->phase != STICK_PHASE_IDLE)
        {
            st->t_last = now_ms;
            if (st->phase == STICK_PHASE_ABSOLUTE)
                out->action = STICK_ACT_PASS;
            else if (st->phase == STICK_PHASE_DEAD)
                out->action = STICK_ACT_DROP;
            return;
        }
        begin(st, x, y, now_ms, out);
        return;

    default:
        break;
    }

    if (st->phase == STICK_PHASE_IDLE)
    {
        /* A contact or release with no press behind it. Nothing to do; an
         * isolated release is dropped by the caller anyway. */
        return;
    }

    dt = now_ms - st->t_last;
    if (dt < 0)
        dt = 0;
    if (dt > MAX_DT_MS)
        dt = MAX_DT_MS;
    st->t_last = now_ms;

    if (st->phase == STICK_PHASE_DEAD)
    {
        out->action = STICK_ACT_DROP;
        if (type == STICK_TOUCH_RELEASE)
            end_gesture(st);
        return;
    }

    if (st->phase == STICK_PHASE_ABSOLUTE)
    {
        st->x = (int16_t)x;
        st->y = (int16_t)y;
        out->action = STICK_ACT_PASS;
        out->tap_x = (int16_t)x;
        out->tap_y = (int16_t)y;
        if (type == STICK_TOUCH_RELEASE)
            end_gesture(st);
        return;
    }

    if (st->phase == STICK_PHASE_EDGE)
    {
        /* Inward travel, once, on the way in. Nothing waits for release:
         * the whole appeal of an edge swipe is that it commits the moment
         * it is unambiguous. */
        bool from_left = st->ox < cfg->edge_px;
        int bind = from_left ? cfg->edge_left : cfg->edge_right;
        int inward;

        st->x = (int16_t)x;
        st->y = (int16_t)y;
        inward = from_left ? (st->x - st->ox) : (st->ox - st->x);

        if (!st->fired && bind != STICK_BIND_NONE &&
            inward >= cfg->edge_travel_px)
        {
            st->fired = true;
            out->action = STICK_ACT_FIRE;
            out->binding = (uint8_t)bind;
            out->cue = STICK_CUE_COMMIT;
        }

        if (type == STICK_TOUCH_RELEASE)
            end_gesture(st);
        return;
    }

    /* Live stick gesture from here on. -------------------------------- */

    if (type == STICK_TOUCH_CONTACT)
    {
        int dx, dy;
        int32_t travel;

        track_velocity(st, y - st->y, dt);

        st->x = (int16_t)x;
        st->y = (int16_t)y;

        dx = st->x - st->ox;
        dy = st->y - st->oy;
        travel = stick_hypot(dx, dy);
        if (travel > st->max_travel)
            st->max_travel = (int16_t)travel;

        /* The work zone is tested on every move. Leaving it stops output
         * immediately; re-entering resumes - spec 2. */
        outside = !stick_zone_contains(&cfg->work, x, y);

        if (outside)
        {
            if (st->phase != STICK_PHASE_CANCELLED)
            {
                st->phase = STICK_PHASE_CANCELLED;
                st->sector = -1;
                st->deflection = 0;
                st->repeat_acc = 0;
                out->cue = STICK_CUE_REJECT;
            }
            return;
        }

        if (st->phase == STICK_PHASE_CANCELLED)
        {
            st->phase = st->dial ? STICK_PHASE_DIAL : STICK_PHASE_ARMED;
            /* Do not turn the trip outside into one huge angular delta. */
            st->last_angle = STICK_ANGLE_UNSET;
            /* Nor into a burst of scroll steps: the excursion happened
             * with output stopped, so scrolling restarts from here. And
             * the sustain clock restarts too - a thumb that left the work
             * zone and came back has not been parked all that while, and
             * without this it fires the ramp on the re-entry event itself.
             */
            st->scroll_ref_y = (int16_t)y;
            st->sustain_t0 = 0;
            st->sustain_acc_q8 = 0;
            /* Fall through: resolve the sector on the same tick we resume,
             * otherwise a release right after re-entry commits nothing. */
        }

        /* The arm window is a window, not a dwell: movement during it
         * accrues normally and output simply begins when it closes. */
        if (st->phase == STICK_PHASE_ARMING)
        {
            if (now_ms - st->t0 < (long)cfg->arm_ms)
                return;
            st->phase = st->dial ? STICK_PHASE_DIAL : STICK_PHASE_ARMED;
            st->t_sector = now_ms;
            out->cue = STICK_CUE_ARM;
        }

        if (st->phase == STICK_PHASE_DIAL)
            advance_dial(st, out);
        else if (advance_dial_arm(st, now_ms, out))
            ;   /* the slow tap is building, or has just armed the dial */
        else
        {
            update_sector(st, out);
            if (!advance_hold(st, out))
                advance_repeat(st, dt, out);
        }
        return;
    }

    if (type == STICK_TOUCH_RELEASE)
    {
        /* Release keeps the coordinates it already had: the panel often
         * reports rubbish on the way up. */
        if (st->phase == STICK_PHASE_ARMING && st->max_travel >= cfg->tap_slop_px)
        {
            /* Released inside the window but with travel: evaluate the
             * gesture once on the way out so a fast flick registers. */
            st->phase = st->dial ? STICK_PHASE_DIAL : STICK_PHASE_ARMED;
            if (!st->dial)
                update_sector(st, out);
            out->cue = STICK_CUE_NONE;
        }

        {
            /* Whether the thumb was still scrolling as it left. Read before
             * arbitration, which resolves and then clears the gesture. */
            bool was_scrolling =
                st->phase == STICK_PHASE_ARMED && st->sector >= 0 &&
                (cfg->bind[st->sector] == STICK_BIND_SCROLL_UP ||
                 cfg->bind[st->sector] == STICK_BIND_SCROLL_DOWN) &&
                cfg->hold[st->sector] == STICK_BIND_NONE;
            int32_t v = st->vel_px_s;
            int32_t av = v < 0 ? -v : v;
            /* The smoothed velocity can come out of a release pointing the
             * other way, because the last samples of a throw are the thumb
             * slowing and rocking back. Coasting on that sign threw the
             * list backwards - the same intermittent reversal the drag
             * latch fixes, arriving one event later. A coast has to agree
             * with the direction the drag actually committed to. */
            int8_t dir = st->scroll_dir;
            bool agrees = dir == 0 || (v < 0 ? dir < 0 : dir > 0);

            arbitrate(st, out);

            if (was_scrolling && agrees && cfg->coast_min_px_s > 0 &&
                av >= cfg->coast_min_px_s)
            {
                /* Let go of a moving list and it keeps going. Everything
                 * else about the gesture is finished, so only the speed
                 * and the pixel remainder survive the reset. */
                end_gesture(st);
                st->phase = STICK_PHASE_COAST;
                st->vel_px_s = v;
                st->scroll_dir = dir;   /* survives, so the coast cannot
                                           reverse either */
                st->coast_sub_px = 0;
                st->t_last = now_ms;
                return;
            }
        }

        end_gesture(st);
        return;
    }
}
