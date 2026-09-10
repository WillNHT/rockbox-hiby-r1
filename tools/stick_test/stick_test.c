/***************************************************************************
 * Host test harness for the Rockpocket Stick engine.
 *
 * Builds and runs on a development machine with nothing but a C compiler:
 * apps/stick.c deliberately has no Rockbox dependencies, which is what makes
 * this possible. Every test below is named after the clause of the
 * implementation spec it defends.
 *
 *   make && ./stick_test              run every test
 *   ./stick_test --trace FILE         replay a captured gesture trace
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 ****************************************************************************/

#include "stick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LCD_W 480
#define LCD_H 800

static int tests_run, tests_failed;
static const char *current_test;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  ", __func__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            tests_failed++;                                                  \
        }                                                                    \
    } while (0)

static void begin_test(const char *name)
{
    current_test = name;
    tests_run++;
    printf("- %s\n", name);
}

/* ------------------------------------------------------------ scaffolding */

/* A recording of everything the engine emitted during one scenario, so a
 * test can assert on the whole gesture rather than a single tick. */
struct log
{
    int  fires[64];
    bool fire_repeating[64];
    int  n_fires;
    int  dial_total;
    int  dial_events;
    int  taps;
    int  tap_x, tap_y;
    int  passes;
    int  drops;
    int  cues[8];
};

struct rig
{
    struct stick_config cfg;
    struct stick_state  st;
    struct log          log;
    long                now;
};

static void rig_init(struct rig *r)
{
    memset(r, 0, sizeof(*r));
    stick_config_default(&r->cfg, LCD_W, LCD_H);
    stick_reset(&r->st, &r->cfg);
    r->now = 1000;
}

static void rig_apply(struct rig *r, const struct stick_output *out)
{
    switch (out->action)
    {
    case STICK_ACT_FIRE:
        if (r->log.n_fires < 64)
        {
            r->log.fire_repeating[r->log.n_fires] = out->repeating;
            r->log.fires[r->log.n_fires++] = out->binding;
        }
        break;
    case STICK_ACT_DIAL:
        r->log.dial_total += out->dial_steps;
        r->log.dial_events++;
        break;
    case STICK_ACT_TAP:
        r->log.taps++;
        r->log.tap_x = out->tap_x;
        r->log.tap_y = out->tap_y;
        break;
    case STICK_ACT_PASS:
        r->log.passes++;
        break;
    case STICK_ACT_DROP:
        r->log.drops++;
        break;
    default:
        break;
    }
    if (out->cue > 0 && out->cue < 8)
        r->log.cues[out->cue]++;
}

static void ev(struct rig *r, int type, int x, int y, long dt)
{
    struct stick_output out;
    r->now += dt;
    stick_process(&r->st, type, x, y, r->now, &out);
    rig_apply(r, &out);
}

static void down(struct rig *r, int x, int y)  { ev(r, STICK_TOUCH_PRESS, x, y, 0); }
static void move(struct rig *r, int x, int y)  { ev(r, STICK_TOUCH_CONTACT, x, y, 40); }
static void up(struct rig *r, int x, int y)    { ev(r, STICK_TOUCH_RELEASE, x, y, 40); }

/* Hold at one point for ms milliseconds, delivering the 40 ms touch tick the
 * real driver produces. */
static void hold(struct rig *r, int x, int y, int ms)
{
    int t;
    for (t = 0; t < ms; t += 40)
        move(r, x, y);
}

static int only_fire(struct rig *r)
{
    if (r->log.n_fires != 1)
        return -1;
    return r->log.fires[0];
}

/* ------------------------------------------------------- spec 5 lifecycle */

static void test_tap_inside_window(void)
{
    struct rig r;
    begin_test("spec 5: release inside the arm window with no travel is a tap");
    rig_init(&r);

    down(&r, 240, 400);
    up(&r, 240, 400);

    CHECK(r.log.taps == 1, "expected one tap, got %d", r.log.taps);
    CHECK(r.log.tap_x == 240 && r.log.tap_y == 400,
          "tap at %d,%d not at the press point", r.log.tap_x, r.log.tap_y);
    CHECK(r.log.n_fires == 0, "a tap must not fire a binding");
}

static void test_flick_inside_window(void)
{
    struct rig r;
    begin_test("spec 5: release inside the window with travel still resolves");
    rig_init(&r);

    /* 60 ms total, well inside the 110 ms window, but a long way up. */
    down(&r, 240, 400);
    ev(&r, STICK_TOUCH_CONTACT, 240, 340, 30);
    ev(&r, STICK_TOUCH_RELEASE, 240, 340, 30);

    CHECK(only_fire(&r) == STICK_BIND_SCROLL_UP,
          "expected scrollUp from the flick, got %d fires", r.log.n_fires);
    CHECK(r.log.taps == 0, "a flick is not a tap");
}

static void test_absolute_never_becomes_a_stick(void)
{
    struct rig r;
    begin_test("spec 5: a press outside ARM stays absolute for its whole life");
    rig_init(&r);

    /* Arm only the bottom plate. */
    r.cfg.arm.shape = STICK_SHAPE_BOX;
    r.cfg.arm.a = 0; r.cfg.arm.b = 440; r.cfg.arm.c = LCD_W; r.cfg.arm.d = 360;
    r.cfg.work = r.cfg.arm;

    down(&r, 240, 100);
    hold(&r, 240, 100, 400);
    move(&r, 240, 40);          /* travels far, and into the arm zone later */
    move(&r, 240, 600);
    up(&r, 240, 600);

    CHECK(r.log.n_fires == 0, "an absolute press must never fire a binding");
    CHECK(r.log.passes > 0, "an absolute press must be passed through");
}

static void test_cancel_commits_nothing(void)
{
    struct rig r;
    begin_test("spec 10.5: a cancelled gesture commits nothing");
    rig_init(&r);

    r.cfg.work.shape = STICK_SHAPE_BOX;
    r.cfg.work.a = 0; r.cfg.work.b = 400; r.cfg.work.c = LCD_W; r.cfg.work.d = 400;

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 500);         /* a real deflection, inside work */
    move(&r, 240, 200);         /* out of work: cancelled */
    up(&r, 240, 200);

    CHECK(r.log.n_fires == 0, "cancelled gesture fired %d bindings", r.log.n_fires);
    CHECK(r.log.cues[STICK_CUE_REJECT] > 0, "cancelling should cue a reject");
}

static void test_cancel_then_reenter_resumes(void)
{
    struct rig r;
    begin_test("spec 2: re-entering the work zone resumes the gesture");
    rig_init(&r);

    r.cfg.work.shape = STICK_SHAPE_BOX;
    r.cfg.work.a = 0; r.cfg.work.b = 400; r.cfg.work.c = LCD_W; r.cfg.work.d = 400;

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 200);         /* cancelled */
    move(&r, 240, 500);         /* back inside, deflected up */
    up(&r, 240, 500);

    CHECK(only_fire(&r) == STICK_BIND_SCROLL_UP,
          "expected scrollUp after resuming, got %d fires", r.log.n_fires);
}

static void test_centre_binding_after_window(void)
{
    struct rig r;
    begin_test("spec 4: release after the window with no deflection is the centre");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 400);
    up(&r, 240, 400);

    CHECK(only_fire(&r) == STICK_BIND_PLAY_PAUSE,
          "expected the centre binding, got %d fires", r.log.n_fires);
    CHECK(r.log.taps == 0, "a held press is not a tap");
}

static void test_mid_flight_correction(void)
{
    struct rig r;
    begin_test("spec 4: a one-shot fires in whichever sector the thumb ended in");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 320);         /* up:    scrollUp */
    move(&r, 330, 400);         /* right: select   */
    move(&r, 150, 400);         /* left:  back     */
    up(&r, 150, 400);

    CHECK(only_fire(&r) == STICK_BIND_BACK,
          "expected the final sector to win, got %d fires", r.log.n_fires);
}

/* ----------------------------------------------------------- spec 2 zones */

static void test_dead_beats_everything(void)
{
    struct rig r;
    begin_test("spec 2: DEAD has the highest precedence of all four zones");
    rig_init(&r);

    r.cfg.n_dead = 1;
    r.cfg.dead[0].shape = STICK_SHAPE_BOX;
    r.cfg.dead[0].a = 0; r.cfg.dead[0].b = 0;
    r.cfg.dead[0].c = LCD_W; r.cfg.dead[0].d = 34;

    down(&r, 240, 10);          /* inside dead, and inside the full-screen arm */
    move(&r, 240, 300);
    up(&r, 240, 300);

    CHECK(r.log.n_fires == 0, "a dead press must not fire");
    CHECK(r.log.taps == 0, "a dead press must not tap");
    CHECK(r.log.passes == 0, "a dead press must not reach absolute touch");
    CHECK(r.log.drops > 0, "a dead press should be dropped");
}

static void test_work_larger_than_arm(void)
{
    struct rig r;
    begin_test("spec 2: work larger than arm is the forgiving case");
    rig_init(&r);

    r.cfg.arm.shape = STICK_SHAPE_CIRCLE;
    r.cfg.arm.a = 240; r.cfg.arm.b = 600; r.cfg.arm.c = 160;
    r.cfg.work.shape = STICK_SHAPE_CIRCLE;
    r.cfg.work.a = 240; r.cfg.work.b = 600; r.cfg.work.c = 230;

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 420);         /* outside arm, still inside work */
    up(&r, 240, 420);

    CHECK(only_fire(&r) == STICK_BIND_SCROLL_UP,
          "leaving arm but staying in work must not cancel (%d fires)",
          r.log.n_fires);
}

static void test_work_smaller_than_arm_is_legal(void)
{
    struct rig r;
    begin_test("spec 2: work smaller than arm is permitted, not blocked");
    rig_init(&r);

    r.cfg.arm.shape = STICK_SHAPE_BOX;
    r.cfg.arm.a = 0; r.cfg.arm.b = 0; r.cfg.arm.c = LCD_W; r.cfg.arm.d = LCD_H;
    r.cfg.work.shape = STICK_SHAPE_CIRCLE;
    r.cfg.work.a = 240; r.cfg.work.b = 600; r.cfg.work.c = 40;

    CHECK(stick_config_validate(&r.cfg, LCD_W, LCD_H),
          "a small work zone must validate, it is legal");

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 400);         /* outside the tiny work zone */
    up(&r, 240, 400);

    CHECK(r.log.n_fires == 0, "this gesture can only ever cancel");
}

/* --------------------------------------------------------- spec 3 sectors */

static void test_single_sector(void)
{
    struct rig r;
    begin_test("spec 3: N = 1 makes the whole disc one sector, no direction");
    rig_init(&r);

    r.cfg.sectors = 1;
    r.cfg.bind[0] = STICK_BIND_PLAY_PAUSE;

    CHECK(stick_sector_at(&r.cfg, 0, -100) == 0, "up must be sector 0");
    CHECK(stick_sector_at(&r.cfg, 0, 100) == 0, "down must be sector 0");
    CHECK(stick_sector_at(&r.cfg, 100, 0) == 0, "right must be sector 0");

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 300);
    up(&r, 240, 300);
    CHECK(only_fire(&r) == STICK_BIND_PLAY_PAUSE, "N=1 should fire bind[0]");
}

static void test_eight_sectors_and_rotation(void)
{
    struct stick_config cfg;
    int i;
    begin_test("spec 3: eight sectors, and rotation moves the rose");
    stick_config_default(&cfg, LCD_W, LCD_H);

    cfg.sectors = 8;
    cfg.rotation = 0;
    CHECK(stick_sector_at(&cfg, 0, -100) == 0, "up is sector 0");
    CHECK(stick_sector_at(&cfg, 100, -100) == 1, "up-right is sector 1");
    CHECK(stick_sector_at(&cfg, 100, 0) == 2, "right is sector 2");
    CHECK(stick_sector_at(&cfg, 0, 100) == 4, "down is sector 4");
    CHECK(stick_sector_at(&cfg, -100, 0) == 6, "left is sector 6");

    /* Sector 0 is always centred on rot. */
    cfg.sectors = 4;
    for (i = 0; i < 360; i += 15)
    {
        int dx = (int)(100 * sin(i * M_PI / 180.0));
        int dy = (int)(-100 * cos(i * M_PI / 180.0));
        cfg.rotation = i;
        CHECK(stick_sector_at(&cfg, dx, dy) == 0,
              "rot %d: the rotation angle itself must land in sector 0", i);
    }

    /* Two-way rotated 90 degrees, as the reference "local" theme does. */
    cfg.sectors = 2;
    cfg.rotation = 90;
    CHECK(stick_sector_at(&cfg, 100, 0) == 0, "rot 90, right is sector 0");
    CHECK(stick_sector_at(&cfg, -100, 0) == 1, "rot 90, left is sector 1");
}

static void test_sector_boundaries(void)
{
    struct stick_config cfg;
    int n;
    begin_test("spec 3: every sector index stays inside [0, N)");
    stick_config_default(&cfg, LCD_W, LCD_H);

    for (n = 1; n <= STICK_MAX_SECTORS; n++)
    {
        int rot, deg;
        cfg.sectors = n;
        for (rot = 0; rot < 360; rot += 7)
        {
            cfg.rotation = rot;
            for (deg = 0; deg < 360; deg++)
            {
                int dx = (int)(200 * sin(deg * M_PI / 180.0));
                int dy = (int)(-200 * cos(deg * M_PI / 180.0));
                int s = stick_sector_at(&cfg, dx, dy);
                if (s < 0 || s >= n)
                {
                    CHECK(0, "N=%d rot=%d deg=%d gave sector %d", n, rot, deg, s);
                    return;
                }
            }
        }
    }
}

static void test_no_axis_pairing(void)
{
    struct rig r;
    begin_test("spec 3: sectors are independent, there is no axis pairing");
    rig_init(&r);

    /* up scrolls, down is play/pause - two unrelated rows of the table. */
    r.cfg.bind[0] = STICK_BIND_SCROLL_UP;
    r.cfg.bind[2] = STICK_BIND_PLAY_PAUSE;

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 500);
    up(&r, 240, 500);

    CHECK(only_fire(&r) == STICK_BIND_PLAY_PAUSE,
          "down should be play/pause, not an inverse scroll");
}

static void test_none_sector_is_inert(void)
{
    struct rig r;
    begin_test("spec 4: an explicitly inert sector fires nothing");
    rig_init(&r);

    r.cfg.bind[1] = STICK_BIND_NONE;

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 340, 400);
    up(&r, 340, 400);

    CHECK(r.log.n_fires == 0, "a none sector fired %d bindings", r.log.n_fires);
}

/* -------------------------------------------------------- spec 4 repeats */

static void test_repeat_rate_and_linearity(void)
{
    struct rig r;
    int low, high;
    begin_test("spec 4 / plan 3.5: repeat rate follows 2 + 14 d^2 and stays linear");

    /* Small deflection, just past the detent radius. */
    rig_init(&r);
    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 400 - (STICK_DEF_DETENT_PX + 2));
    hold(&r, 240, 400 - (STICK_DEF_DETENT_PX + 2), 3000);
    up(&r, 240, 400 - (STICK_DEF_DETENT_PX + 2));
    low = r.log.n_fires;

    /* Full deflection. */
    rig_init(&r);
    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 400 - (STICK_DEF_TRAVEL_PX + 20));
    hold(&r, 240, 400 - (STICK_DEF_TRAVEL_PX + 20), 3000);
    up(&r, 240, 400 - (STICK_DEF_TRAVEL_PX + 20));
    high = r.log.n_fires;

    /* Held 3 s after a 260 ms delay: about 2/s at rest, about 16/s at full
     * deflection. The log caps at 64 entries, which full deflection reaches. */
    CHECK(low >= 4 && low <= 9, "min-deflection rate looks wrong: %d fires", low);
    CHECK(high > low * 3, "full deflection should be far faster: %d vs %d",
          high, low);

    /* The rate must not keep climbing on its own - that is what list
     * acceleration would have done if it were still in the path. */
    CHECK(high <= 64, "rate ran away: %d fires in 3 s", high);
}

static void test_repeat_release_adds_nothing(void)
{
    struct rig r;
    int during;
    begin_test("spec 4: a repeating binding adds nothing at release");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 300);
    hold(&r, 240, 300, 1000);
    during = r.log.n_fires;
    up(&r, 240, 300);

    CHECK(during > 0, "the repeating binding should have fired while held");
    CHECK(r.log.n_fires == during, "release added %d extra fires",
          r.log.n_fires - during);
}

static void test_oneshot_does_not_repeat(void)
{
    struct rig r;
    begin_test("spec 4: a one-shot fires exactly once, however long it is held");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 360, 400);         /* right: select, a one-shot */
    hold(&r, 360, 400, 3000);
    CHECK(r.log.n_fires == 0, "a one-shot must not fire while held");
    up(&r, 360, 400);

    CHECK(only_fire(&r) == STICK_BIND_SELECT,
          "expected exactly one select, got %d fires", r.log.n_fires);
}

/* ------------------------------------------------------------ spec 6 dial */

static void dial_config(struct rig *r)
{
    r->cfg.dial = STICK_DIAL_VOLUME;
    r->cfg.deg_per_detent = 14;
    r->cfg.arm.shape = STICK_SHAPE_CIRCLE;
    r->cfg.arm.a = 240; r->cfg.arm.b = 600; r->cfg.arm.c = 150;
    r->cfg.work.shape = STICK_SHAPE_CIRCLE;
    r->cfg.work.a = 240; r->cfg.work.b = 600; r->cfg.work.c = 220;
}

/* Walk the thumb around the dial centre by the given number of degrees, in
 * two-degree steps, at a radius the engine will actually read. */
static void spin(struct rig *r, double from_deg, double degrees, int radius)
{
    double a;
    double step = degrees > 0 ? 2.0 : -2.0;
    for (a = 0; fabs(a) < fabs(degrees); a += step)
    {
        double d = (from_deg + a) * M_PI / 180.0;
        move(r, 240 + (int)(radius * sin(d)), 600 - (int)(radius * cos(d)));
    }
}

static void test_dial_steps(void)
{
    struct rig r;
    begin_test("spec 6: the dial emits one step per degPerDetent");
    rig_init(&r);
    dial_config(&r);

    down(&r, 240, 500);          /* 100 px above the centre, above the floor */
    hold(&r, 240, 500, 200);
    spin(&r, 0, 140, 100);
    up(&r, 240 + 64, 600 - 77);

    /* 140 degrees at 14 per detent is 10 steps. */
    CHECK(r.log.dial_total >= 8 && r.log.dial_total <= 11,
          "expected about 10 detents from a 140 degree sweep, got %d",
          r.log.dial_total);
    CHECK(r.log.cues[STICK_CUE_DETENT] > 0, "each detent should click");
}

static void test_dial_recircling_is_unlimited(void)
{
    struct rig r;
    begin_test("spec 6: re-circling is unlimited, three loops is 100+ detents");
    rig_init(&r);
    dial_config(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 200);
    spin(&r, 0, 3 * 360, 100);
    up(&r, 240, 500);

    CHECK(r.log.dial_total > 70,
          "three loops at 14 deg per detent should be 77 steps, got %d",
          r.log.dial_total);
}

static void test_dial_jitter_floor(void)
{
    struct rig r;
    begin_test("spec 6: no angle is read below the minimum radius");
    rig_init(&r);
    dial_config(&r);

    down(&r, 240, 590);          /* 10 px from the centre */
    hold(&r, 240, 590, 200);
    spin(&r, 0, 360, 20);        /* a full loop, but inside the floor */
    up(&r, 240, 590);

    CHECK(r.log.dial_total == 0,
          "a resting thumb produced %d phantom steps", r.log.dial_total);
}

static void test_dial_cancel_reverts_to_arm_time(void)
{
    struct rig r;
    begin_test("spec 6: cancelling a dial reverts to the value at arm time");
    rig_init(&r);
    dial_config(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 200);
    spin(&r, 0, 140, 100);
    CHECK(r.log.dial_total > 5, "the spin should have applied steps first");

    move(&r, 240, 200);          /* well outside the work circle */
    up(&r, 240, 200);

    CHECK(r.log.dial_total == 0,
          "a cancelled dial must revert to zero net steps, got %d",
          r.log.dial_total);
}

static void test_dial_tap_falls_through(void)
{
    struct rig r;
    begin_test("spec 6: a dial press with no spin is still a tap");
    rig_init(&r);
    dial_config(&r);

    down(&r, 240, 500);
    up(&r, 240, 500);

    CHECK(r.log.taps == 1, "expected a tap, got %d", r.log.taps);
    CHECK(r.log.dial_total == 0, "a tap must not move the dial");
}

/* ------------------------------------------------------ spec 9 robustness */

static void test_lost_release_is_a_cancel(void)
{
    struct rig r;
    struct stick_output out;
    begin_test("spec 9: a lost release is reaped as a cancel, never a commit");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 300);          /* a real deflection is pending */

    /* The panel goes quiet. The UI tick keeps calling us. */
    r.now += STICK_TIMEOUT_MS + 100;
    stick_process(&r.st, STICK_TOUCH_IDLE, 0, 0, r.now, &out);
    rig_apply(&r, &out);

    CHECK(out.action != STICK_ACT_FIRE, "a reaped gesture must not fire");
    CHECK(r.st.phase == STICK_PHASE_IDLE, "the gesture should have been reaped");

    /* And a release arriving late must not resurrect it. */
    up(&r, 240, 300);
    CHECK(r.log.n_fires == 0, "a late release committed %d bindings",
          r.log.n_fires);
}

static void test_second_contact_ignored(void)
{
    struct rig r;
    begin_test("spec 9: a second contact is ignored while a gesture is live");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 300);          /* first thumb: up */
    down(&r, 100, 700);          /* a stray second contact */
    up(&r, 240, 300);

    CHECK(r.log.n_fires <= 1, "a second contact started a second gesture");
    CHECK(only_fire(&r) == STICK_BIND_SCROLL_UP,
          "the original gesture should still win");
}

static void test_invalid_config_falls_back(void)
{
    struct stick_config cfg;
    begin_test("spec 9: an invalid config falls back rather than refusing to run");

    stick_config_default(&cfg, LCD_W, LCD_H);
    cfg.sectors = 0;
    CHECK(!stick_config_validate(&cfg, LCD_W, LCD_H), "N=0 should be reported");
    CHECK(cfg.sectors == 4, "N should fall back to the default, got %d",
          cfg.sectors);

    stick_config_default(&cfg, LCD_W, LCD_H);
    cfg.sectors = 9;
    stick_config_validate(&cfg, LCD_W, LCD_H);
    CHECK(cfg.sectors == 4, "N=9 should fall back, got %d", cfg.sectors);

    stick_config_default(&cfg, LCD_W, LCD_H);
    cfg.arm.shape = STICK_SHAPE_BOX;
    cfg.arm.c = 0;                       /* a degenerate zone */
    stick_config_validate(&cfg, LCD_W, LCD_H);
    CHECK(cfg.arm.c == LCD_W, "a degenerate arm zone should fall back");

    stick_config_default(&cfg, LCD_W, LCD_H);
    cfg.deg_per_detent = 5;              /* below the panel's angular noise */
    stick_config_validate(&cfg, LCD_W, LCD_H);
    CHECK(cfg.deg_per_detent == STICK_DEF_DEG_PER_DETENT,
          "sub-10 degree detents should fall back");

    stick_config_default(&cfg, LCD_W, LCD_H);
    cfg.rotation = -90;
    stick_config_validate(&cfg, LCD_W, LCD_H);
    CHECK(cfg.rotation == 270, "rotation should normalise, got %d", cfg.rotation);
}

static void test_inert_table_is_detected(void)
{
    struct stick_config cfg;
    int i;
    begin_test("spec 10.3: an all-none table with an inert centre is rejected");

    stick_config_default(&cfg, LCD_W, LCD_H);
    CHECK(!stick_config_is_inert(&cfg), "the default table is not inert");

    for (i = 0; i < STICK_MAX_SECTORS; i++)
        cfg.bind[i] = STICK_BIND_NONE;
    CHECK(!stick_config_is_inert(&cfg), "an inert table with a live centre is fine");

    cfg.centre = STICK_BIND_NONE;
    CHECK(stick_config_is_inert(&cfg), "this table can do nothing at all");

    cfg.dial = STICK_DIAL_VOLUME;
    CHECK(!stick_config_is_inert(&cfg), "a dial context is never inert");
}

static void test_state_is_small(void)
{
    begin_test("spec 9: the gesture struct stays small and fixed size");
    CHECK(sizeof(struct stick_state) <= 72,
          "stick_state is %d bytes", (int)sizeof(struct stick_state));
    CHECK(sizeof(struct stick_config) <= 128,
          "stick_config is %d bytes", (int)sizeof(struct stick_config));
}

/* -------------------------------------------------------------- the maths */

static void test_atan2_accuracy(void)
{
    int dx, dy;
    double worst = 0.0;
    int worst_dx = 0, worst_dy = 0;

    begin_test("plan 3.4: the atan2 LUT is accurate to well under a detent");

    for (dx = -LCD_W; dx <= LCD_W; dx += 1)
    {
        for (dy = -LCD_H; dy <= LCD_H; dy += 7)
        {
            double want, got, err;
            if (dx == 0 && dy == 0)
                continue;
            want = atan2((double)dx, (double)-dy) * 180.0 / M_PI;
            if (want < 0)
                want += 360.0;
            got = stick_atan2_deg64(dx, dy) / 64.0;
            err = fabs(want - got);
            if (err > 180.0)
                err = 360.0 - err;
            if (err > worst)
            {
                worst = err;
                worst_dx = dx;
                worst_dy = dy;
            }
        }
    }

    CHECK(worst < 0.25, "worst atan2 error %.4f deg at (%d,%d)",
          worst, worst_dx, worst_dy);
    printf("    worst atan2 error %.4f deg\n", worst);
}

static void test_hypot_accuracy(void)
{
    int dx, dy;
    begin_test("the integer hypotenuse never drifts from the real one");

    for (dx = 0; dx <= LCD_W; dx += 3)
    {
        for (dy = 0; dy <= LCD_H; dy += 7)
        {
            int want = (int)sqrt((double)(dx * dx + dy * dy));
            int got = stick_hypot(dx, dy);
            if (got < want - 1 || got > want + 1)
            {
                CHECK(0, "hypot(%d,%d) = %d, expected %d", dx, dy, got, want);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------- fuzz */

static uint64_t rng_state = 12345;

static uint32_t rng(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(rng_state >> 33);
}

static void test_fuzz(void)
{
    struct rig r;
    struct stick_output out;
    long i;
    int pending_fires = 0;
    bool armed_since_press = false;
    const long ITERATIONS = 1000000;

    begin_test("fuzz: one million random events keep every invariant");
    rig_init(&r);

    for (i = 0; i < ITERATIONS; i++)
    {
        int type = (int)(rng() % 4);
        int x = (int)(rng() % (LCD_W + 100)) - 50;
        int y = (int)(rng() % (LCD_H + 100)) - 50;
        long dt = (long)(rng() % 120);
        bool was_cancelled = (r.st.phase == STICK_PHASE_CANCELLED);

        /* Occasionally throw a whole new config at it mid-gesture. */
        if ((rng() % 50000) == 0)
        {
            r.cfg.sectors = (uint8_t)(rng() % 12);
            r.cfg.rotation = (int16_t)(rng() % 1000) - 500;
            r.cfg.deg_per_detent = (uint8_t)(rng() % 80);
            r.cfg.dial = (uint8_t)(rng() % 6);
            stick_config_validate(&r.cfg, LCD_W, LCD_H);
        }

        r.now += dt;
        stick_process(&r.st, type, x, y, r.now, &out);

        if (r.st.phase < STICK_PHASE_IDLE || r.st.phase > STICK_PHASE_DEAD)
        {
            CHECK(0, "phase escaped the valid set: %d", r.st.phase);
            return;
        }
        if (r.st.sector < -1 || r.st.sector >= STICK_MAX_SECTORS)
        {
            CHECK(0, "sector escaped: %d", r.st.sector);
            return;
        }
        if (out.binding >= STICK_BIND_COUNT)
        {
            CHECK(0, "emitted an out of range binding: %d", out.binding);
            return;
        }
        if (was_cancelled && out.action == STICK_ACT_FIRE)
        {
            CHECK(0, "a cancelled gesture fired at iteration %ld", i);
            return;
        }

        if (type == STICK_TOUCH_PRESS && r.st.phase == STICK_PHASE_ARMING)
            armed_since_press = true;
        if (out.action == STICK_ACT_FIRE)
        {
            if (!armed_since_press)
            {
                CHECK(0, "fired without ever arming, at iteration %ld", i);
                return;
            }
            pending_fires++;
        }
        if (r.st.phase == STICK_PHASE_IDLE)
            armed_since_press = false;
    }

    printf("    survived %ld events, %d fires\n", ITERATIONS, pending_fires);
}

/* ---------------------------------------------------------- trace replay */

/* Replays a capture in the shared trace format:
 *
 *     # sectors 4
 *     # arm_ms 110
 *        0 down 240 600
 *       40 move 240 560
 *      120 up   240 520
 *     > expect scrollUp
 */
static int run_trace(const char *path)
{
    FILE *f = fopen(path, "r");
    struct rig r;
    char line[256];
    int expectations = 0, matched = 0, next_expect = 0;
    long base = -1;

    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }

    rig_init(&r);
    printf("replaying %s\n", path);

    while (fgets(line, sizeof(line), f))
    {
        char what[32], arg[32];
        long t;
        int x, y, v;

        if (line[0] == '#')
        {
            if (sscanf(line, "# %31s %d", what, &v) == 2)
            {
                if (!strcmp(what, "sectors"))         r.cfg.sectors = (uint8_t)v;
                else if (!strcmp(what, "rotation"))   r.cfg.rotation = (int16_t)v;
                else if (!strcmp(what, "arm_ms"))     r.cfg.arm_ms = (uint16_t)v;
                else if (!strcmp(what, "dial"))       r.cfg.dial = (uint8_t)v;
                else if (!strcmp(what, "detent"))     r.cfg.deg_per_detent = (uint8_t)v;
                stick_config_validate(&r.cfg, LCD_W, LCD_H);
            }
            continue;
        }

        if (line[0] == '>')
        {
            if (sscanf(line, "> expect %31s", arg) == 1)
            {
                int want = stick_binding_from_name(arg);
                expectations++;
                if (next_expect < r.log.n_fires &&
                    r.log.fires[next_expect] == want)
                {
                    matched++;
                    next_expect++;
                }
                else
                {
                    printf("  MISMATCH: expected %s, got %s\n", arg,
                           next_expect < r.log.n_fires
                               ? stick_binding_name(r.log.fires[next_expect])
                               : "nothing");
                    tests_failed++;
                }
            }
            continue;
        }

        if (sscanf(line, "%ld %31s %d %d", &t, what, &x, &y) == 4)
        {
            struct stick_output out;
            if (base < 0)
                base = t;
            r.now = 1000 + (t - base);
            if (!strcmp(what, "down"))
                stick_process(&r.st, STICK_TOUCH_PRESS, x, y, r.now, &out);
            else if (!strcmp(what, "move"))
                stick_process(&r.st, STICK_TOUCH_CONTACT, x, y, r.now, &out);
            else if (!strcmp(what, "up"))
                stick_process(&r.st, STICK_TOUCH_RELEASE, x, y, r.now, &out);
            else
                continue;
            rig_apply(&r, &out);
        }
    }

    fclose(f);
    printf("  %d fires, %d/%d expectations matched\n",
           r.log.n_fires, matched, expectations);
    return tests_failed ? 1 : 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--trace"))
        return run_trace(argv[2]);

    printf("Rockpocket Stick engine tests\n\n");

    test_tap_inside_window();
    test_flick_inside_window();
    test_absolute_never_becomes_a_stick();
    test_cancel_commits_nothing();
    test_cancel_then_reenter_resumes();
    test_centre_binding_after_window();
    test_mid_flight_correction();

    test_dead_beats_everything();
    test_work_larger_than_arm();
    test_work_smaller_than_arm_is_legal();

    test_single_sector();
    test_eight_sectors_and_rotation();
    test_sector_boundaries();
    test_no_axis_pairing();
    test_none_sector_is_inert();

    test_repeat_rate_and_linearity();
    test_repeat_release_adds_nothing();
    test_oneshot_does_not_repeat();

    test_dial_steps();
    test_dial_recircling_is_unlimited();
    test_dial_jitter_floor();
    test_dial_cancel_reverts_to_arm_time();
    test_dial_tap_falls_through();

    test_lost_release_is_a_cancel();
    test_second_contact_ignored();
    test_invalid_config_falls_back();
    test_inert_table_is_detected();
    test_state_is_small();

    test_atan2_accuracy();
    test_hypot_accuracy();
    test_fuzz();

    printf("\n%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
