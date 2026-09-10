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

    cfg->dial = STICK_DIAL_OFF;
    cfg->deg_per_detent = STICK_DEF_DEG_PER_DETENT;
    cfg->arm_ms = STICK_DEF_ARM_MS;
    cfg->repeat_delay_ms = STICK_DEF_REPEAT_MS;
    cfg->detent_px = STICK_DEF_DETENT_PX;
    cfg->dial_min_px = STICK_DEF_DIAL_MIN_PX;
    cfg->travel_px = STICK_DEF_TRAVEL_PX;
    cfg->tap_slop_px = STICK_DEF_TAP_SLOP_PX;
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
    if (cfg->dial_min_px < 1 || cfg->dial_min_px > lcd_w)
    {
        cfg->dial_min_px = def.dial_min_px;
        ok = false;
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
        if (cfg->bind[i] != STICK_BIND_NONE)
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
    st->detents = 0;
    st->acc_angle = 0;
    st->last_angle = STICK_ANGLE_UNSET;
    st->repeat_acc = 0;
    st->max_travel = 0;
}

static bool phase_is_live(int phase)
{
    return phase == STICK_PHASE_ARMING || phase == STICK_PHASE_ARMED ||
           phase == STICK_PHASE_DIAL   || phase == STICK_PHASE_CANCELLED;
}

static void begin(struct stick_state *st, int x, int y, long now,
                  struct stick_output *out)
{
    const struct stick_config *cfg = st->cfg;

    end_gesture(st);

    st->ox = st->x = x;
    st->oy = st->y = y;
    st->t0 = st->t_last = st->t_sector = now;

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
    st->dial = (cfg->dial != STICK_DIAL_OFF);
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
            st->sector = -1;
            st->repeat_acc = 0;
        }
        st->deflection = 0;
        return;
    }

    s = stick_sector_at(cfg, dx, dy);

    if (r >= cfg->travel_px)
        st->deflection = 255;
    else
        st->deflection = (uint8_t)(((r - cfg->detent_px) * 255) /
                                   (cfg->travel_px - cfg->detent_px));

    if (s != st->sector)
    {
        st->sector = (int8_t)s;
        st->repeat_acc = 0;
        st->t_sector = st->t_last;
        if (out->cue == STICK_CUE_NONE)
            out->cue = STICK_CUE_SECTOR;
    }
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
    int steps = 0;

    /* The jitter floor: below this radius the angle is not read at all. */
    if (stick_hypot(dx, dy) < cfg->dial_min_px)
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

    if (st->phase == STICK_PHASE_CANCELLED)
    {
        /* Nothing is committed, in every state, including mid-dial. */
        dial_revert(st, out);
        out->cue = STICK_CUE_REJECT;
        return;
    }

    if (st->dial)
    {
        if (st->detents == 0 && !travelled)
        {
            /* No spin at all. Inside the arm window that is a plain tap;
             * after it, the centre binding. */
            if (in_window)
            {
                out->action = STICK_ACT_TAP;
                out->tap_x = st->ox;
                out->tap_y = st->oy;
                return;
            }
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
        /* Release inside the window with no travel is a plain tap:
         * an absolute hit test at the press point - spec 5. */
        out->action = STICK_ACT_TAP;
        out->tap_x = st->ox;
        out->tap_y = st->oy;
        return;
    }

    if (st->sector >= 0)
    {
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

    /* Live stick gesture from here on. -------------------------------- */

    if (type == STICK_TOUCH_CONTACT)
    {
        int dx, dy;
        int32_t travel;

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
        else
        {
            update_sector(st, out);
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

        arbitrate(st, out);
        end_gesture(st);
        return;
    }
}
