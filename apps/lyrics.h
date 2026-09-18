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
#ifndef _LYRICS_H_
#define _LYRICS_H_

#include <stdbool.h>

/* Lyrics of the track that is playing, for the skin engine.
 *
 * Found next to the track (<track>.lrc, then <track>.txt) or inside it
 * (ID3v2 SYLT/USLT, Vorbis comment LYRICS/UNSYNCEDLYRICS in FLAC, Ogg and
 * Opus, MP4 (c)lyr); Settings > Playback > Lyrics picks which is tried
 * first. LRC, including the enhanced word-timed kind (its word times are
 * dropped: a line is what is shown), and plain text. Everything is kept
 * as UTF-8; files without a BOM that are not valid UTF-8 are read in the
 * default codepage. */

enum lyrics_kind
{
    LYRICS_NONE = 0,
    LYRICS_SYNCED,      /* every line has a time */
    LYRICS_PLAIN,       /* no times: the view follows the track's progress */
};

enum lyrics_source_order
{
    LYRICS_FILE_FIRST = 0,
    LYRICS_EMBEDDED_FIRST,
};

struct lyrics_pos
{
    enum lyrics_kind kind;
    int count;          /* lines */
    int current;        /* the line being sung, -1 before the first */
    long line_ms;       /* how long ago the current line started */
    long next_ms;       /* how long the current line lasts, <= 0 unknown */
    unsigned long generation;   /* changes whenever other lyrics load */
};

/* Loads the playing track's lyrics if they are not loaded yet, and says
 * where playback is in them. False when there are none (or lyrics are
 * turned off). */
bool lyrics_position(struct lyrics_pos *pos);

/* Line i (0-based) of what lyrics_position() last loaded, or NULL. */
const char *lyrics_line(int i);

/* Forget what is loaded, so the next lyrics_position() looks again. */
void lyrics_invalidate(void);

#endif /* _LYRICS_H_ */
