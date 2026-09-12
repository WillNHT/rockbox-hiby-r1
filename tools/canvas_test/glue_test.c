/* Host tests for apps/canvas_glue.c - the Rockbox side of the Canvas.
 *
 * The framebuffer, buflib and lcd_update_rect() are all faked here, which
 * is enough to test the one thing that matters about this layer: an
 * overlay that moves puts back exactly what it covered, and asks the panel
 * for exactly the region that changed. That is the bug class ("sticky
 * styling", the smearing overlay) the compositor exists to close, and it
 * is otherwise only observable by looking at a device.
 *
 * The fake buflib moves the allocation on every call, because the real one
 * may: any pointer cached across a call would fail here loudly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd.h"
#include "core_alloc.h"
#include "canvas_glue.h"

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

/* ---- the fake framebuffer ---------------------------------------- */

static fb_data fb[LCD_WIDTH * LCD_HEIGHT];

struct frame_buffer_t lcd_framebuffer_default =
{
    { .fb_ptr = fb },
    .get_address_fn = NULL,
    .stride = LCD_WIDTH,
    .elems = LCD_WIDTH * LCD_HEIGHT,
};

static struct { int x, y, w, h; int calls; } last_update;

void lcd_update_rect(int x, int y, int width, int height)
{
    last_update.x = x;
    last_update.y = y;
    last_update.w = width;
    last_update.h = height;
    last_update.calls++;
}

/* ---- the fake buflib --------------------------------------------- */

static char *block;
static size_t block_size;

int core_alloc(size_t size)
{
    if (block)
        return -1;
    block = malloc(size);
    block_size = size;
    return block ? 1 : -1;
}

int core_free(int handle)
{
    (void)handle;
    free(block);
    block = NULL;
    block_size = 0;
    return -1;
}

void *core_get_data(int handle)
{
    /* move it, every single time */
    char *moved = malloc(block_size);

    (void)handle;
    memcpy(moved, block, block_size);
    free(block);
    block = moved;

    return block;
}

/* ---- helpers ------------------------------------------------------ */

static void fb_fill(canvas_px c)
{
    int i;
    for (i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
        fb[i] = c;
}

static canvas_px at(int x, int y)
{
    return fb[(size_t)y * LCD_WIDTH + x];
}

static const canvas_px BG  = CANVAS_RGB(0, 0, 255);
static const canvas_px INK = CANVAS_RGB(255, 0, 0);

int main(void)
{
    struct canvas_overlay *ov;
    struct canvas_surface *s;
    struct canvas_rect all;

    printf("Rockpocket Canvas glue tests\n");

    TEST("the pool allocates");
    CHECK(canvas_glue_init(), "no pool");
    CHECK(canvas_available(), "pool not reported available");
    CHECK(canvas_pool_size() == CANVAS_POOL_MAX_KIB * 1024,
          "pool is %u bytes", (unsigned)canvas_pool_size());

    TEST("an overlay can be claimed and drawn into");
    ov = canvas_overlay_claim(64, 64);
    CHECK(ov != NULL, "claim failed");
    s = canvas_overlay_surface(ov);
    CHECK(s != NULL, "no surface");
    all = (struct canvas_rect){ 0, 0, 64, 64 };
    canvas_fill_a(s, &all, INK, 255);

    TEST("showing the overlay paints it and nothing else");
    fb_fill(BG);
    CHECK(canvas_overlay_show(ov, 100, 100, 64, 64, 255), "show refused");
    CHECK(at(100, 100) == INK, "overlay not painted");
    CHECK(at(163, 163) == INK, "overlay not painted to its far corner");
    CHECK(at(164, 164) == BG, "overlay painted past its rectangle");
    CHECK(at(99, 99) == BG, "overlay painted before its rectangle");

    TEST("presenting asks for exactly the rectangle that changed");
    last_update.calls = 0;
    CHECK(canvas_present(), "nothing to present");
    CHECK(last_update.calls == 1, "%d update calls", last_update.calls);
    CHECK(last_update.x == 100 && last_update.y == 100 &&
          last_update.w == 64 && last_update.h == 64,
          "updated %d,%d %dx%d", last_update.x, last_update.y,
          last_update.w, last_update.h);

    TEST("nothing left to present the second time");
    CHECK(!canvas_present(), "presented twice");

    TEST("moving the overlay leaves no trail - the whole point");
    CHECK(canvas_overlay_show(ov, 200, 300, 64, 64, 255), "show refused");
    CHECK(at(100, 100) == BG, "old position still inked");
    CHECK(at(163, 163) == BG, "old position still inked");
    CHECK(at(200, 300) == INK, "new position not inked");

    TEST("the present after a move covers both rectangles");
    CHECK(canvas_present(), "nothing to present");
    CHECK(last_update.x <= 100 && last_update.y <= 100, "update starts at %d,%d",
          last_update.x, last_update.y);
    CHECK(last_update.x + last_update.w >= 264 &&
          last_update.y + last_update.h >= 364,
          "update ends at %d,%d",
          last_update.x + last_update.w, last_update.y + last_update.h);

    TEST("restoring puts the screen back and forgets the position");
    CHECK(canvas_overlay_restore(ov), "restore did nothing");
    CHECK(at(200, 300) == BG, "restore left ink behind");
    CHECK(!canvas_overlay_restore(ov), "restored twice");

    TEST("an overlay clipped by the screen edge still restores cleanly");
    fb_fill(BG);
    CHECK(canvas_overlay_show(ov, LCD_WIDTH - 10, LCD_HEIGHT - 10, 64, 64, 255),
          "show refused at the corner");
    CHECK(at(LCD_WIDTH - 1, LCD_HEIGHT - 1) == INK, "corner not painted");
    canvas_overlay_restore(ov);
    CHECK(at(LCD_WIDTH - 1, LCD_HEIGHT - 1) == BG, "corner not restored");

    TEST("a wholly off-screen position is refused rather than drawn");
    CHECK(!canvas_overlay_show(ov, LCD_WIDTH + 5, 0, 64, 64, 255),
          "accepted an off-screen position");

    TEST("a partly transparent overlay blends rather than replaces");
    fb_fill(BG);
    canvas_fill_a(s, &all, INK, 128);
    canvas_overlay_show(ov, 10, 10, 64, 64, 255);
    {
        canvas_px p = at(20, 20);
        CHECK(p != BG && p != INK, "half-alpha overlay did not blend (%04x)", p);
    }
    canvas_overlay_restore(ov);
    CHECK(at(20, 20) == BG, "blended overlay did not restore exactly");

    TEST("invalidate drops the saved backdrop without restoring it");
    canvas_fill_a(s, &all, INK, 255);
    canvas_overlay_show(ov, 10, 10, 64, 64, 255);
    canvas_overlay_invalidate(ov);
    CHECK(!canvas_overlay_restore(ov), "restored after invalidate");
    CHECK(at(20, 20) == INK, "invalidate repainted the backdrop");

    TEST("blur scratch comes out of the pool");
    {
        size_t n = 0;
        canvas_px *sc = canvas_scratch(240, 240, &n);
        CHECK(sc != NULL, "no scratch");
        CHECK(n >= canvas_blur_scratch_px(240, 240), "scratch too small (%u)",
              (unsigned)n);
    }

    TEST("an overlay larger than the pool is refused, not half-allocated");
    {
        struct canvas_overlay *big = canvas_overlay_claim(LCD_WIDTH, LCD_HEIGHT);
        CHECK(big == NULL, "claimed an overlay the pool cannot hold");
    }

    TEST("releasing and shutting down is clean");
    canvas_overlay_release(ov);
    canvas_glue_shutdown();
    CHECK(!canvas_available(), "still available after shutdown");
    CHECK(!canvas_present(), "presented with no pool");

    TEST("a claim after shutdown brings the pool back by itself");
    CHECK(canvas_overlay_claim(8, 8) != NULL, "claim did not re-init");
    CHECK(canvas_available(), "pool not available after a lazy claim");

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
