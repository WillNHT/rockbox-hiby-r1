/* Draws the click wheel used by the "Rockpocket iPod" WPS.
 *
 * The skin engine can place bitmaps and fill rectangles, and that is all -
 * there is no circle primitive - so the wheel is baked here instead, at the
 * one size the R1's 480x800 panel needs. Run it when the colours or the
 * geometry change; the output is checked in beside the skin.
 *
 *   gcc -O2 -o gen_clickwheel gen_clickwheel.c -lm && ./gen_clickwheel out.bmp
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 272 px, not 360. The skin now gives the iPod's screen roughly two thirds
 * of the panel and the wheel the remaining third, which is closer to the
 * real thing's proportions than the half and half it started at. The
 * shapes and colours are unchanged - only the scale. */
#define W    272
#define H    272
#define CX   (W / 2)
#define CY   (H / 2)

#define R_OUT   134
#define R_IN     56      /* the centre button */

/* Monolith's background, so the wheel sits on the theme rather than on a
 * grey card. */
static const unsigned char BG[3]     = { 0x0E, 0x0D, 0x0C };   /* BGR */
static const unsigned char WHEEL[3]  = { 0xC4, 0xC9, 0xC9 };
static const unsigned char CENTRE[3] = { 0xAF, 0xB4, 0xB4 };
static const unsigned char INK[3]    = { 0x28, 0x2A, 0x2A };

static unsigned char img[H][W][3];

static void put(int x, int y, const unsigned char c[3])
{
    if (x < 0 || y < 0 || x >= W || y >= H)
        return;
    memcpy(img[y][x], c, 3);
}

/* Anti-aliased only in the crudest sense: the edge pixel is blended by the
 * fractional coverage of the radius. Enough to stop the ring looking like a
 * staircase at this size. */
static void blend(int x, int y, const unsigned char c[3], double a)
{
    int i;
    if (x < 0 || y < 0 || x >= W || y >= H)
        return;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    for (i = 0; i < 3; i++)
        img[y][x][i] = (unsigned char)(img[y][x][i] * (1 - a) + c[i] * a);
}

static void disc(int cx, int cy, int r, const unsigned char c[3])
{
    int x, y;
    for (y = cy - r - 2; y <= cy + r + 2; y++)
        for (x = cx - r - 2; x <= cx + r + 2; x++)
        {
            double d = sqrt((double)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (d <= r - 1)
                put(x, y, c);
            else if (d < r + 1)
                blend(x, y, c, (r + 1 - d) / 2.0);
        }
}

/* A filled triangle, given three points. */
static void tri(int x0, int y0, int x1, int y1, int x2, int y2,
                const unsigned char c[3])
{
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int x, y;

    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++)
        {
            int d0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
            int d1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int d2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
            if ((d0 >= 0 && d1 >= 0 && d2 >= 0) ||
                (d0 <= 0 && d1 <= 0 && d2 <= 0))
                put(x, y, c);
        }
}

static void rect(int x, int y, int w, int h, const unsigned char c[3])
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            put(i, j, c);
}

/* The transport glyphs, at the three o'clock, six o'clock and nine o'clock
 * positions of the wheel, where an iPod puts them. */
static void glyph_next(int cx, int cy)
{
    tri(cx - 10, cy - 10, cx - 10, cy + 10, cx + 1, cy, INK);
    tri(cx + 1,  cy - 10, cx + 1,  cy + 10, cx + 12, cy, INK);
    rect(cx + 12, cy - 10, 3, 21, INK);
}

static void glyph_prev(int cx, int cy)
{
    tri(cx + 10, cy - 10, cx + 10, cy + 10, cx - 1, cy, INK);
    tri(cx - 1,  cy - 10, cx - 1,  cy + 10, cx - 12, cy, INK);
    rect(cx - 15, cy - 10, 3, 21, INK);
}

static void glyph_playpause(int cx, int cy)
{
    tri(cx - 16, cy - 10, cx - 16, cy + 10, cx - 3, cy, INK);
    rect(cx + 4,  cy - 10, 4, 21, INK);
    rect(cx + 12, cy - 10, 4, 21, INK);
}

/* MENU, at twelve o'clock, and baked in like the other three.
 *
 * It was real skin text once, so that it would take the theme's font. That
 * cannot work: skin_display.c redraws every loaded image at the end of
 * *every* viewport it renders (wps_display_images ignores the viewport it
 * is handed), so any viewport declared after the wheel gets the wheel
 * stamped over it, and any viewport declared before it is cleared by the
 * wheel's own. A label sitting on top of a bitmap has nowhere to live. So
 * it is a glyph now, three bars, in the same ink as its neighbours. */
static void glyph_menu(int cx, int cy)
{
    int i;
    for (i = -1; i <= 1; i++)
        rect(cx - 13, cy + i * 7 - 2, 26, 4, INK);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "clickwheel.bmp";
    FILE *f;
    int row_bytes = (W * 3 + 3) & ~3;
    int data_size = row_bytes * H;
    int file_size = 54 + data_size;
    unsigned char hdr[54];
    int x, y;

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            memcpy(img[y][x], BG, 3);

    disc(CX, CY, R_OUT, WHEEL);
    disc(CX, CY, R_IN, CENTRE);

    glyph_next(CX + (R_OUT + R_IN) / 2, CY);
    glyph_prev(CX - (R_OUT + R_IN) / 2, CY);
    glyph_playpause(CX, CY + (R_OUT + R_IN) / 2);
    glyph_menu(CX, CY - (R_OUT + R_IN) / 2);

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xff; hdr[3] = (file_size >> 8) & 0xff;
    hdr[4] = (file_size >> 16) & 0xff; hdr[5] = (file_size >> 24) & 0xff;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = W & 0xff; hdr[19] = (W >> 8) & 0xff;
    hdr[22] = H & 0xff; hdr[23] = (H >> 8) & 0xff;
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = data_size & 0xff; hdr[35] = (data_size >> 8) & 0xff;
    hdr[36] = (data_size >> 16) & 0xff; hdr[37] = (data_size >> 24) & 0xff;

    f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return 1;
    }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = H - 1; y >= 0; y--)   /* BMP rows run bottom to top */
    {
        static unsigned char pad[4];
        fwrite(img[y], 3, W, f);
        fwrite(pad, 1, row_bytes - W * 3, f);
    }
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, W, H);
    return 0;
}
