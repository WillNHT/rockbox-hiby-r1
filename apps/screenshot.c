/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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
/*
 * Screenshots, as JPEG, into /screenshots.
 *
 * Rockbox has screen_dump(), but it writes a BMP into the root of the card:
 * three quarters of a megabyte a shot, named in local time, that nothing on
 * a phone or a web page wants to open. A baseline JPEG is a few tens of
 * kilobytes and opens everywhere.
 *
 * The encoder is the smallest one that is still a proper JPEG: baseline,
 * no subsampling, the standard tables from Annex K of the spec, and a
 * plain matrix DCT - a screenshot is taken once, so a few tens of
 * milliseconds of arithmetic is not worth a cleverer transform. It reads
 * the framebuffer in place, eight rows at a time, and writes through a
 * small buffer: no allocation.
 */
#include "config.h"

#if LCD_DEPTH == 16

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "lcd.h"
#ifdef HAVE_LCD_LAYERS
#include "lcd-layers.h"
#endif
#include "splash.h"
#include "screenshot.h"

#define SHOT_DIR     "/screenshots"
#define SHOT_QUALITY 92

/* --- the standard tables (ITU T.81 Annex K) --------------------------- */

static const uint8_t zigzag[64] =
{
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t q_lum[64] =
{
    16, 11, 10, 16, 24, 40, 51, 61,  12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,  14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,  24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,  72, 92, 95, 98,112,100,103, 99,
};

static const uint8_t q_chr[64] =
{
    17, 18, 24, 47, 99, 99, 99, 99,  18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,  47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t dc_lum_bits[16] = { 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
static const uint8_t dc_chr_bits[16] = { 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
static const uint8_t dc_vals[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };

static const uint8_t ac_lum_bits[16] = { 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d };
static const uint8_t ac_lum_vals[162] =
{
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa,
};

static const uint8_t ac_chr_bits[16] = { 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
static const uint8_t ac_chr_vals[162] =
{
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa,
};

/* cos((2x + 1) * u * pi / 16) * c(u) / 2, times 4096: a row pass and a
 * column pass with this matrix are the 2-D DCT, 1/4 c(u) c(v) included. */
static const int16_t dct_m[8][8] =
{
    { 1448, 1448, 1448, 1448, 1448, 1448, 1448, 1448 },
    { 2009, 1703, 1138,  400, -400,-1138,-1703,-2009 },
    { 1892,  784, -784,-1892,-1892, -784,  784, 1892 },
    { 1703, -400,-2009,-1138, 1138, 2009,  400,-1703 },
    { 1448,-1448,-1448, 1448, 1448,-1448,-1448, 1448 },
    { 1138,-2009,  400, 1703,-1703, -400, 2009,-1138 },
    {  784,-1892, 1892, -784, -784, 1892,-1892,  784 },
    {  400,-1138, 1703,-2009, 2009,-1703, 1138, -400 },
};

/* --- output ------------------------------------------------------------ */

struct huff { uint16_t code[256]; uint8_t len[256]; };

static struct
{
    int fd;
    bool failed;
    int used;
    uint8_t buf[4096];
    uint32_t bits;
    int nbits;
    uint8_t q[2][64];               /* natural order */
    struct huff dc[2], ac[2];
} enc;

static void flush(void)
{
    if (enc.used && !enc.failed &&
        write(enc.fd, enc.buf, enc.used) != enc.used)
        enc.failed = true;
    enc.used = 0;
}

static void put8(int b)
{
    if (enc.used == (int)sizeof(enc.buf))
        flush();
    enc.buf[enc.used++] = (uint8_t)b;
}

static void put16(int w)
{
    put8(w >> 8);
    put8(w & 0xff);
}

static void put_bits(uint32_t code, int len)
{
    enc.bits = (enc.bits << len) | (code & ((1u << len) - 1));
    enc.nbits += len;
    while (enc.nbits >= 8)
    {
        int b = (enc.bits >> (enc.nbits - 8)) & 0xff;
        put8(b);
        if (b == 0xff)
            put8(0);                /* byte stuffing */
        enc.nbits -= 8;
    }
}

static void build_huff(struct huff *h, const uint8_t *bits, const uint8_t *vals)
{
    int code = 0, k = 0;

    for (int len = 1; len <= 16; len++)
    {
        for (int i = 0; i < bits[len - 1]; i++, k++)
        {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = len;
        }
        code <<= 1;
    }
}

static void put_dht(int class_id, const uint8_t *bits, const uint8_t *vals)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
        n += bits[i];
    put16(0xffc4);
    put16(2 + 1 + 16 + n);
    put8(class_id);
    for (int i = 0; i < 16; i++)
        put8(bits[i]);
    for (int i = 0; i < n; i++)
        put8(vals[i]);
}

static void headers(void)
{
    static const uint8_t jfif[] =
        { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };

    put16(0xffd8);
    put16(0xffe0);
    put16(2 + sizeof(jfif));
    for (unsigned i = 0; i < sizeof(jfif); i++)
        put8(jfif[i]);

    for (int t = 0; t < 2; t++)
    {
        put16(0xffdb);
        put16(2 + 1 + 64);
        put8(t);
        for (int i = 0; i < 64; i++)
            put8(enc.q[t][zigzag[i]]);
    }

    put16(0xffc0);                  /* baseline, 3 components, 1x1 each */
    put16(8 + 3 * 3);
    put8(8);
    put16(LCD_HEIGHT);
    put16(LCD_WIDTH);
    put8(3);
    for (int c = 0; c < 3; c++)
    {
        put8(c + 1);
        put8(0x11);
        put8(c ? 1 : 0);
    }

    put_dht(0x00, dc_lum_bits, dc_vals);
    put_dht(0x10, ac_lum_bits, ac_lum_vals);
    put_dht(0x01, dc_chr_bits, dc_vals);
    put_dht(0x11, ac_chr_bits, ac_chr_vals);

    put16(0xffda);
    put16(6 + 2 * 3);
    put8(3);
    for (int c = 0; c < 3; c++)
    {
        put8(c + 1);
        put8(c ? 0x11 : 0x00);
    }
    put8(0);
    put8(63);
    put8(0);
}

/* The magnitude category of v, and v in the form the bits after it take. */
static int category(int v, uint32_t *bits)
{
    int a = v < 0 ? -v : v, n = 0;
    while (a >> n)
        n++;
    *bits = v < 0 ? (uint32_t)(v - 1) : (uint32_t)v;
    return n;
}

static void block(const int16_t px[64], int t, int *dc_prev)
{
    int tmp[64], out[64];
    uint32_t bits;
    int n;

    for (int y = 0; y < 8; y++)             /* rows */
        for (int u = 0; u < 8; u++)
        {
            int s = 0;
            for (int x = 0; x < 8; x++)
                s += dct_m[u][x] * px[y * 8 + x];
            tmp[y * 8 + u] = (s + 2048) >> 12;
        }
    for (int u = 0; u < 8; u++)             /* columns */
        for (int v = 0; v < 8; v++)
        {
            int s = 0, q = enc.q[t][v * 8 + u];
            for (int y = 0; y < 8; y++)
                s += dct_m[v][y] * tmp[y * 8 + u];
            s = (s + 2048) >> 12;
            out[v * 8 + u] = s < 0 ? -((-s + q / 2) / q) : (s + q / 2) / q;
        }

    n = category(out[0] - *dc_prev, &bits);
    *dc_prev = out[0];
    put_bits(enc.dc[t].code[n], enc.dc[t].len[n]);
    if (n)
        put_bits(bits, n);

    int run = 0;
    for (int i = 1; i < 64; i++)
    {
        int v = out[zigzag[i]];
        if (!v)
        {
            run++;
            continue;
        }
        while (run > 15)
        {
            put_bits(enc.ac[t].code[0xf0], enc.ac[t].len[0xf0]);
            run -= 16;
        }
        n = category(v, &bits);
        put_bits(enc.ac[t].code[(run << 4) | n], enc.ac[t].len[(run << 4) | n]);
        put_bits(bits, n);
        run = 0;
    }
    if (run)
        put_bits(enc.ac[t].code[0], enc.ac[t].len[0]);
}

static void encode(void)
{
    int dc[3] = { 0, 0, 0 };
    int16_t y[64], cb[64], cr[64];
    int scale = SHOT_QUALITY < 50 ? 5000 / SHOT_QUALITY
                                  : 200 - SHOT_QUALITY * 2;

    for (int i = 0; i < 64; i++)
    {
        int a = (q_lum[i] * scale + 50) / 100;
        int b = (q_chr[i] * scale + 50) / 100;
        enc.q[0][i] = (uint8_t)MAX(1, MIN(a, 255));
        enc.q[1][i] = (uint8_t)MAX(1, MIN(b, 255));
    }
    build_huff(&enc.dc[0], dc_lum_bits, dc_vals);
    build_huff(&enc.dc[1], dc_chr_bits, dc_vals);
    build_huff(&enc.ac[0], ac_lum_bits, ac_lum_vals);
    build_huff(&enc.ac[1], ac_chr_bits, ac_chr_vals);

    headers();

    for (int by = 0; by < LCD_HEIGHT; by += 8)
        for (int bx = 0; bx < LCD_WIDTH; bx += 8)
        {
            for (int j = 0; j < 8; j++)
            {
                const fb_data *row = FBADDR(0, MIN(by + j, LCD_HEIGHT - 1));
                for (int i = 0; i < 8; i++)
                {
                    unsigned p = row[MIN(bx + i, LCD_WIDTH - 1)];
                    int r = RGB_UNPACK_RED(p);
                    int g = RGB_UNPACK_GREEN(p);
                    int b = RGB_UNPACK_BLUE(p);
                    /* BT.601, as JFIF wants, level-shifted to centre on 0 */
                    y[j * 8 + i]  = (( 77 * r + 150 * g +  29 * b) >> 8) - 128;
                    cb[j * 8 + i] = ((-43 * r -  85 * g + 128 * b) >> 8);
                    cr[j * 8 + i] = ((128 * r - 107 * g -  21 * b) >> 8);
                }
            }
            block(y, 0, &dc[0]);
            block(cb, 1, &dc[1]);
            block(cr, 1, &dc[2]);
        }

    if (enc.nbits)                          /* pad with ones */
        put_bits(0x7f, 8 - enc.nbits);
    put16(0xffd9);
    flush();
}

/* --- the shot ----------------------------------------------------------- */

void screenshot_take(void)
{
    static const char digits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char path[MAX_PATH], suffix[6];
    time_t now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);
    for (int i = 0; i < 5; i++)
        suffix[i] = digits[rand() % (sizeof(digits) - 1)];
    suffix[5] = '\0';

    mkdir(SHOT_DIR);
    snprintf(path, sizeof(path),
             SHOT_DIR "/%04d-%02d-%02d-%02d-%02d-%02d-utc0-%s.jpg",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, suffix);

    memset(&enc, 0, sizeof(enc));
    enc.fd = creat(path, 0666);
    if (enc.fd < 0)
        return;

#ifdef HAVE_LCD_LAYERS
    /* The panel is the framebuffer with the layers over it. */
    lcd_layers_compose(true);
#endif
    encode();
#ifdef HAVE_LCD_LAYERS
    lcd_layers_compose(false);
#endif

    close(enc.fd);
    if (enc.failed)
        remove(path);
}

#endif /* LCD_DEPTH == 16 */
