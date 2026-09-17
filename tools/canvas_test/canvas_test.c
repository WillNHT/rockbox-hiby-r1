/* Host tests for the Rockpocket Canvas engine (apps/canvas.c).
 *
 * Same shape as tools/stick_test: every test is named after the behaviour
 * it defends, and a fuzz run at the end asserts the invariants that must
 * hold whatever the input was. Run with --dump to also write PPM files of
 * the effects, because a blur that is subtly wrong still passes a numeric
 * test and still looks wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "canvas.h"

static int tests_run, tests_failed;
static const char *current;

#define CHECK(cond, ...) do {                                           \
    tests_run++;                                                        \
    if (!(cond)) {                                                      \
        tests_failed++;                                                 \
        printf("  FAIL %s: ", current);                                 \
        printf(__VA_ARGS__);                                            \
        printf("\n    at %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                   \
} while (0)

#define TEST(name) do { current = name; } while (0)

/* ---------------------------------------------------------------- */
/* Surfaces with guard margins, so a blit that runs off the end is a
 * test failure rather than a silent corruption.                      */
/* ---------------------------------------------------------------- */

#define GUARD 8
#define GUARD_PX 0xBEEF
#define GUARD_A  0x5A

struct test_surf
{
    struct canvas_surface s;
    canvas_px *base;
    uint8_t *abase;
    int w, h, stride;
    bool has_alpha;
};

static void ts_make(struct test_surf *t, int w, int h, bool alpha)
{
    int stride = w + 2 * GUARD;
    int rows = h + 2 * GUARD;
    int i;

    t->w = w; t->h = h; t->stride = stride; t->has_alpha = alpha;
    t->base = malloc((size_t)stride * rows * sizeof(canvas_px));
    t->abase = alpha ? malloc((size_t)stride * rows) : NULL;

    for (i = 0; i < stride * rows; i++)
        t->base[i] = GUARD_PX;
    if (t->abase)
        memset(t->abase, GUARD_A, (size_t)stride * rows);

    canvas_surface_init(&t->s,
                        t->base + (size_t)GUARD * stride + GUARD,
                        alpha ? t->abase + (size_t)GUARD * stride + GUARD : NULL,
                        w, h, stride);
}

static void ts_free(struct test_surf *t)
{
    free(t->base);
    free(t->abase);
}

static bool ts_guards_intact(const struct test_surf *t)
{
    int x, y;

    for (y = 0; y < t->h + 2 * GUARD; y++)
    {
        for (x = 0; x < t->stride; x++)
        {
            bool inside = (y >= GUARD && y < GUARD + t->h &&
                           x >= GUARD && x < GUARD + t->w);
            if (inside)
                continue;
            if (t->base[(size_t)y * t->stride + x] != GUARD_PX)
                return false;
            if (t->abase && t->abase[(size_t)y * t->stride + x] != GUARD_A)
                return false;
        }
    }
    return true;
}

static void ts_fill_all(struct test_surf *t, canvas_px c, uint8_t a)
{
    struct canvas_rect r = { 0, 0, t->w, t->h };
    canvas_fill_a(&t->s, &r, c, a);
}

static canvas_px px(const struct test_surf *t, int x, int y)
{
    return *canvas_at(&t->s, x, y);
}

static uint8_t al(const struct test_surf *t, int x, int y)
{
    return *canvas_alpha_at(&t->s, x, y);
}

/* ---------------------------------------------------------------- */

static const canvas_px RED   = CANVAS_RGB(255, 0, 0);
static const canvas_px GREEN = CANVAS_RGB(0, 255, 0);
static const canvas_px BLUE  = CANVAS_RGB(0, 0, 255);
static const canvas_px BLACK = 0;
static const canvas_px WHITE = CANVAS_RGB(255, 255, 255);

/* ---------------------------------------------------------------- */
/* Rectangles                                                        */
/* ---------------------------------------------------------------- */

static void test_rects(void)
{
    struct canvas_rect a = { 10, 10, 20, 20 };
    struct canvas_rect b = { 20, 20, 20, 20 };
    struct canvas_rect c = { 100, 100, 5, 5 };
    struct canvas_rect t;

    TEST("intersect overlapping");
    t = a;
    CHECK(canvas_rect_intersect(&t, &b), "should overlap");
    CHECK(t.x == 20 && t.y == 20 && t.w == 10 && t.h == 10,
          "got %d,%d %dx%d", t.x, t.y, t.w, t.h);

    TEST("intersect disjoint empties the rect");
    t = a;
    CHECK(!canvas_rect_intersect(&t, &c), "should not overlap");
    CHECK(canvas_rect_empty(&t), "should be empty");

    TEST("union covers both");
    t = a;
    canvas_rect_union(&t, &b);
    CHECK(t.x == 10 && t.y == 10 && t.w == 30 && t.h == 30,
          "got %d,%d %dx%d", t.x, t.y, t.w, t.h);

    TEST("union with empty is a no-op");
    t = a;
    canvas_rect_union(&t, &(struct canvas_rect){ 0, 0, 0, 0 });
    CHECK(t.w == 20 && t.h == 20, "empty changed the union");

    TEST("union into empty adopts the other");
    t = (struct canvas_rect){ 5, 5, 0, 0 };
    canvas_rect_union(&t, &b);
    CHECK(t.x == 20 && t.w == 20, "got %d,%d %dx%d", t.x, t.y, t.w, t.h);

    TEST("contains");
    CHECK(canvas_rect_contains(&a, &(struct canvas_rect){ 12, 12, 2, 2 }), "inner");
    CHECK(!canvas_rect_contains(&a, &b), "overlapping is not containing");
}

/* ---------------------------------------------------------------- */
/* Blending                                                          */
/* ---------------------------------------------------------------- */

static void test_blend_px(void)
{
    TEST("alpha 255 is exactly the source");
    CHECK(canvas_blend_px(RED, GREEN, 255) == GREEN, "not source");

    TEST("alpha 0 is exactly the destination");
    CHECK(canvas_blend_px(RED, GREEN, 0) == RED, "not destination");

    TEST("half alpha lands between the two");
    {
        canvas_px m = canvas_blend_px(BLACK, WHITE, 128);
        int r = CANVAS_R(m);
        CHECK(r > 96 && r < 160, "midpoint red %d", r);
    }

    TEST("blending is monotonic in alpha");
    {
        int prev = -1, a;
        for (a = 0; a <= 255; a++)
        {
            int r = CANVAS_R(canvas_blend_px(BLACK, WHITE, a));
            CHECK(r >= prev, "alpha %d went backwards (%d < %d)", a, r, prev);
            prev = r;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Drawing and clipping                                              */
/* ---------------------------------------------------------------- */

static void test_fill_and_clip(void)
{
    struct test_surf t;

    ts_make(&t, 16, 16, true);
    ts_fill_all(&t, BLACK, 0);

    TEST("fill paints the rect and nothing else");
    canvas_fill(&t.s, &(struct canvas_rect){ 4, 4, 4, 4 }, RED);
    CHECK(px(&t, 4, 4) == RED, "inside not filled");
    CHECK(px(&t, 7, 7) == RED, "inside not filled");
    CHECK(px(&t, 8, 8) == BLACK, "spilled past the rect");
    CHECK(px(&t, 3, 3) == BLACK, "spilled before the rect");

    TEST("fill clips against the surface");
    canvas_fill(&t.s, &(struct canvas_rect){ -10, -10, 100, 100 }, GREEN);
    CHECK(px(&t, 0, 0) == GREEN && px(&t, 15, 15) == GREEN, "not filled");
    CHECK(ts_guards_intact(&t), "wrote outside the surface");

    TEST("a fully off-surface fill draws nothing");
    canvas_fill(&t.s, &(struct canvas_rect){ 100, 100, 10, 10 }, RED);
    CHECK(ts_guards_intact(&t), "wrote outside the surface");

    ts_free(&t);
}

static void test_blit_clip(void)
{
    struct test_surf dst, src;

    ts_make(&dst, 16, 16, false);
    ts_make(&src, 8, 8, false);
    ts_fill_all(&dst, BLACK, 255);
    ts_fill_all(&src, RED, 255);

    TEST("blit straddling the top-left corner clips both ends");
    canvas_blit(&dst.s, -4, -4, &src.s, &(struct canvas_rect){ 0, 0, 8, 8 });
    CHECK(px(&dst, 0, 0) == RED, "visible part missing");
    CHECK(px(&dst, 3, 3) == RED, "visible part missing");
    CHECK(px(&dst, 4, 4) == BLACK, "drew too much");
    CHECK(ts_guards_intact(&dst), "wrote outside the destination");

    TEST("blit straddling the bottom-right corner clips both ends");
    ts_fill_all(&dst, BLACK, 255);
    canvas_blit(&dst.s, 12, 12, &src.s, &(struct canvas_rect){ 0, 0, 8, 8 });
    CHECK(px(&dst, 15, 15) == RED, "visible part missing");
    CHECK(px(&dst, 11, 11) == BLACK, "drew too much");
    CHECK(ts_guards_intact(&dst), "wrote outside the destination");

    TEST("blend with a transparent source leaves the destination alone");
    ts_free(&src);
    ts_make(&src, 8, 8, true);
    ts_fill_all(&src, RED, 0);
    ts_fill_all(&dst, BLACK, 255);
    canvas_blend(&dst.s, 0, 0, &src.s, &(struct canvas_rect){ 0, 0, 8, 8 }, 255);
    CHECK(px(&dst, 2, 2) == BLACK, "transparent source painted");

    TEST("blend honours layer opacity as well as source alpha");
    ts_fill_all(&src, WHITE, 255);
    ts_fill_all(&dst, BLACK, 255);
    canvas_blend(&dst.s, 0, 0, &src.s, &(struct canvas_rect){ 0, 0, 8, 8 }, 128);
    {
        int r = CANVAS_R(px(&dst, 2, 2));
        CHECK(r > 96 && r < 160, "half-opaque white gave red %d", r);
    }

    ts_free(&dst);
    ts_free(&src);
}

/* ---------------------------------------------------------------- */
/* The compositor                                                    */
/* ---------------------------------------------------------------- */

static void test_compositor(void)
{
    struct test_surf target, bg, ov;
    struct canvas c;
    struct canvas_rect d;
    int lbg, lov;

    ts_make(&target, 32, 32, false);
    ts_make(&bg, 32, 32, false);
    ts_make(&ov, 8, 8, true);

    ts_fill_all(&target, GREEN, 255);
    ts_fill_all(&bg, BLUE, 255);
    ts_fill_all(&ov, RED, 255);

    canvas_init(&c, &target.s);
    lbg = canvas_add_layer(&c, &bg.s, 0, 0, CANVAS_COPY);
    lov = canvas_add_layer(&c, &ov.s, 4, 4, CANVAS_OVER);

    TEST("adding a layer damages where it lands");
    CHECK(canvas_has_damage(&c), "no damage after adding layers");

    TEST("first composite draws both layers");
    d = canvas_composite(&c);
    CHECK(d.w == 32 && d.h == 32, "damage %dx%d, expected full", d.w, d.h);
    CHECK(px(&target, 0, 0) == BLUE, "background missing");
    CHECK(px(&target, 5, 5) == RED, "overlay missing");
    CHECK(!canvas_has_damage(&c), "damage survived the composite");

    TEST("a composite with no damage does nothing");
    d = canvas_composite(&c);
    CHECK(canvas_rect_empty(&d), "composited %dx%d with no damage", d.w, d.h);

    TEST("a moved layer leaves no trail behind it");
    canvas_layer_move(&c, lov, 20, 20);
    d = canvas_composite(&c);
    CHECK(px(&target, 5, 5) == BLUE, "old position still shows the overlay");
    CHECK(px(&target, 21, 21) == RED, "new position missing the overlay");
    CHECK(canvas_rect_contains(&d, &(struct canvas_rect){ 4, 4, 8, 8 }),
          "damage did not cover the vacated rectangle");

    TEST("moving damages only the two rectangles involved");
    canvas_layer_move(&c, lov, 21, 20);
    d = canvas_composite(&c);
    CHECK(d.w <= 10 && d.h <= 9, "damage grew to %dx%d for a one-pixel move",
          d.w, d.h);

    TEST("hiding a layer repaints what was under it");
    canvas_layer_set_visible(&c, lov, false);
    canvas_composite(&c);
    CHECK(px(&target, 22, 21) == BLUE, "hidden layer still on screen");

    TEST("an invisible layer costs nothing to composite");
    CHECK(!canvas_has_damage(&c), "damage left over");

    TEST("opacity zero is treated as invisible");
    canvas_layer_set_visible(&c, lov, true);
    canvas_layer_set_opacity(&c, lov, 0);
    canvas_composite(&c);
    CHECK(px(&target, 22, 21) == BLUE, "zero-opacity layer painted");

    TEST("damage is clipped to the target");
    canvas_damage(&c, &(struct canvas_rect){ -100, -100, 1000, 1000 });
    d = canvas_composite(&c);
    CHECK(d.x == 0 && d.y == 0 && d.w == 32 && d.h == 32,
          "damage %d,%d %dx%d escaped the target", d.x, d.y, d.w, d.h);

    TEST("compositing never writes outside the target");
    CHECK(ts_guards_intact(&target), "wrote outside the target surface");

    (void)lbg;
    ts_free(&target);
    ts_free(&bg);
    ts_free(&ov);
}

/* ---------------------------------------------------------------- */
/* Effects                                                           */
/* ---------------------------------------------------------------- */

static void test_blur(void)
{
    struct test_surf t;
    canvas_px *scratch;
    size_t n;
    struct canvas_rect all = { 0, 0, 64, 64 };
    int x, y;

    ts_make(&t, 64, 64, false);
    n = canvas_blur_scratch_px(64, 64);
    scratch = malloc(n * sizeof(canvas_px));

    TEST("blurring a flat field changes nothing");
    ts_fill_all(&t, CANVAS_RGB(128, 128, 128), 255);
    CHECK(canvas_blur(&t.s, &all, 8, scratch, n), "blur refused");
    for (y = 0; y < 64; y += 7)
        for (x = 0; x < 64; x += 7)
        {
            int r = CANVAS_R(px(&t, x, y));
            CHECK(r > 112 && r < 144, "flat field moved to %d at %d,%d", r, x, y);
        }

    TEST("edges clamp instead of darkening");
    {
        int corner = CANVAS_R(px(&t, 0, 0));
        int middle = CANVAS_R(px(&t, 32, 32));
        CHECK(abs(corner - middle) < 24, "corner %d vs middle %d", corner, middle);
    }

    TEST("a hard edge becomes a ramp");
    ts_fill_all(&t, BLACK, 255);
    canvas_fill(&t.s, &(struct canvas_rect){ 32, 0, 32, 64 }, WHITE);
    CHECK(canvas_blur(&t.s, &all, 12, scratch, n), "blur refused");
    {
        int left = CANVAS_R(px(&t, 16, 32));
        int mid = CANVAS_R(px(&t, 32, 32));
        int right = CANVAS_R(px(&t, 48, 32));
        CHECK(left < mid && mid < right, "not a ramp: %d %d %d", left, mid, right);
        CHECK(mid > 40 && mid < 215, "edge did not soften (%d)", mid);
    }

    TEST("blur declines a scratch that is too small");
    CHECK(!canvas_blur(&t.s, &all, 8, scratch, 4), "accepted a tiny scratch");

    TEST("blur declines a degenerate rectangle");
    CHECK(!canvas_blur(&t.s, &(struct canvas_rect){ 0, 0, 3, 3 }, 4, scratch, n),
          "accepted a 3x3 rect");

    TEST("blur stays inside the surface");
    CHECK(ts_guards_intact(&t), "wrote outside the surface");

    free(scratch);
    ts_free(&t);
}

static void test_round_rect(void)
{
    struct test_surf t;
    struct canvas_rect r = { 4, 4, 24, 24 };

    ts_make(&t, 32, 32, true);
    ts_fill_all(&t, WHITE, 0);

    TEST("the middle is fully inside and the far corner fully outside");
    canvas_round_rect(&t.s, &r, 8, 255, 0);
    CHECK(al(&t, 16, 16) == 255, "centre alpha %d", al(&t, 16, 16));
    CHECK(al(&t, 0, 0) == 0, "outside alpha %d", al(&t, 0, 0));
    CHECK(al(&t, 5, 5) < 255, "corner of the bounding box is not cut away");

    TEST("the corner is antialiased, not stepped");
    {
        int partials = 0, x, y;
        for (y = 4; y < 16; y++)
            for (x = 4; x < 16; x++)
            {
                uint8_t a = al(&t, x, y);
                if (a > 8 && a < 247)
                    partials++;
            }
        CHECK(partials >= 6, "only %d partial pixels on the curve", partials);
    }

    TEST("a zero radius is a plain rectangle");
    canvas_round_rect(&t.s, &r, 0, 255, 0);
    CHECK(al(&t, 4, 4) == 255, "square corner alpha %d", al(&t, 4, 4));
    CHECK(al(&t, 3, 3) == 0, "outside the square is opaque");

    ts_free(&t);
}

static void test_round_shapes(void)
{
    struct test_surf t;
    struct canvas_rect disc = { 8, 8, 32, 32 };

    ts_make(&t, 48, 48, false);

    TEST("a filled circle is solid in the middle and clear at the corners");
    ts_fill_all(&t, BLACK, 255);
    canvas_fill_round_rect(&t.s, &disc, 16, WHITE, 255);
    CHECK(px(&t, 24, 24) == WHITE, "centre not filled");
    CHECK(px(&t, 8, 8) == BLACK, "bounding-box corner filled");
    CHECK(px(&t, 24, 8) == WHITE, "top of the circle missing");

    TEST("its edge is antialiased");
    {
        /* Counted over the whole curve, not across one row: a row that
         * crosses the boundary square-on can legitimately step from solid
         * to clear in a single pixel. It is the diagonals that have to be
         * feathered, and there are plenty of them. */
        int partials = 0, x, y;
        for (y = 0; y < 48; y++)
            for (x = 0; x < 48; x++)
            {
                int r = CANVAS_R(px(&t, x, y));
                if (r > 16 && r < 239)
                    partials++;
            }
        CHECK(partials >= 16, "only %d partial pixels on the curve", partials);
    }

    TEST("a stroke is hollow, and sits on the boundary");
    ts_fill_all(&t, BLACK, 255);
    canvas_stroke_round_rect(&t.s, &disc, 16, 2, WHITE, 255);
    CHECK(px(&t, 24, 24) == BLACK, "stroke filled the middle");
    CHECK(CANVAS_R(px(&t, 24, 8)) > 128, "stroke missing at the top");
    CHECK(px(&t, 24, 0) == BLACK, "stroke drawn well outside the shape");

    TEST("a thicker stroke covers more pixels");
    {
        int thin = 0, thick = 0, y;
        ts_fill_all(&t, BLACK, 255);
        canvas_stroke_round_rect(&t.s, &disc, 16, 1, WHITE, 255);
        for (y = 0; y < 48; y++)
            if (CANVAS_R(px(&t, 24, y)) > 64) thin++;
        ts_fill_all(&t, BLACK, 255);
        canvas_stroke_round_rect(&t.s, &disc, 16, 5, WHITE, 255);
        for (y = 0; y < 48; y++)
            if (CANVAS_R(px(&t, 24, y)) > 64) thick++;
        CHECK(thick > thin, "5 px stroke covered %d, 1 px covered %d",
              thick, thin);
    }

    TEST("opacity scales the whole shape");
    ts_fill_all(&t, BLACK, 255);
    canvas_fill_round_rect(&t.s, &disc, 16, WHITE, 128);
    {
        int r = CANVAS_R(px(&t, 24, 24));
        CHECK(r > 96 && r < 160, "half-opaque fill gave %d", r);
    }

    TEST("round shapes stay inside the surface");
    canvas_fill_round_rect(&t.s, &(struct canvas_rect){ -20, -20, 100, 100 },
                           10, WHITE, 255);
    canvas_stroke_round_rect(&t.s, &(struct canvas_rect){ -20, -20, 100, 100 },
                             10, 4, WHITE, 255);
    CHECK(ts_guards_intact(&t), "wrote outside the surface");

    ts_free(&t);
}

static void test_shadow(void)
{
    struct test_surf t;
    struct canvas_rect r = { 16, 16, 16, 16 };

    ts_make(&t, 48, 48, false);
    ts_fill_all(&t, WHITE, 255);

    TEST("the shadow is darkest under the shape and fades outward");
    canvas_shadow(&t.s, &r, 4, 8, 0, 0, BLACK, 200);
    {
        int under = CANVAS_R(px(&t, 24, 24));
        int near = CANVAS_R(px(&t, 24, 12));
        int far = CANVAS_R(px(&t, 24, 6));
        CHECK(under < near, "under %d not darker than near %d", under, near);
        CHECK(near < far, "near %d not darker than far %d", near, far);
    }

    TEST("nothing is drawn beyond the blur radius");
    CHECK(px(&t, 24, 2) == WHITE, "shadow reached %d,%d", 24, 2);
    CHECK(ts_guards_intact(&t), "wrote outside the surface");

    TEST("an offset shadow moves with the offset");
    ts_fill_all(&t, WHITE, 255);
    canvas_shadow(&t.s, &r, 4, 6, 6, 6, BLACK, 200);
    CHECK(CANVAS_R(px(&t, 36, 36)) < CANVAS_R(px(&t, 12, 12)),
          "offset shadow did not move down-right");

    ts_free(&t);
}

static void test_gradient(void)
{
    struct test_surf t;
    struct canvas_rect all = { 0, 0, 32, 64 };
    int y, prev;

    ts_make(&t, 32, 64, false);

    TEST("a vertical gradient runs from one stop to the other");
    canvas_gradient(&t.s, &all, BLACK, WHITE, true, false);
    CHECK(CANVAS_R(px(&t, 0, 0)) < 8, "top is not the first stop");
    CHECK(CANVAS_R(px(&t, 0, 63)) > 247, "bottom is not the second stop");

    TEST("an undithered gradient is monotonic");
    prev = -1;
    for (y = 0; y < 64; y++)
    {
        int r = CANVAS_R(px(&t, 0, y));
        CHECK(r >= prev, "row %d went backwards (%d < %d)", y, r, prev);
        prev = r;
    }

    TEST("dithering breaks up the bands without shifting the ramp");
    {
        long plain = 0, dith = 0;
        int x;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 32; x++)
                plain += CANVAS_R(px(&t, x, y));

        canvas_gradient(&t.s, &all, BLACK, WHITE, true, true);
        for (y = 0; y < 64; y++)
            for (x = 0; x < 32; x++)
                dith += CANVAS_R(px(&t, x, y));

        CHECK(labs(plain - dith) < (64 * 32 * 6),
              "dither moved the mean by %ld", labs(plain - dith) / (64 * 32));
    }

    TEST("dithering breaks the bands up where they would otherwise step");
    {
        /* Only rows whose exact value sits near a quantisation boundary
         * can dither at all - which is the point, and why this counts
         * rows that vary rather than demanding that every row does. */
        int x, varied = 0;
        for (y = 0; y < 64; y++)
        {
            int first = CANVAS_R(px(&t, 0, y));
            for (x = 1; x < 32; x++)
                if (CANVAS_R(px(&t, x, y)) != first)
                {
                    varied++;
                    break;
                }
        }
        CHECK(varied >= 8, "only %d of 64 dithered rows varied", varied);
    }

    ts_free(&t);
}

static void test_9slice(void)
{
    struct test_surf dst, src;
    struct canvas_rect d = { 0, 0, 40, 40 };
    int x, y;

    ts_make(&src, 12, 12, false);
    ts_make(&dst, 40, 40, false);

    /* a frame: red border, green middle, blue single-pixel corners */
    ts_fill_all(&src, GREEN, 255);
    for (y = 0; y < 12; y++)
        for (x = 0; x < 12; x++)
            if (x < 4 || y < 4 || x >= 8 || y >= 8)
                *canvas_at(&src.s, x, y) = RED;
    *canvas_at(&src.s, 0, 0) = BLUE;
    *canvas_at(&src.s, 11, 0) = BLUE;
    *canvas_at(&src.s, 0, 11) = BLUE;
    *canvas_at(&src.s, 11, 11) = BLUE;

    ts_fill_all(&dst, BLACK, 255);
    canvas_9slice(&dst.s, &d, &src.s, 4, 4, 4, 4);

    TEST("corners survive unstretched");
    CHECK(px(&dst, 0, 0) == BLUE, "top-left corner lost");
    CHECK(px(&dst, 39, 0) == BLUE, "top-right corner lost");
    CHECK(px(&dst, 0, 39) == BLUE, "bottom-left corner lost");
    CHECK(px(&dst, 39, 39) == BLUE, "bottom-right corner lost");

    TEST("the border stays a border and the centre stretches");
    CHECK(px(&dst, 20, 1) == RED, "top edge is not the border");
    CHECK(px(&dst, 20, 20) == GREEN, "centre did not stretch");
    CHECK(px(&dst, 1, 20) == RED, "left edge is not the border");

    TEST("nine-slice stays inside the destination");
    CHECK(ts_guards_intact(&dst), "wrote outside the destination");

    ts_free(&dst);
    ts_free(&src);
}

static void test_vinyl(void)
{
    struct test_surf dst, lab;
    const canvas_px accent = RED;

    ts_make(&dst, 64, 64, false);
    ts_make(&lab, 16, 16, false);
    ts_fill_all(&lab, WHITE, 255);
    canvas_fill(&lab.s, &(struct canvas_rect){ 12, 0, 4, 16 }, BLACK);

    ts_fill_all(&dst, GREEN, 255);
    canvas_vinyl(&dst.s, 32, 32, 30, 0, NULL, 40, accent, BLACK);

    TEST("vinyl leaves the corners alone");
    CHECK(px(&dst, 0, 0) == GREEN && px(&dst, 63, 63) == GREEN,
          "wrote outside the circle");

    TEST("vinyl has a hole in the middle");
    CHECK(px(&dst, 32, 32) == BLACK, "centre is %04x", px(&dst, 32, 32));

    TEST("vinyl covers its own disc");
    CHECK(px(&dst, 32, 32 + 25) != GREEN, "groove area not drawn");

    TEST("a bare label is the accent");
    CHECK(px(&dst, 32, 32 - 8) == accent, "label is %04x", px(&dst, 32, 24));

    TEST("the bare label's mark turns with the record");
    {
        canvas_px right0 = px(&dst, 32 + 9, 32);
        canvas_vinyl(&dst.s, 32, 32, 30, 90, NULL, 40, accent, BLACK);
        CHECK(right0 != accent && px(&dst, 32 + 9, 32) == accent &&
              px(&dst, 32, 32 + 9) != accent,
              "mark did not move a quarter turn clockwise");
    }

    TEST("a cover label rotates with the record");
    {
        /* The dark band is on the right of the cover. Half a turn puts it
         * on the left. */
        canvas_vinyl(&dst.s, 32, 32, 30, 0, &lab.s, 40, accent, BLACK);
        CHECK(px(&dst, 32 + 10, 32) == BLACK && px(&dst, 32 - 10, 32) == WHITE,
              "unturned cover is not the right way round");
        canvas_vinyl(&dst.s, 32, 32, 30, 180, &lab.s, 40, accent, BLACK);
        CHECK(px(&dst, 32 - 10, 32) == BLACK && px(&dst, 32 + 10, 32) == WHITE,
              "half a turn did not move the band across");
    }

    TEST("vinyl clips at the surface edge");
    canvas_vinyl(&dst.s, 2, 2, 30, 45, &lab.s, 40, accent, BLACK);
    CHECK(ts_guards_intact(&dst), "wrote outside the destination");

    ts_free(&dst);
    ts_free(&lab);
}

static void test_reflect(void)
{
    struct test_surf dst, src;
    struct canvas_rect s = { 0, 0, 16, 16 };

    ts_make(&src, 16, 16, false);
    ts_make(&dst, 16, 32, false);

    ts_fill_all(&src, BLACK, 255);
    canvas_fill(&src.s, &(struct canvas_rect){ 0, 0, 16, 4 }, RED);
    canvas_fill(&src.s, &(struct canvas_rect){ 0, 12, 16, 4 }, WHITE);
    ts_fill_all(&dst, BLACK, 255);

    canvas_reflect(&dst.s, 0, 0, &src.s, &s, 16, 255, 0);

    TEST("the reflection is flipped");
    CHECK(CANVAS_R(px(&dst, 8, 0)) > 200, "bottom of the source is not on top");

    TEST("the reflection fades to nothing");
    CHECK(px(&dst, 8, 15) == BLACK, "the far end of the fade is not clear");

    TEST("the fade is monotonic across a constant band");
    {
        int a = CANVAS_R(px(&dst, 8, 0));
        int b = CANVAS_R(px(&dst, 8, 2));
        CHECK(a >= b, "fade went the wrong way (%d then %d)", a, b);
    }

    TEST("reflection stays inside the destination");
    CHECK(ts_guards_intact(&dst), "wrote outside the destination");

    ts_free(&dst);
    ts_free(&src);
}

/* ---------------------------------------------------------------- */
/* Text coverage                                                     */
/* ---------------------------------------------------------------- */

static void test_coverage(void)
{
    struct test_surf t;
    uint8_t cov[4 * 4];
    int i;

    ts_make(&t, 8, 8, false);
    ts_fill_all(&t, BLACK, 255);

    for (i = 0; i < 16; i++)
        cov[i] = (i == 5) ? 255 : 0;

    TEST("zero coverage leaves the destination untouched");
    canvas_blend_coverage(&t.s, 2, 2, cov, 4, 4, 4, WHITE, false);
    CHECK(px(&t, 2, 2) == BLACK, "zero coverage painted");

    TEST("full coverage is exactly the colour");
    CHECK(px(&t, 3, 3) == WHITE, "full coverage did not paint the colour");

    TEST("coverage clips at the destination edge");
    canvas_blend_coverage(&t.s, -2, -2, cov, 4, 4, 4, WHITE, false);
    canvas_blend_coverage(&t.s, 6, 6, cov, 4, 4, 4, WHITE, false);
    CHECK(ts_guards_intact(&t), "wrote outside the surface");

    TEST("gamma correction is monotonic and keeps the endpoints");
    {
        int prev = -1;
        for (i = 0; i <= 255; i++)
        {
            uint8_t one = (uint8_t)i;
            int r;
            ts_fill_all(&t, BLACK, 255);
            canvas_blend_coverage(&t.s, 0, 0, &one, 1, 1, 1, WHITE, true);
            r = CANVAS_R(px(&t, 0, 0));
            CHECK(r >= prev, "coverage %d went backwards (%d < %d)", i, r, prev);
            prev = r;
        }
        ts_fill_all(&t, BLACK, 255);
        {
            uint8_t full = 255;
            canvas_blend_coverage(&t.s, 0, 0, &full, 1, 1, 1, WHITE, true);
            CHECK(px(&t, 0, 0) == WHITE, "gamma broke full coverage");
        }
    }

    TEST("gamma lifts the midtones rather than dropping them");
    {
        uint8_t half = 128;
        int plain, corrected;
        ts_fill_all(&t, BLACK, 255);
        canvas_blend_coverage(&t.s, 0, 0, &half, 1, 1, 1, WHITE, false);
        plain = CANVAS_R(px(&t, 0, 0));
        ts_fill_all(&t, BLACK, 255);
        canvas_blend_coverage(&t.s, 0, 0, &half, 1, 1, 1, WHITE, true);
        corrected = CANVAS_R(px(&t, 0, 0));
        CHECK(corrected > plain, "gamma did not lift 50%% (%d vs %d)",
              corrected, plain);
    }

    ts_free(&t);
}

static void test_font4_expand(void)
{
    /* two rows of four pixels, packed two to a byte, counting down from
     * opaque - the format Rockbox font files already use */
    const uint8_t src[4] = { 0x0f, 0xf0, 0x08, 0x80 };
    uint8_t cov[8];

    canvas_expand_font4(cov, 4, src, 4, 4, 2);

    TEST("nibble order is low first");
    CHECK(cov[0] == 0, "pixel 0 should be transparent, got %d", cov[0]);
    CHECK(cov[1] == 255, "pixel 1 should be opaque, got %d", cov[1]);

    TEST("the scale is inverted and spans the full range");
    CHECK(cov[2] == 255, "pixel 2 got %d", cov[2]);
    CHECK(cov[3] == 0, "pixel 3 got %d", cov[3]);

    TEST("rows may share a byte");
    CHECK(cov[4] == (15 - 8) * 17, "row 1 pixel 0 got %d", cov[4]);
}

/* ---------------------------------------------------------------- */
/* Animation                                                         */
/* ---------------------------------------------------------------- */

static void test_anim(void)
{
    struct canvas_anim a;
    int i;

    TEST("an animation starts at from and ends at to, exactly");
    canvas_anim_start(&a, 100, 300, 500, CANVAS_EASE_OUT_CUBIC, 1000);
    CHECK(canvas_anim_value(&a, 1000) == 100, "start %d",
          canvas_anim_value(&a, 1000));
    CHECK(canvas_anim_value(&a, 1500) == 300, "end %d",
          canvas_anim_value(&a, 1500));
    CHECK(canvas_anim_value(&a, 9999) == 300, "overrun %d",
          canvas_anim_value(&a, 9999));

    TEST("done only once the duration is up");
    CHECK(!canvas_anim_done(&a, 1250), "done too early");
    CHECK(canvas_anim_done(&a, 1500), "not done at the end");

    TEST("every easing curve is monotonic and hits both ends");
    for (i = 0; i <= CANVAS_EASE_IN_OUT_CUBIC; i++)
    {
        int t, prev = -1;
        CHECK(canvas_ease_q8(i, 0) == 0, "curve %d does not start at 0", i);
        CHECK(canvas_ease_q8(i, 256) == 256, "curve %d does not end at 256", i);
        for (t = 0; t <= 256; t++)
        {
            int v = canvas_ease_q8(i, t);
            CHECK(v >= prev, "curve %d went backwards at %d", i, t);
            CHECK(v >= -1 && v <= 257, "curve %d overshot to %d", i, v);
            prev = v;
        }
    }

    TEST("an out curve is ahead of linear in the first half");
    CHECK(canvas_ease_q8(CANVAS_EASE_OUT_CUBIC, 64) >
          canvas_ease_q8(CANVAS_EASE_LINEAR, 64), "out cubic is not ahead");

    TEST("a stopped animation reads as finished at its destination");
    canvas_anim_stop(&a);
    CHECK(canvas_anim_done(&a, 1100), "stopped but not done");
    CHECK(canvas_anim_value(&a, 1100) == 300, "stopped but not at the end");

    TEST("a backwards animation works too");
    canvas_anim_start(&a, 300, 100, 200, CANVAS_EASE_LINEAR, 0);
    CHECK(canvas_anim_value(&a, 100) < 300 && canvas_anim_value(&a, 100) > 100,
          "midpoint %d", canvas_anim_value(&a, 100));
    CHECK(canvas_anim_value(&a, 200) == 100, "end %d", canvas_anim_value(&a, 200));
}

/* ---------------------------------------------------------------- */
/* Fuzz: whatever happens, stay inside the surfaces and settle       */
/* ---------------------------------------------------------------- */

static unsigned rng_state = 12345;

static unsigned rnd(unsigned n)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) % (n ? n : 1);
}

static void test_fuzz(int iterations)
{
    struct test_surf target, bg, l1, l2;
    struct canvas c;
    int i, a, b;
    bool guards_ok = true, settled = true;

    ts_make(&target, 64, 96, false);
    ts_make(&bg, 64, 96, false);
    ts_make(&l1, 20, 12, true);
    ts_make(&l2, 33, 41, true);

    ts_fill_all(&bg, BLUE, 255);
    ts_fill_all(&l1, RED, 200);
    ts_fill_all(&l2, GREEN, 90);

    canvas_init(&c, &target.s);
    canvas_add_layer(&c, &bg.s, 0, 0, CANVAS_COPY);
    a = canvas_add_layer(&c, &l1.s, 0, 0, CANVAS_OVER);
    b = canvas_add_layer(&c, &l2.s, 0, 0, CANVAS_OVER);

    for (i = 0; i < iterations; i++)
    {
        switch (rnd(6))
        {
        case 0:
            canvas_layer_move(&c, a, (int)rnd(200) - 70, (int)rnd(200) - 70);
            break;
        case 1:
            canvas_layer_move(&c, b, (int)rnd(200) - 70, (int)rnd(200) - 70);
            break;
        case 2:
            canvas_layer_set_opacity(&c, a, rnd(256));
            break;
        case 3:
            canvas_layer_set_visible(&c, b, rnd(2) != 0);
            break;
        case 4:
            canvas_damage(&c, &(struct canvas_rect){
                (int)rnd(120) - 30, (int)rnd(160) - 30,
                (int)rnd(90), (int)rnd(90) });
            break;
        default:
            canvas_layer_damage(&c, a, &(struct canvas_rect){
                (int)rnd(30) - 10, (int)rnd(30) - 10,
                (int)rnd(40), (int)rnd(40) });
            break;
        }

        canvas_composite(&c);

        if (canvas_has_damage(&c))
            settled = false;
        if (!ts_guards_intact(&target))
        {
            guards_ok = false;
            break;
        }
    }

    TEST("fuzz never writes outside the target");
    CHECK(guards_ok, "guard bytes clobbered after %d iterations", i);

    TEST("fuzz always leaves the canvas settled");
    CHECK(settled, "damage survived a composite");

    ts_free(&target);
    ts_free(&bg);
    ts_free(&l1);
    ts_free(&l2);
}

/* ---------------------------------------------------------------- */
/* PPM dumps, for looking at                                         */
/* ---------------------------------------------------------------- */

static void dump_ppm(const char *name, struct test_surf *t)
{
    FILE *f = fopen(name, "wb");
    int x, y;

    if (!f)
        return;

    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    for (y = 0; y < t->h; y++)
        for (x = 0; x < t->w; x++)
        {
            canvas_px p = px(t, x, y);
            unsigned char rgb[3] = { CANVAS_R(p), CANVAS_G(p), CANVAS_B(p) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    printf("  wrote %s\n", name);
}

static void dump_samples(void)
{
    struct test_surf t, card;
    struct canvas_rect all = { 0, 0, 240, 320 };
    canvas_px *scratch;
    size_t n;

    ts_make(&t, 240, 320, false);
    ts_make(&card, 180, 100, true);

    /* a background worth blurring */
    canvas_gradient(&t.s, &all, CANVAS_RGB(20, 40, 110),
                    CANVAS_RGB(200, 90, 40), true, true);
    canvas_fill(&t.s, &(struct canvas_rect){ 30, 40, 60, 60 }, WHITE);
    canvas_fill(&t.s, &(struct canvas_rect){ 150, 200, 70, 70 }, BLACK);
    dump_ppm("canvas_background.ppm", &t);

    n = canvas_blur_scratch_px(240, 320);
    scratch = malloc(n * sizeof(canvas_px));
    canvas_blur(&t.s, &(struct canvas_rect){ 0, 110, 240, 110 }, 20, scratch, n);
    dump_ppm("canvas_blur_band.ppm", &t);

    /* a frosted card with a shadow and rounded corners */
    canvas_shadow(&t.s, &(struct canvas_rect){ 30, 120, 180, 100 },
                  16, 12, 0, 6, BLACK, 160);
    ts_fill_all(&card, CANVAS_RGB(245, 245, 250), 0);
    canvas_round_rect(&card.s, &(struct canvas_rect){ 0, 0, 180, 100 },
                      16, 210, 0);
    canvas_blend(&t.s, 30, 120, &card.s,
                 &(struct canvas_rect){ 0, 0, 180, 100 }, 255);
    dump_ppm("canvas_card.ppm", &t);

    free(scratch);
    ts_free(&t);
    ts_free(&card);
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    bool dump = false;
    int i;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--dump"))
            dump = true;

    printf("Rockpocket Canvas host tests\n");

    test_rects();
    test_blend_px();
    test_fill_and_clip();
    test_blit_clip();
    test_compositor();
    test_blur();
    test_round_rect();
    test_round_shapes();
    test_shadow();
    test_gradient();
    test_9slice();
    test_reflect();
    test_vinyl();
    test_coverage();
    test_font4_expand();
    test_anim();
    test_fuzz(20000);

    if (dump)
        dump_samples();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
