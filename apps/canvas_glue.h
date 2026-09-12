/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Canvas - the Rockbox side.
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

/* Surfaces, memory and presentation: everything the engine in canvas.c
 * deliberately refuses to know about.
 *
 * The design constraint that shapes this file is that Rockbox screens draw
 * immediately into the main framebuffer and are not going to stop. So the
 * compositor here does not own the screen. It owns *layers over* it: each
 * one keeps the pixels it covered, puts them back before it moves, and
 * pushes only the rectangles that changed. That is enough to kill the
 * class of bug where a moving overlay leaves a trail, without a scene
 * graph, without converting a single screen, and without the plugin API
 * noticing anything at all.
 *
 * Everything degrades: if the pool cannot be allocated, every call here
 * becomes a no-op that reports failure, and the caller keeps its old
 * direct-to-framebuffer path.
 */

#ifndef _CANVAS_GLUE_H
#define _CANVAS_GLUE_H

#include <stdbool.h>
#include "canvas.h"

/* Hard cap on everything this module will ever allocate. Memory on this
 * device is not as free as 64 MB suggests: the build asks for 16 and the
 * audio buffer has most of it. */
#define CANVAS_POOL_MAX_KIB  384

struct canvas_overlay;

/* Allocate the pool. Safe to call more than once. Returns false if the
 * memory was not available, which is not an error - it just means every
 * caller keeps doing what it did before. */
bool canvas_glue_init(void);
void canvas_glue_shutdown(void);
bool canvas_available(void);

/* Bytes currently reserved, and the high-water mark. For the debug menu. */
size_t canvas_pool_used(void);
size_t canvas_pool_size(void);

/* Claim an overlay that can cover up to w x h pixels of screen. Returns
 * NULL if the pool is exhausted or the compositor is unavailable. */
struct canvas_overlay *canvas_overlay_claim(int w, int h);

/* The same, but with no surface of its own - only the backing store. For
 * a caller that wants to keep painting straight at the framebuffer with
 * the lcd_* API (text especially, which has no other way to reach a
 * surface) and only needs the compositor to remember what was underneath.
 * Two fifths of the memory of a full overlay. */
struct canvas_overlay *canvas_overlay_claim_backing(int w, int h);

/* Put back the previous region and save the one about to be painted, in
 * one call. The caller then paints whatever it likes into that rectangle
 * with any API at all; the next capture, or a restore, takes it off again
 * exactly. Returns false if it would not fit. */
bool canvas_overlay_capture(struct canvas_overlay *ov, int x, int y,
                            int w, int h);
void canvas_overlay_release(struct canvas_overlay *ov);

/* The surface the caller draws its overlay into. Origin is the overlay's
 * own top-left, not the screen's. Has an alpha plane. */
struct canvas_surface *canvas_overlay_surface(struct canvas_overlay *ov);

/* Put back what the overlay last covered. Call before the screen under it
 * repaints, and before showing it somewhere else. Returns false if it was
 * not covering anything. */
bool canvas_overlay_restore(struct canvas_overlay *ov);

/* Composite the overlay's surface onto the screen at (x, y), saving what
 * was underneath first so it can be restored. w and h may be smaller than
 * the claimed size. Returns false if it would not fit. */
bool canvas_overlay_show(struct canvas_overlay *ov, int x, int y,
                         int w, int h, unsigned opacity);

/* Forget the saved backdrop without putting it back - for when the screen
 * has repainted underneath us and the saved pixels are stale. */
void canvas_overlay_invalidate(struct canvas_overlay *ov);

/* Push everything restore/show touched since the last present to the
 * panel, as one rectangle. Returns false if there was nothing to push. */
bool canvas_present(void);

/* Scratch big enough to blur a region of this size, out of the pool.
 * Valid until the next call. NULL if the pool cannot spare it. */
canvas_px *canvas_scratch(int w, int h, size_t *px_out);

#endif /* _CANVAS_GLUE_H */
