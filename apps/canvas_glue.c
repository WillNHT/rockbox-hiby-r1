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

#include <string.h>
#include "config.h"

#ifdef HAVE_COMPOSITOR

#include "lcd.h"
#include "core_alloc.h"
#include "canvas_glue.h"

static int pool_handle = -1;
static size_t pool_size, pool_used;
static size_t scratch_off, scratch_len; /* tail of the pool, reused */

/* ------------------------------------------------------------------ */

bool canvas_glue_init(void)
{
    if (pool_handle >= 0)
        return true;

    pool_size = (size_t)CANVAS_POOL_MAX_KIB * 1024;
    pool_handle = core_alloc(pool_size);

    if (pool_handle < 0)
    {
        /* Documented failure mode: no pool, no compositor, and every
         * caller carries on painting straight at the framebuffer. */
        pool_size = 0;
        return false;
    }

    pool_used = 0;
    scratch_off = scratch_len = 0;

    return true;
}

void canvas_glue_shutdown(void)
{
    if (pool_handle >= 0)
        pool_handle = core_free(pool_handle);

    pool_handle = -1;
    pool_size = pool_used = 0;
    scratch_off = scratch_len = 0;
}

bool canvas_available(void)
{
    return pool_handle >= 0;
}

size_t canvas_pool_used(void) { return pool_used; }
size_t canvas_pool_size(void) { return pool_size; }

canvas_px *canvas_scratch(int w, int h, size_t *px_out)
{
    size_t need = canvas_blur_scratch_px(w, h);
    size_t bytes = ((need * sizeof(canvas_px)) + 3u) & ~3u;
    char *base;

    if (pool_handle < 0)
        return NULL;

    /* The scratch lives at the tail of the pool and is handed out again
     * every time. A blur is a within-frame operation; nothing may hold on
     * to this across one. */
    if (bytes > pool_size || pool_used + bytes > pool_size)
        return NULL;

    scratch_len = bytes;
    scratch_off = pool_size - bytes;

    base = core_get_data(pool_handle);

    if (px_out)
        *px_out = need;

    return (canvas_px *)(base + scratch_off);
}

#endif /* HAVE_COMPOSITOR */
