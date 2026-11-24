/* Host harness for apps/recorder/jpeg_load.c - the decoder behind every
 * cover, backdrop and playlist thumbnail.
 *
 * A damaged cover.jpg must be refused, never crash or hang the player
 * (#120). This builds the real decoder against a handful of stubs and
 * feeds it files:
 *
 *   jpeg_test FILE...            decode each, both as album art (resized
 *                                into a box) and at full size
 *   jpeg_test --fuzz N FILE...   also decode N mutations of each file
 *
 * Every decode gets a buffer of exactly the size it is promised, so under
 * the sanitizers (make test builds with them) a write past it is caught,
 * and every row the decoder hands out is checked against the bitmap it
 * was asked for. A decode that does not finish in a few seconds is a hang.
 * Files under seeds/ must decode; anything else only has to survive.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "jpeg_load.h"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(p, n)   ((void)(p), (void)(n))
#define ASAN_UNPOISON_MEMORY_REGION(p, n) ((void)(p), (void)(n))
#endif

/* A sanitizer report ends in abort(), so on_abort() can say which
 * mutation it was. */
const char *__asan_default_options(void)
{
    return "abort_on_error=1:detect_leaks=1";
}

const char *__ubsan_default_options(void)
{
    return "abort_on_error=1:print_stacktrace=1";
}

#define BUF_SIZE   (512 * 1024)
#define HANG_SECS  5

/* ---- what jpeg_load.c needs from the rest of Rockbox -------------- */

off_t filesize(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : st.st_size;
}

void yield(void)
{
}

/* apps/recorder/resize.c, verbatim apart from the upscaler being on */
int recalc_dimension(struct dim *dst, struct dim *src)
{
    int sw = src->width * LCD_PIXEL_ASPECT_HEIGHT;
    int sh = src->height * LCD_PIXEL_ASPECT_WIDTH;
    int tmp;
    if (dst->width <= 0)
        dst->width = LCD_WIDTH;
    if (dst->height <= 0)
        dst->height = LCD_HEIGHT;
    tmp = (sw * dst->height + (sh >> 1)) / sh;
    if (tmp > dst->width)
        dst->height = (sh * dst->width + (sw >> 1)) / sw;
    else
        dst->width = tmp;
    return src->width == dst->width && src->height == dst->height;
}

static int rows_out;
static const char *current;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s: %s\n", current, what);
    exit(1);
}

/* The real scaler pulls every source row through store_part; that is the
 * part that exercises the decoder, so do the same and check each row lies
 * inside the buffer the decoder was given. The scaler's own share of the
 * buffer is not used here, so it is poisoned: a decoder writing past its
 * decode buffer into it is caught. */
static volatile unsigned sink;

int resize_on_load(struct bitmap *bm, bool dither, struct dim *src,
                   struct rowset *rset, unsigned char *buf, unsigned int len,
                   const struct custom_format *cformat, int format_index,
                   struct img_part *(*store_part)(void *args), void *args)
{
    unsigned char *lo = bm->data, *hi = bm->data + BUF_SIZE;
    int row, x;

    (void)dither; (void)rset; (void)cformat; (void)format_index;

    if (src->width <= 0 || src->height <= 0)
        fail("resize handed a source with no size");
    if (buf < lo || buf + len > hi)
        fail("resize handed a buffer outside the one given");

    ASAN_POISON_MEMORY_REGION(buf, len);
    for (row = 0; row < src->height; row++)
    {
        struct img_part *part = store_part(args);
        unsigned char *p;

        if (part == NULL)
        {
            ASAN_UNPOISON_MEMORY_REGION(buf, len);
            return 0;
        }
        p = (unsigned char *)part->buf;
        if (part->len < src->width)
            fail("a row shorter than the image");
        if (p < lo || p + part->len * sizeof(*part->buf) > hi)
            fail("a row outside the decode buffer");
        for (x = 0; x < src->width; x++)
            sink += part->buf[x].red;
    }
    ASAN_UNPOISON_MEMORY_REGION(buf, len);
    rows_out = bm->height;
    return 1;
}

void output_row_8_native(uint32_t row, void *row_in,
                         struct scaler_context *ctx)
{
    struct bitmap *bm = ctx->bm;
    struct uint8_rgb *in = row_in;
    fb_data *out;
    int x;

    if ((int)row >= bm->height || row != (uint32_t)rows_out)
        fail("row out of order or past the bitmap");
    out = (fb_data *)bm->data + row * bm->width;
    for (x = 0; x < bm->width; x++)
        out[x] = ((in[x].red >> 3) << 11) | ((in[x].green >> 2) << 5)
                 | (in[x].blue >> 3);
    rows_out++;
}

/* ---- the harness -------------------------------------------------- */

/* the sanitizers report where, this says which file */
static void on_abort(int sig)
{
    (void)sig;
    fprintf(stderr, "FAIL %s: report above\n", current);
    signal(SIGABRT, SIG_DFL);
    abort();
}

static void on_alarm(int sig)
{
    (void)sig;
    fprintf(stderr, "FAIL %s: still decoding after %ds\n", current,
            HANG_SECS);
    _exit(2);
}

/* Decode the file behind fd, the way albumart.c and the backdrop loader
 * do, into a buffer exactly maxsize long. */
static int decode(int fd, int w, int h, int format)
{
    unsigned char *buf = malloc(BUF_SIZE);
    struct bitmap bm;
    int ret;

    memset(&bm, 0, sizeof bm);
    bm.width = w;
    bm.height = h;
    bm.data = buf;
    rows_out = 0;

    lseek(fd, 0, SEEK_SET);
    alarm(HANG_SECS);
    ret = read_jpeg_fd(fd, 0, &bm, BUF_SIZE, format, NULL, NULL);
    alarm(0);

    if (ret > 0 && (bm.width <= 0 || bm.height <= 0 || bm.width > w * 64 + 32767))
        fail("success with a nonsense size");
    if (ret > 0 && rows_out != bm.height)
        fail("success without every row written");

    free(buf);
    return ret;
}

static int decode_both(int fd)
{
    int art = decode(fd, 200, 200,
                     FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT);
    int full = decode(fd, 0, 0, FORMAT_NATIVE);
    return art > 0 && full > 0;
}

static unsigned long rnd_state = 1;
static unsigned long rnd(void)
{
    rnd_state = rnd_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rnd_state >> 33;
}

/* One mutation of src into dst: flipped bits, bytes set to the values a
 * JPEG parser cares about, a cut, or a copied run. Returns the new size. */
static size_t mutate(const unsigned char *src, size_t len, unsigned char *dst)
{
    static const unsigned char magic[] =
        { 0x00, 0x01, 0x02, 0x04, 0x08, 0x0f, 0x10, 0x11, 0x22, 0x3f, 0x40,
          0x7f, 0x80, 0xc0, 0xc4, 0xd0, 0xd8, 0xd9, 0xda, 0xdb, 0xdd, 0xff };
    size_t n = len, i;
    int edits = 1 + rnd() % 8;

    memcpy(dst, src, len);
    while (edits--)
    {
        size_t at = rnd() % n;
        switch (rnd() % 5)
        {
        case 0: dst[at] ^= 1 << (rnd() % 8); break;
        case 1: dst[at] = magic[rnd() % sizeof magic]; break;
        case 2: dst[at] = rnd(); break;
        case 3: if (at > 2) n = at; break;
        case 4:
        {
            size_t from = rnd() % n, run = 1 + rnd() % 16;
            for (i = 0; i < run && at + i < n && from + i < n; i++)
                dst[at + i] = dst[from + i];
            break;
        }
        }
    }
    return n;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n > 0 ? n : 1);
    if (n <= 0 || fread(data, 1, n, f) != (size_t)n)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = n;
    return data;
}

int main(int argc, char **argv)
{
    long fuzz = 0, dump = -1, accepted = 0, refused = 0;
    char label[512];
    int i, failed = 0;
    char tmpl[] = "/tmp/jpeg_test.XXXXXX";
    int tmp;

    signal(SIGALRM, on_alarm);
    signal(SIGABRT, on_abort);

    if (argc > 2 && !strcmp(argv[1], "--fuzz"))
    {
        fuzz = atol(argv[2]);
        argv += 2;
        argc -= 2;
    }
    else if (argc > 2 && !strcmp(argv[1], "--dump"))
    {
        dump = atol(argv[2]);
        argv += 2;
        argc -= 2;
    }
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s [--fuzz N | --dump K] FILE...\n"
                        "  --dump K writes mutation K of FILE to "
                        "mutation.jpg, to reproduce a failure\n", argv[0]);
        return 1;
    }

    tmp = mkstemp(tmpl);
    if (tmp < 0)
    {
        perror("mkstemp");
        return 1;
    }
    unlink(tmpl);

    for (i = 1; i < argc; i++)
    {
        size_t len, n;
        unsigned char *src = slurp(argv[i], &len), *mut;
        bool must_decode = strstr(argv[i], "seeds/") != NULL;
        long k;
        int fd;

        current = argv[i];
        if (!src)
        {
            fprintf(stderr, "FAIL %s: cannot read\n", argv[i]);
            failed++;
            continue;
        }

        if (dump >= 0)
        {
            FILE *f = fopen("mutation.jpg", "wb");
            mut = malloc(len);
            rnd_state = (unsigned long)dump * 2654435761UL + 1;
            n = mutate(src, len, mut);
            if (!f || fwrite(mut, 1, n, f) != n)
                perror("mutation.jpg");
            if (f)
                fclose(f);
            printf("mutation %ld of %s -> mutation.jpg\n", dump, argv[i]);
            free(mut);
            free(src);
            return 0;
        }

        fd = open(argv[i], O_RDONLY);
        if (decode_both(fd))
            printf("  ok   %s\n", argv[i]);
        else if (must_decode)
        {
            printf("  FAIL %s: a good file was refused\n", argv[i]);
            failed++;
        }
        else
            printf("  ok   %s (refused)\n", argv[i]);
        close(fd);

        mut = malloc(len);
        for (k = 0; k < fuzz; k++)
        {
            rnd_state = (unsigned long)k * 2654435761UL + 1;
            n = mutate(src, len, mut);
            snprintf(label, sizeof label,
                     "%s mutation %ld (--dump %ld %s)", argv[i], k, k, argv[i]);
            current = label;
            if (ftruncate(tmp, 0) || pwrite(tmp, mut, n, 0) != (ssize_t)n)
            {
                perror("write");
                return 1;
            }
            if (decode_both(tmp))
                accepted++;
            else
                refused++;
        }
        free(mut);
        free(src);
    }

    if (fuzz)
        printf("\n%ld mutations survived (%ld decoded, %ld refused)\n",
               accepted + refused, accepted, refused);
    printf("%d of %d files failed\n", failed, argc - 1);
    close(tmp);
    return failed ? 1 : 0;
}
