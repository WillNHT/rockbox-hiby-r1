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

/* Memory for the engine in canvas.c: a pool, and the scratch a blur needs
 * out of it. The engine itself deliberately knows nothing about either.
 *
 * Everything degrades: if the pool cannot be allocated, every call here
 * becomes a no-op that reports failure.
 */

#ifndef _CANVAS_GLUE_H
#define _CANVAS_GLUE_H

#include <stdbool.h>
#include "canvas.h"

/* Hard cap on everything this module will ever allocate. Memory on this
 * device is not as free as 64 MB suggests: the build asks for 16 and the
 * audio buffer has most of it. */
#define CANVAS_POOL_MAX_KIB  768

/* Allocate the pool. Safe to call more than once. Returns false if the
 * memory was not available, which is not an error - it just means every
 * caller keeps doing what it did before. */
bool canvas_glue_init(void);
void canvas_glue_shutdown(void);
bool canvas_available(void);

/* Bytes currently reserved, and the high-water mark. For the debug menu. */
size_t canvas_pool_used(void);
size_t canvas_pool_size(void);

/* Scratch big enough to blur a region of this size, out of the pool.
 * Valid until the next call. NULL if the pool cannot spare it. */
canvas_px *canvas_scratch(int w, int h, size_t *px_out);

#endif /* _CANVAS_GLUE_H */
