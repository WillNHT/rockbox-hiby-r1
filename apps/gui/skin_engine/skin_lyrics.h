/***************************************************************************
 * Lyrics in skins: see skin_lyrics.c.
 ****************************************************************************/
#ifndef _SKIN_LYRICS_H_
#define _SKIN_LYRICS_H_

#include <stdbool.h>

struct gui_wps;
struct skin_viewport;

enum skin_lyrics_align
{
    SKIN_LYRICS_LEFT = 0,
    SKIN_LYRICS_CENTER,
    SKIN_LYRICS_RIGHT,
};

/* %yb(x, y, w, h [, inactive colour [, align [, gap]]]) */
struct skin_lyrics
{
    short x, y, w, h;
    short align;            /* enum skin_lyrics_align */
    short gap;              /* extra pixels between lyric lines */
    int inactive;           /* colour of the lines not being sung */
    bool has_inactive;      /* else a mix of the fore- and background */
};

#if defined(HAVE_LYRICS) && !defined(__PCTOOL__)
/* Draws the lyrics block into its viewport, which it clears first: give
 * it a viewport of its own. full is a full-skin refresh; otherwise it
 * draws only when something moved. */
void skin_lyrics_draw(struct gui_wps *gwps, struct skin_viewport *svp,
                      const struct skin_lyrics *ly, bool full);
#endif

#endif
