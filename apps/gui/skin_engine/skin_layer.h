/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Layering for the skin engine: translucent viewport clears and blended
 * rectangles. See skin_layer.c for what the layer actually is.
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

#ifndef _SKIN_LAYER_H_
#define _SKIN_LAYER_H_

#include <stdbool.h>

struct screen;
struct skin_viewport;

/* Clear one skin viewport, honouring its %Vt veil.
 *
 * Always call this instead of display->clear_viewport() from the skin
 * engine: with no %Vt on the viewport it is exactly that call, so every
 * existing skin is byte for byte unchanged. */
void skin_layer_clear_viewport(struct screen *display,
                               struct skin_viewport *svp);

/* Fill (or gradient-fill) a rectangle in the current viewport with
 * `alpha` percent opacity over what is already on the screen. Returns
 * false if it could not blend, in which case the caller should do its own
 * opaque fill. */
bool skin_layer_fillrect(struct screen *display, struct viewport *vp,
                         int x, int y, int w, int h,
                         unsigned start_colour, unsigned end_colour,
                         unsigned alpha);

#endif /* _SKIN_LAYER_H_ */
