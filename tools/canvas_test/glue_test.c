/* Host tests for apps/canvas_glue.c - the Rockbox side of the Canvas.
 *
 * Since the overlays moved into their own layer (lcd-layers.c) the glue is
 * only the pool and the blur scratch, and that is what is tested here:
 * the pool is capped, the scratch stays inside it and refuses what it
 * cannot hold, and a pool that cannot be allocated degrades to "no
 * compositor" rather than to a crash.
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

/* ---- the fake buflib --------------------------------------------- */

static char *block;
static size_t block_size;
static int alloc_fails, alloc_calls;

int core_alloc(size_t size)
{
    alloc_calls++;
    if (block || alloc_fails)
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

static int inside_pool(const canvas_px *p, size_t px)
{
    const char *c = (const char *)p;
    return c >= block && c + px * sizeof(canvas_px) <= block + block_size;
}

int main(void)
{
    canvas_px *sc;
    size_t n;

    printf("Rockpocket Canvas glue tests\n");

    TEST("the pool allocates, capped");
    CHECK(canvas_glue_init(), "no pool");
    CHECK(canvas_available(), "pool not reported available");
    CHECK(canvas_pool_size() == CANVAS_POOL_MAX_KIB * 1024,
          "pool is %u bytes", (unsigned)canvas_pool_size());
    CHECK(canvas_pool_used() == 0, "%u bytes used before any call",
          (unsigned)canvas_pool_used());

    TEST("init is safe to call again and does not allocate twice");
    alloc_calls = 0;
    CHECK(canvas_glue_init(), "second init failed");
    CHECK(alloc_calls == 0, "second init allocated %d times", alloc_calls);

    TEST("blur scratch comes out of the pool, big enough");
    n = 0;
    sc = canvas_scratch(240, 240, &n);
    CHECK(sc != NULL, "no scratch");
    CHECK(n >= canvas_blur_scratch_px(240, 240), "scratch too small (%u)",
          (unsigned)n);
    CHECK(sc && inside_pool(sc, n), "scratch is outside the pool");

    TEST("the scratch is the tail of the pool, reused every call");
    {
        canvas_px *again = canvas_scratch(240, 240, &n);
        const char *base = block;
        CHECK(again && (const char *)again + ((n * sizeof(canvas_px) + 3u) & ~3u)
                       == base + block_size,
              "scratch does not end at the pool's end");
    }

    TEST("a scratch larger than the pool is refused, not half-handed out");
    n = 12345;
    CHECK(canvas_scratch(8192, 8192, &n) == NULL,
          "handed out a scratch the pool cannot hold");
    CHECK(n == 12345, "px_out written on refusal");

    TEST("shutting down is clean");
    canvas_glue_shutdown();
    CHECK(!canvas_available(), "still available after shutdown");
    CHECK(block == NULL, "pool not freed");
    CHECK(canvas_pool_size() == 0, "size kept after shutdown");
    CHECK(canvas_scratch(16, 16, NULL) == NULL, "scratch with no pool");

    TEST("no memory means no compositor, not a crash");
    alloc_fails = 1;
    CHECK(!canvas_glue_init(), "init claimed success with no memory");
    CHECK(!canvas_available(), "available with no pool");
    CHECK(canvas_pool_size() == 0, "size reported with no pool");
    CHECK(canvas_scratch(16, 16, NULL) == NULL, "scratch with no pool");

    TEST("the pool comes back once memory does");
    alloc_fails = 0;
    CHECK(canvas_glue_init(), "init failed after memory returned");
    CHECK(canvas_scratch(16, 16, NULL) != NULL, "no scratch after recovery");
    canvas_glue_shutdown();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
