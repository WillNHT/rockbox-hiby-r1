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
    int  rows;      /* scroll steps owed, summed across fires */
    int  dial_total;
    int  dial_events;
    int  taps;
    int  tap_x, tap_y;
    int  passes;
    int  drops;
    int  cues[8];
    int  hold_ends[16];
    int  n_hold_ends;
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
    /* hold_end rides alongside the action: a gesture can end one hold and
     * deliver something else on the same event. */
    if (out->hold_end != STICK_BIND_NONE && r->log.n_hold_ends < 16)
        r->log.hold_ends[r->log.n_hold_ends++] = out->hold_end;

    switch (out->action)
    {
    case STICK_ACT_FIRE:
        if (r->log.n_fires < 64)
        {
            r->log.fire_repeating[r->log.n_fires] = out->repeating;
            r->log.fires[r->log.n_fires++] = out->binding;
            r->log.rows += out->dial_steps;
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
/* A drag the way a panel actually reports one: n samples along the way,
 * not a single teleport. The engine will not act on one opening sample -
 * a lone report the wrong way is exactly what it is guarding against - and
 * a real thumb never produces just one either. */
static void drag(struct rig *r, int x, int y0, int y1, int n)
{
    int i;
    for (i = 1; i <= n; i++)
        move(r, x, y0 + (y1 - y0) * i / n);
}

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
    begin_test("a tap is the centre binding, never an absolute hit test");
    rig_init(&r);

    down(&r, 240, 400);
    up(&r, 240, 400);

    /* This used to replay the press as an ordinary touch at 240,400. It no
     * longer does: an absolute hit test inside a relative scheme meant a
     * tap acted on whatever row sat under the thumb, which is the behaviour
     * the stick exists to replace. */
    CHECK(r.log.taps == 0, "a tap must not fall through to absolute touch");
    CHECK(only_fire(&r) == r.cfg.centre,
          "expected the centre binding, got %d", only_fire(&r));
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

    /* A one-shot: scroll steps are applied live as the thumb drags, so
     * "commits nothing" is a statement about the things that commit at
     * release, not about a list that already moved. */
    r.cfg.bind[0] = STICK_BIND_SELECT;

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
    r.cfg.bind[0] = STICK_BIND_SELECT;

    r.cfg.work.shape = STICK_SHAPE_BOX;
    r.cfg.work.a = 0; r.cfg.work.b = 400; r.cfg.work.c = LCD_W; r.cfg.work.d = 400;

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 200);         /* cancelled */
    move(&r, 240, 500);         /* back inside, deflected up */
    up(&r, 240, 500);

    CHECK(only_fire(&r) == STICK_BIND_SELECT,
          "expected select after resuming, got %d fires", r.log.n_fires);
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
    r.cfg.bind[0] = STICK_BIND_SELECT;

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
    r.cfg.bind[0] = STICK_BIND_SELECT;

    r.cfg.arm.shape = STICK_SHAPE_CIRCLE;
    r.cfg.arm.a = 240; r.cfg.arm.b = 600; r.cfg.arm.c = 160;
    r.cfg.work.shape = STICK_SHAPE_CIRCLE;
    r.cfg.work.a = 240; r.cfg.work.b = 600; r.cfg.work.c = 230;

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 420);         /* outside arm, still inside work */
    up(&r, 240, 420);

    CHECK(only_fire(&r) == STICK_BIND_SELECT,
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

/* ------------------------------------------------- scrolling is a drag */

/* The linear grammar on its own, with velocity acceleration turned off and
 * the old 28 px baseline restored. These tests are about which way the
 * list goes and when it stops, not about how far a throw carries; the
 * acceleration tests below own that. */
#define LSTEP 28

static void linear_scroll(struct rig *r)
{
    r->cfg.scroll_px = LSTEP;
    r->cfg.accel_max_q8 = 256;   /* gain 1.0: no acceleration */
    stick_reset(&r->st, &r->cfg);
}

static void test_scroll_is_inverted(void)
{
    struct rig r;
    begin_test("scroll: dragging up scrolls down, the way a finger pushes paper");
    rig_init(&r);
    linear_scroll(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    drag(&r, 240, 400, 400 - (LSTEP + STICK_DEF_DETENT_PX + 4), 3);
    up(&r, 240, 400 - (LSTEP + STICK_DEF_DETENT_PX + 4));

    CHECK(r.log.n_fires >= 1, "dragging up should have scrolled");
    CHECK(r.log.fires[0] == STICK_BIND_SCROLL_DOWN,
          "up should scroll down, got binding %d", r.log.fires[0]);
}

static void test_scroll_follows_the_thumb(void)
{
    struct rig r;
    int one, two;
    begin_test("scroll: one step per scrollPx of travel, not per unit of time");
    rig_init(&r);
    linear_scroll(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 200);

    drag(&r, 240, 500, 500 - LSTEP, 3);
    one = r.log.n_fires;

    drag(&r, 240, 500 - LSTEP, 500 - 3 * LSTEP, 2);
    two = r.log.n_fires;

    up(&r, 240, 500 - 3 * LSTEP);

    CHECK(one >= 1, "one step of travel should emit at least one step");
    CHECK(two > one, "further travel should emit further steps: %d then %d",
          one, two);
}

static void test_scroll_stops_when_the_thumb_stops(void)
{
    struct rig r;
    int moving, after;
    begin_test("scroll: a held but still thumb emits nothing");
    rig_init(&r);
    linear_scroll(&r);

    /* Deliberately short of travel_px. Past it the thumb is parked at the
     * end of the stick's range and the sustain ramp takes over on purpose
     * - that is a different rule with its own tests. What must never
     * happen, and what this defends, is a partial deflection turning into
     * a held key. */
    down(&r, 240, 500);
    hold(&r, 240, 500, 200);
    drag(&r, 240, 500, 500 - 2 * LSTEP, 3);
    moving = r.log.n_fires;

    /* Two full seconds held and perfectly still. Under a repeat this would
     * be dozens of fires. */
    hold(&r, 240, 500 - 2 * LSTEP, 2000);
    after = r.log.n_fires;

    up(&r, 240, 500 - 2 * LSTEP);

    CHECK(moving > 0, "the drag itself should have scrolled");
    CHECK(after == moving, "holding still added %d fires", after - moving);
}

/* A quick drag whose very first sample is a large one the wrong way. The
 * panel does this: the contact settles and one report lands well off the
 * track before the rest follow. Until the latch had a direction, that first
 * sample was taken at face value and became a step the wrong way with
 * nothing standing in its way - "quick drag up, sometimes it scrolls up".
 *
 * Seeding the latch from the distance already travelled from the touch-down
 * point makes the stray sample a reversal instead, which has to clear
 * scroll_reverse_px and be agreed with by the next event before it counts.
 * It never is. */
static void test_scroll_first_sample_cannot_reverse(void)
{
    struct rig r;
    int i;
    begin_test("scroll: a stray first sample cannot start the scroll backwards");
    rig_init(&r);
    linear_scroll(&r);

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);      /* close the arm window, still at rest */

    /* One bogus report, far enough down to be worth several steps. */
    move(&r, 240, 600 + 4 * LSTEP);

    /* Then the drag the user actually made: upward, quickly. */
    for (i = 1; i <= 6; i++)
        move(&r, 240, 600 - i * 2 * LSTEP);
    up(&r, 240, 600 - 12 * LSTEP);

    for (i = 0; i < r.log.n_fires; i++)
        CHECK(r.log.fires[i] != STICK_BIND_SCROLL_UP,
              "fire %d went up; an upward drag must only scroll down", i);
    CHECK(r.log.n_fires > 0, "the drag should still have scrolled");
}

/* ------------------------------------------------- sustained scrolling */

/* Park the thumb at full deflection and the list keeps going, faster the
 * longer it is held. This is the long-library case: a drag can only cover
 * as much list as the thumb can cover screen, and a thumb that has stopped
 * moving but not let go is asking for a rate, not a distance. */
static void test_sustain_at_full_deflection(void)
{
    struct rig r;
    int early, late;
    begin_test("sustain: a parked thumb keeps scrolling, and speeds up");
    rig_init(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 200);
    drag(&r, 240, 500, 500 - (STICK_DEF_TRAVEL_PX + 20), 4);

    hold(&r, 240, 500 - (STICK_DEF_TRAVEL_PX + 20), 1200);
    early = r.log.rows;
    hold(&r, 240, 500 - (STICK_DEF_TRAVEL_PX + 20), 1200);
    late = r.log.rows - early;
    up(&r, 240, 500 - (STICK_DEF_TRAVEL_PX + 20));

    CHECK(early > 0, "a parked thumb should have kept scrolling");
    CHECK(late > early, "the ramp should accelerate: %d rows then %d",
          early, late);
}

/* The rim. A drag that begins near the top of the panel cannot reach full
 * deflection upward - the glass runs out first - and that is precisely the
 * drag a long list produces. Pressed against the edge in the direction
 * already committed counts as being as far over as the stick goes. */
static void test_sustain_at_the_rim(void)
{
    struct rig r;
    begin_test("sustain: the rim counts, because the glass ran out first");
    rig_init(&r);

    /* Only 40 px of travel available, well under one scroll step, so the
     * drag itself never emits anything at all. */
    down(&r, 240, 60);
    hold(&r, 240, 60, 200);
    drag(&r, 240, 60, 20, 3);
    CHECK(r.log.rows == 0, "the drag alone should not have scrolled");

    hold(&r, 240, 20, 1500);
    up(&r, 240, 20);

    CHECK(r.log.rows > 0, "pinned at the top rim should sustain");
    CHECK(r.log.fires[r.log.n_fires - 1] == STICK_BIND_SCROLL_DOWN,
          "up against the rim must scroll down, got %d",
          r.log.fires[r.log.n_fires - 1]);
}

/* The two must never both count, or a thumb that keeps pushing gets the
 * sum of a drag and a ramp. */
static void test_sustain_needs_a_still_thumb(void)
{
    struct rig r;
    int moving;
    begin_test("sustain: a thumb that is still travelling does not sustain");
    rig_init(&r);

    down(&r, 240, 700);
    hold(&r, 240, 700, 200);

    /* Past full deflection the whole way, but never still: every sample
     * moves further than the stillness slop. */
    {
        int i;
        for (i = 1; i <= 20; i++)
            move(&r, 240, 700 - (STICK_DEF_TRAVEL_PX + 20) - i * 20);
    }
    moving = r.log.rows;
    up(&r, 240, 700 - (STICK_DEF_TRAVEL_PX + 20) - 20 * 20);

    /* 20 samples x 20 px is 400 px of drag; at the accelerated step that
     * is a handful of rows, nowhere near what 800 ms of ramp would give. */
    CHECK(moving < 30, "a moving thumb emitted %d rows - the ramp ran too",
          moving);
}

/* ------------------------------------------- scrolling is velocity-scaled */

/* Drag from y0 upward by total px, delivered in n equal steps of 40 ms
 * each. The step size is what sets the speed, so the same distance covered
 * in fewer events is a faster thumb. */
static int drag_up(struct rig *r, int y0, int total, int n)
{
    int i;
    down(r, 240, y0);
    hold(r, 240, y0, 200);
    for (i = 1; i <= n; i++)
        move(r, 240, y0 - (total * i) / n);
    up(r, 240, y0 - total);
    return r->log.rows;
}

static void test_slow_drag_is_the_coarse_baseline(void)
{
    struct rig r;
    int fires;
    begin_test("scroll: a slow thumb gets the coarse baseline, one row at a time");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;   /* the drag only, no coast on top of it */
    stick_reset(&r.st, &r.cfg);

    /* 300 px of travel over 1.2 s: about 250 px/s, one unit of gain, so a
     * bit under two rows. The point is that it is a handful and not a
     * screenful - this is the 10x reduction in baseline sensitivity. */
    fires = drag_up(&r, 700, 300, 30);

    CHECK(fires >= 1, "a 300 px crawl should still move the list");
    CHECK(fires <= 4, "a 300 px crawl emitted %d steps, far too twitchy",
          fires);
}

static void test_fast_drag_covers_far_more_ground(void)
{
    struct rig r;
    int slow, fast;
    begin_test("scroll: the same distance dragged fast moves many more rows");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);
    slow = drag_up(&r, 700, 300, 30);

    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);
    fast = drag_up(&r, 700, 300, 3);

    /* This is the whole answer to long lists: one scheme, no knowledge of
     * how many rows the screen holds, precision when the thumb creeps and
     * reach when it does not. */
    CHECK(fast > 3 * slow, "fast drag moved %d rows against a slow %d",
          fast, slow);
}

static void test_acceleration_is_capped(void)
{
    struct rig r;
    begin_test("scroll: gain has a ceiling, a jerk cannot teleport the list");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);

    /* One 40 ms event covering 600 px: 15000 px/s, sixty units of raw gain
     * if nothing stopped it. */
    down(&r, 240, 700);
    hold(&r, 240, 700, 200);
    move(&r, 240, 100);
    up(&r, 240, 100);

    {
        int max = 600 / (STICK_DEF_SCROLL_PX * 256 / STICK_DEF_ACCEL_MAX_Q8);
        CHECK(r.log.rows <= max + 1,
              "%d rows from one jerk, cap allows %d", r.log.rows, max);
    }
}

static void test_acceleration_can_be_turned_off(void)
{
    struct rig r;
    int fast_on, fast_off;
    begin_test("scroll: gain 1.0 restores the plain linear drag");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);
    fast_on = drag_up(&r, 700, 300, 3);

    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    r.cfg.accel_max_q8 = 256;
    stick_reset(&r.st, &r.cfg);
    fast_off = drag_up(&r, 700, 300, 3);

    CHECK(fast_off < fast_on,
          "acceleration off still moved %d rows against %d with it on",
          fast_off, fast_on);
    CHECK(fast_off == 300 / STICK_DEF_SCROLL_PX,
          "linear drag should be exactly 300/scrollPx rows, got %d",
          fast_off);
}

static void test_baseline_is_a_tenth_of_the_old_sensitivity(void)
{
    struct rig r;
    begin_test("scroll: the slow baseline is coarse, the fast end is not");
    rig_init(&r);

    /* Guards the two decisions together, because they were made together:
     * the slow baseline is 5x the old 28 px, and the ceiling was lowered
     * with it so the fast end of the range did not move. */
    CHECK(r.cfg.scroll_px == 140,
          "baseline scrollPx is %d, expected 140", r.cfg.scroll_px);
    CHECK(stick_effective_scroll_px(&r.st) == r.cfg.scroll_px,
          "a still thumb must get the baseline, got %d",
          stick_effective_scroll_px(&r.st));
    CHECK((r.cfg.scroll_px * 256) / r.cfg.accel_max_q8 == 23,
          "fully accelerated step is %d px, expected 23 as before",
          (int)((r.cfg.scroll_px * 256) / r.cfg.accel_max_q8));
}

/* ------------------------------------------------ the dial's slow tap */

static void test_dial_needs_the_slow_tap(void)
{
    struct rig r;
    begin_test("dial: holding still for dialArmMs arms it, travel does not");
    rig_init(&r);
    r.cfg.dial = STICK_DIAL_VOLUME;
    r.cfg.centre = STICK_BIND_NONE;
    stick_reset(&r.st, &r.cfg);

    down(&r, 240, 400);
    hold(&r, 240, 400, 400);
    CHECK(stick_phase(&r.st) != STICK_PHASE_DIAL,
          "a short hold should not have armed the dial");

    hold(&r, 240, 400, 900);
    CHECK(stick_phase(&r.st) == STICK_PHASE_DIAL,
          "a second of holding still should arm the dial, phase %d",
          stick_phase(&r.st));

    up(&r, 240, 400);
}

static void test_dial_arming_cues_while_it_builds(void)
{
    struct rig r;
    begin_test("dial: the wait is cued, so it is not silence while it builds");
    rig_init(&r);
    r.cfg.dial = STICK_DIAL_VOLUME;
    r.cfg.centre = STICK_BIND_NONE;
    stick_reset(&r.st, &r.cfg);

    down(&r, 240, 400);
    hold(&r, 240, 400, 1100);
    up(&r, 240, 400);

    /* One per STICK_DIAL_TICK_MS over the arming second, give or take the
     * event grid. The cue is the progress, not the receipt: it has to tick
     * several times before the dial arms or the hold is a silent wait. */
    CHECK(r.log.cues[STICK_CUE_DETENT] >= 3,
          "only %d progress cues during the hold",
          r.log.cues[STICK_CUE_DETENT]);
    CHECK(r.log.cues[STICK_CUE_COMMIT] >= 1,
          "arming the dial should be cued distinctly");
}

static void test_a_drag_is_never_a_dial(void)
{
    struct rig r;
    begin_test("dial: a thumb that travels gets sectors, however long it stays");
    rig_init(&r);
    r.cfg.dial = STICK_DIAL_VOLUME;
    stick_reset(&r.st, &r.cfg);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 400, 400);              /* out of the detent radius */
    hold(&r, 400, 400, 4000);        /* and held there far past dialArmMs */
    up(&r, 400, 400);

    CHECK(stick_phase(&r.st) != STICK_PHASE_DIAL,
          "a travelled gesture became a dial");
    CHECK(r.log.dial_events == 0, "a travelled gesture emitted dial steps");
}

/* ------------------------------- direction, once committed, stays put */

static void test_a_small_reversal_does_not_flip_the_scroll(void)
{
    struct rig r;
    int forward, after;
    begin_test("scroll: a thumb rocking back does not scroll the other way");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);

    /* A fast drag up, then one sample that jumps a long way back. That is
     * what the panel does on the way to a release, and it is the shape of
     * the intermittent backwards scroll: a lone sample nothing else
     * agrees with. */
    down(&r, 240, 700);
    hold(&r, 240, 700, 200);
    move(&r, 240, 560);
    move(&r, 240, 420);
    forward = r.log.rows;

    move(&r, 240, 600);         /* the bogus sample */
    after = r.log.rows;
    up(&r, 240, 600);

    CHECK(forward > 0, "the drag should have scrolled");
    CHECK(after == forward,
          "one stray sample emitted %d step(s) the wrong way",
          after - forward);
}

static void test_a_real_reversal_still_turns_around(void)
{
    struct rig r;
    int i, up_steps = 0, down_steps = 0;
    begin_test("scroll: a deliberate reversal still changes direction");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);

    down(&r, 240, 500);
    hold(&r, 240, 500, 200);
    drag(&r, 240, 500, 360, 2); /* up: scrolls down */
    /* Back down past the reversal threshold and kept going, which is what
     * separates a change of mind from a stray sample. */
    move(&r, 240, 500);
    move(&r, 240, 640);
    move(&r, 240, 780);
    up(&r, 240, 780);

    for (i = 0; i < r.log.n_fires; i++)
    {
        if (r.log.fires[i] == STICK_BIND_SCROLL_DOWN) down_steps++;
        if (r.log.fires[i] == STICK_BIND_SCROLL_UP)   up_steps++;
    }

    CHECK(down_steps > 0, "the first half should have scrolled down");
    CHECK(up_steps > 0, "the reversal never took effect");
}

static void test_an_arc_does_not_leave_the_sector(void)
{
    struct rig r;
    int i;
    begin_test("scroll: a thumb arcing sideways stays in the vertical sector");
    rig_init(&r);
    r.cfg.coast_min_px_s = 0;
    stick_reset(&r.st, &r.cfg);

    /* Straight up to begin with, so the gesture commits to the vertical
     * sector, then sweeping right the way a thumb pivoting on its knuckle
     * does. The last two points are past the 45 degree boundary: without a
     * margin the gesture changes sector mid-scroll and the release fires
     * select. */
    down(&r, 240, 700);
    hold(&r, 240, 700, 200);
    move(&r, 240, 600);
    move(&r, 330, 590);
    move(&r, 400, 580);
    up(&r, 400, 580);

    for (i = 0; i < r.log.n_fires; i++)
        CHECK(r.log.fires[i] == STICK_BIND_SCROLL_DOWN ||
              r.log.fires[i] == STICK_BIND_SCROLL_UP,
              "an arcing drag fired %d, not a scroll", r.log.fires[i]);
}

static void test_a_deliberate_sideways_move_still_leaves_the_sector(void)
{
    struct rig r;
    begin_test("scroll: hysteresis is a margin, not a trap");
    rig_init(&r);

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 300);          /* commit to up */
    move(&r, 400, 390);          /* then firmly right */
    up(&r, 400, 390);

    CHECK(stick_sector_at(&r.cfg, 160, -10) == 1,
          "the geometry under test is not the right-hand sector");
    CHECK(r.log.n_fires > 0, "the sideways move fired nothing at all");
}

static void test_a_throw_never_coasts_backwards(void)
{
    struct rig r;
    int i;
    struct stick_output out;
    begin_test("coast: a throw cannot coast against the way it was thrown");
    rig_init(&r);

    /* Thrown hard upward, then the last sample before release rocks back -
     * exactly what a decelerating thumb does, and what used to leave the
     * smoothed velocity pointing the wrong way. */
    down(&r, 240, 700);
    hold(&r, 240, 700, 200);
    move(&r, 240, 520);
    move(&r, 240, 340);
    move(&r, 240, 460);         /* one stray sample, the wrong way */
    up(&r, 240, 460);

    /* Let any coast run to a stop. */
    for (i = 0; i < 200; i++)
    {
        r.now += 40;
        stick_process(&r.st, STICK_TOUCH_IDLE, 0, 0, r.now, &out);
        rig_apply(&r, &out);
    }

    for (i = 0; i < r.log.n_fires; i++)
        CHECK(r.log.fires[i] != STICK_BIND_SCROLL_UP,
              "the coast ran backwards: fire %d was scrollUp", i);
}

/* ------------------------------------------------------------------ holds */

/* The WPS table in miniature: a one-shot on the short drag and something
 * else when the same direction is held. */
static void hold_config(struct rig *r)
{
    r->cfg.sectors = 4;
    r->cfg.bind[0] = STICK_BIND_MENU;
    r->cfg.hold[0] = STICK_BIND_QUICKSCREEN;
    r->cfg.bind[1] = STICK_BIND_NEXT;
    r->cfg.hold[1] = STICK_BIND_SEEK_FWD;
    r->cfg.bind[3] = STICK_BIND_PREV;
    r->cfg.hold[3] = STICK_BIND_SEEK_BACK;
    r->cfg.centre = STICK_BIND_NONE;
    stick_reset(&r->st, &r->cfg);
}

static void test_short_drag_fires_the_drag_binding(void)
{
    struct rig r;
    begin_test("hold: a drag released before holdMs fires the drag binding");
    rig_init(&r);
    hold_config(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 160);        /* past the arm window */
    move(&r, 240, 500 - 90);        /* up */
    up(&r, 240, 500 - 90);

    CHECK(only_fire(&r) == STICK_BIND_MENU,
          "expected menu, got %d", only_fire(&r));
    CHECK(r.log.n_hold_ends == 0, "nothing was held, so nothing ended");
}

static void test_held_drag_fires_the_hold_binding(void)
{
    struct rig r;
    int i, quick = 0, menu = 0;
    begin_test("hold: the same drag held past holdMs fires the hold binding");
    rig_init(&r);
    hold_config(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 160);
    move(&r, 240, 500 - 90);
    hold(&r, 240, 500 - 90, 700);   /* well past STICK_DEF_HOLD_MS */
    up(&r, 240, 500 - 90);

    for (i = 0; i < r.log.n_fires; i++)
    {
        if (r.log.fires[i] == STICK_BIND_QUICKSCREEN) quick++;
        if (r.log.fires[i] == STICK_BIND_MENU)        menu++;
    }

    CHECK(quick == 1, "expected one quickscreen, got %d", quick);
    /* The whole point: holding must not also do the short-drag thing. */
    CHECK(menu == 0, "a held drag also fired its short-drag binding");
}

static void test_hold_end_is_reported(void)
{
    struct rig r;
    begin_test("hold: a released hold is reported so a seek can be stopped");
    rig_init(&r);
    hold_config(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 160);
    move(&r, 340, 500);             /* right */
    hold(&r, 340, 500, 700);
    up(&r, 340, 500);

    CHECK(r.log.n_hold_ends == 1, "expected one hold end, got %d",
          r.log.n_hold_ends);
    CHECK(r.log.hold_ends[0] == STICK_BIND_SEEK_FWD,
          "the wrong binding ended: %d", r.log.hold_ends[0]);
}

static void test_hold_ends_when_the_sector_changes(void)
{
    struct rig r;
    begin_test("hold: sliding from one held sector to another ends the first");
    rig_init(&r);
    hold_config(&r);

    down(&r, 240, 500);
    hold(&r, 240, 500, 160);
    move(&r, 340, 500);             /* right: seek forward */
    hold(&r, 340, 500, 700);
    move(&r, 140, 500);             /* left, without lifting */
    hold(&r, 140, 500, 700);
    up(&r, 140, 500);

    CHECK(r.log.n_hold_ends == 2, "expected two hold ends, got %d",
          r.log.n_hold_ends);
    CHECK(r.log.hold_ends[0] == STICK_BIND_SEEK_FWD,
          "the forward seek should have been stopped first");
    CHECK(r.log.hold_ends[1] == STICK_BIND_SEEK_BACK,
          "the backward seek should have been stopped at release");
}

static void test_cancelled_hold_still_ends(void)
{
    struct rig r;
    begin_test("hold: a hold cancelled out of the work zone is still stopped");
    rig_init(&r);
    hold_config(&r);
    /* A work zone the thumb can leave. */
    r.cfg.work.shape = STICK_SHAPE_CIRCLE;
    r.cfg.work.a = 240;
    r.cfg.work.b = 500;
    r.cfg.work.c = 150;
    stick_reset(&r.st, &r.cfg);

    down(&r, 240, 500);
    hold(&r, 240, 500, 160);
    move(&r, 340, 500);
    hold(&r, 340, 500, 700);
    move(&r, 470, 500);             /* outside the work circle: cancel */
    up(&r, 470, 500);

    CHECK(r.log.n_hold_ends >= 1,
          "a cancelled seek would run forever; got %d hold ends",
          r.log.n_hold_ends);
    CHECK(r.log.hold_ends[0] == STICK_BIND_SEEK_FWD,
          "the wrong binding ended: %d", r.log.hold_ends[0]);
}

static void test_scroll_speed_follows_rockbox(void)
{
    int i;
    begin_test("scroll speed: Rockbox's own setting, 9 leaving the feel alone");

    CHECK(stick_scroll_px_for_speed(9) == STICK_DEF_SCROLL_PX,
          "the default Scroll Speed should still be the tested feel, got %d",
          stick_scroll_px_for_speed(9));
    for (i = 1; i <= 17; i++)
        CHECK(stick_scroll_px_for_speed(i) <= stick_scroll_px_for_speed(i - 1),
              "speed %d is not at least as fast as %d", i, i - 1);
    CHECK(stick_scroll_px_for_speed(17) >= 6,
          "even the fastest setting needs a floor a thumb can hit");
}

/* -------------------------------------------------------- edge swipes */

static void edge_rig(struct rig *r)
{
    rig_init(r);
    r->cfg.edge_px = STICK_DEF_EDGE_PX;
    r->cfg.edge_travel_px = STICK_DEF_EDGE_TRAVEL_PX;
    r->cfg.edge_left = STICK_BIND_BACK;
    r->cfg.edge_right = STICK_BIND_MENU;
    stick_reset(&r->st, &r->cfg);
}

static void test_edge_swipe_fires(void)
{
    struct rig r;
    begin_test("edge: a swipe inward from the left edge goes back");
    edge_rig(&r);

    down(&r, 4, 400);
    move(&r, 40, 400);
    move(&r, 100, 400);
    up(&r, 100, 400);

    CHECK(only_fire(&r) == STICK_BIND_BACK,
          "expected back, got %d", only_fire(&r));
    CHECK(r.log.passes == 0,
          "an edge swipe must not fall through to absolute touch");
}

static void test_edge_swipe_needs_travel(void)
{
    struct rig r;
    begin_test("edge: a short poke at the edge does nothing");
    edge_rig(&r);

    down(&r, 4, 400);
    move(&r, 20, 400);
    up(&r, 20, 400);

    CHECK(r.log.n_fires == 0, "a poke fired %d bindings", r.log.n_fires);
}

static void test_edge_swipe_fires_once(void)
{
    struct rig r;
    begin_test("edge: a long swipe is still one action");
    edge_rig(&r);

    down(&r, 4, 400);
    move(&r, 100, 400);
    move(&r, 200, 400);
    move(&r, 400, 400);
    up(&r, 400, 400);

    CHECK(r.log.n_fires == 1, "fired %d times", r.log.n_fires);
}

static void test_edge_is_never_a_stick(void)
{
    struct rig r;
    int i;
    begin_test("edge: a gesture that starts on the strip never scrolls");
    edge_rig(&r);

    down(&r, 4, 600);
    hold(&r, 4, 600, 300);
    move(&r, 4, 300);           /* straight up: a scroll, if it were one */
    move(&r, 4, 200);
    up(&r, 4, 200);

    for (i = 0; i < r.log.n_fires; i++)
        CHECK(r.log.fires[i] != STICK_BIND_SCROLL_UP &&
              r.log.fires[i] != STICK_BIND_SCROLL_DOWN,
              "the edge strip scrolled: %d", r.log.fires[i]);
}

static void test_stick_still_works_beside_the_edge(void)
{
    struct rig r;
    begin_test("edge: the rest of the panel is still the stick");
    edge_rig(&r);
    linear_scroll(&r);

    down(&r, 240, 600);
    hold(&r, 240, 600, 200);
    move(&r, 240, 600 - 3 * LSTEP);
    up(&r, 240, 600 - 3 * LSTEP);

    CHECK(r.log.n_fires > 0, "the stick stopped working with edges on");
}

/* ------------------------------------------------------------- the coast */

static void scroll_rig(struct rig *r)
{
    rig_init(r);
    r->cfg.arm_ms = 0;          /* armed from the first move */
    stick_reset(&r->st, &r->cfg);
}

/* Drag upward at a steady speed, one 40 ms tick at a time. */
static void flick_up(struct rig *r, int px_per_tick, int ticks)
{
    int i, y = 600;
    down(r, 240, y);
    for (i = 0; i < ticks; i++)
    {
        y -= px_per_tick;
        move(r, 240, y);
    }
    up(r, 240, y);
}

static int coast_ticks(struct rig *r, int max_ticks)
{
    int i;
    for (i = 0; i < max_ticks; i++)
    {
        if (stick_phase(&r->st) != STICK_PHASE_COAST)
            break;
        ev(r, STICK_TOUCH_IDLE, r->st.x, r->st.y, 40);
    }
    return i;
}

static void test_flick_coasts(void)
{
    struct rig r;
    int during, after;
    begin_test("coast: a flicked list keeps moving after the thumb goes");
    scroll_rig(&r);

    flick_up(&r, 40, 6);        /* 1000 px/s, well over the threshold */
    during = r.log.n_fires;
    CHECK(stick_phase(&r.st) == STICK_PHASE_COAST,
          "a fast release should have started a coast, phase %d",
          stick_phase(&r.st));

    coast_ticks(&r, 200);
    after = r.log.n_fires;

    CHECK(after > during, "the coast emitted nothing");
    CHECK(stick_phase(&r.st) == STICK_PHASE_IDLE,
          "the coast never stopped");
}

static void test_coast_keeps_direction(void)
{
    struct rig r;
    int i;
    begin_test("coast: it carries on the way the drag was going");
    scroll_rig(&r);

    flick_up(&r, 40, 6);
    i = r.log.n_fires;
    coast_ticks(&r, 200);

    for (; i < r.log.n_fires; i++)
        CHECK(r.log.fires[i] == STICK_BIND_SCROLL_DOWN,
              "dragging up should keep walking down the list, got %d",
              r.log.fires[i]);
}

static void test_slow_release_does_not_coast(void)
{
    struct rig r;
    begin_test("coast: letting go gently stops the list, it does not throw it");
    scroll_rig(&r);

    /* 5 px per 40 ms tick is 125 px/s at the thumb - a deliberate stop. */
    flick_up(&r, 2, 8);

    CHECK(stick_phase(&r.st) != STICK_PHASE_COAST,
          "a slow release must not coast");
}

static void test_touch_catches_the_coast(void)
{
    struct rig r;
    int caught;
    begin_test("coast: touching a moving list stops it dead");
    scroll_rig(&r);

    flick_up(&r, 40, 6);
    coast_ticks(&r, 3);
    CHECK(stick_phase(&r.st) == STICK_PHASE_COAST, "should still be coasting");

    down(&r, 240, 400);
    caught = r.log.n_fires;
    coast_ticks(&r, 50);

    CHECK(stick_phase(&r.st) != STICK_PHASE_COAST,
          "the coast survived a new press");
    CHECK(r.log.n_fires == caught,
          "the caught coast kept scrolling: %d extra fires",
          r.log.n_fires - caught);
}

static void test_coast_decelerates(void)
{
    struct rig r;
    int first = 0, last = 0, i, n;
    begin_test("coast: it slows down rather than stopping all at once");
    scroll_rig(&r);

    flick_up(&r, 40, 6);

    /* Steps emitted in the first ten ticks against the last ten. */
    n = r.log.n_fires;
    coast_ticks(&r, 10);
    first = r.log.n_fires - n;

    while (stick_phase(&r.st) == STICK_PHASE_COAST)
    {
        n = r.log.n_fires;
        i = coast_ticks(&r, 10);
        if (i == 0)
            break;
        last = r.log.n_fires - n;
    }

    CHECK(first > 0, "the coast started with nothing");
    CHECK(last <= first, "the coast sped up: %d then %d", first, last);
}

/* -------------------------------------------------------- spec 4 repeats */

static void test_repeat_rate_and_linearity(void)
{
    struct rig r;
    int low, high;
    begin_test("spec 4 / plan 3.5: repeat rate follows 2 + 14 d^2 and stays linear");

    /* Volume is the held-key case: scroll bindings follow the thumb
     * instead of repeating, and are covered by their own tests. */
    rig_init(&r);
    r.cfg.bind[0] = STICK_BIND_VOL_UP;
    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 400 - (STICK_DEF_DETENT_PX + 2));
    hold(&r, 240, 400 - (STICK_DEF_DETENT_PX + 2), 3000);
    up(&r, 240, 400 - (STICK_DEF_DETENT_PX + 2));
    low = r.log.n_fires;

    /* Full deflection. */
    rig_init(&r);
    r.cfg.bind[0] = STICK_BIND_VOL_UP;
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
    r.cfg.bind[0] = STICK_BIND_VOL_UP;

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
    /* These tests are about the dial's grammar - degrees in, detents out -
     * so they take it the short way, without the slow tap that arms it in
     * the firmware. The arming has tests of its own below. */
    r->cfg.dial_on_hold = 0;
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
    begin_test("spec 6: a dial press with no spin is the centre binding");
    rig_init(&r);
    dial_config(&r);

    down(&r, 240, 500);
    up(&r, 240, 500);

    CHECK(r.log.taps == 0, "a tap must not fall through to absolute touch");
    CHECK(only_fire(&r) == r.cfg.centre,
          "expected the centre binding, got %d", only_fire(&r));
    CHECK(r.log.dial_total == 0, "a tap must not move the dial");
}

/* ------------------------------------------------------ spec 9 robustness */

static void test_lost_release_is_a_cancel(void)
{
    struct rig r;
    struct stick_output out;
    begin_test("spec 9: a lost release is reaped as a cancel, never a commit");
    rig_init(&r);
    r.cfg.bind[0] = STICK_BIND_SELECT;

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
    r.cfg.bind[0] = STICK_BIND_SELECT;

    down(&r, 240, 400);
    hold(&r, 240, 400, 200);
    move(&r, 240, 300);          /* first thumb: up */
    down(&r, 100, 700);          /* a stray second contact */
    up(&r, 240, 300);

    CHECK(r.log.n_fires <= 1, "a second contact started a second gesture");
    CHECK(only_fire(&r) == STICK_BIND_SELECT,
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
    /* The bound is a bloat alarm, not a budget: it moves when a feature
     * genuinely needs the room. It went 72 -> 96 for the coast's velocity
     * and the hold's bookkeeping, and 96 -> 128 for the sustain ramp's
     * clock. */
    CHECK(sizeof(struct stick_state) <= 128,
          "stick_state is %d bytes", (int)sizeof(struct stick_state));
    CHECK(sizeof(struct stick_config) <= 160,
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

        if (r.st.phase < STICK_PHASE_IDLE || r.st.phase > STICK_PHASE_EDGE)
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

    test_scroll_is_inverted();
    test_scroll_follows_the_thumb();
    test_scroll_stops_when_the_thumb_stops();
    test_scroll_first_sample_cannot_reverse();
    test_sustain_at_full_deflection();
    test_sustain_at_the_rim();
    test_sustain_needs_a_still_thumb();
    test_slow_drag_is_the_coarse_baseline();
    test_fast_drag_covers_far_more_ground();
    test_acceleration_is_capped();
    test_acceleration_can_be_turned_off();
    test_baseline_is_a_tenth_of_the_old_sensitivity();
    test_dial_needs_the_slow_tap();
    test_dial_arming_cues_while_it_builds();
    test_a_drag_is_never_a_dial();
    test_a_small_reversal_does_not_flip_the_scroll();
    test_a_real_reversal_still_turns_around();
    test_an_arc_does_not_leave_the_sector();
    test_a_deliberate_sideways_move_still_leaves_the_sector();
    test_a_throw_never_coasts_backwards();
    test_short_drag_fires_the_drag_binding();
    test_held_drag_fires_the_hold_binding();
    test_hold_end_is_reported();
    test_hold_ends_when_the_sector_changes();
    test_cancelled_hold_still_ends();
    test_scroll_speed_follows_rockbox();
    test_edge_swipe_fires();
    test_edge_swipe_needs_travel();
    test_edge_swipe_fires_once();
    test_edge_is_never_a_stick();
    test_stick_still_works_beside_the_edge();
    test_flick_coasts();
    test_coast_keeps_direction();
    test_slow_release_does_not_coast();
    test_touch_catches_the_coast();
    test_coast_decelerates();
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
