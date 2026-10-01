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
#include "config.h"

struct screen;
struct skin_viewport;
struct gui_wps;

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

/* Lifting: a viewport declared after one it overlaps draws into a
 * compositor surface of its own (lcd-layers.h), so the two keep their
 * order however either of them redraws. The status bar, the WPS and the
 * FM screen lift; each one's surfaces go when it does
 * (skin_layer_leave(), with its enum skinnable_screens). */
#if defined(HAVE_LCD_LAYERS) && !defined(__PCTOOL__)
/* Before a pass: which viewports are lifted. A full pass starts the
 * skin's surfaces again and clears the ground beneath them. */
void skin_layer_begin(struct gui_wps *gwps, bool full);
/* Around one lifted viewport's render. enter() is true if its surface is
 * new, and the viewport has to render in full. */
bool skin_layer_enter(struct gui_wps *gwps, struct skin_viewport *svp,
                      int z);
void skin_layer_exit(struct skin_viewport *svp);
/* A viewport %Vd just turned off: a lifted one gives its surface back,
 * which shows what is beneath; any other is cleared. */
void skin_layer_hide(struct screen *display, struct skin_viewport *svp);
void skin_layer_leave(int skin);
/* Something is about to draw over the screen without knowing about
 * surfaces: bake them into the framebuffer, and draw the old way until
 * the next full pass. */
void skin_layer_flatten(void);
/* The viewport's background colour, while bg_pattern is a surface key. */
unsigned skin_layer_bg(const struct skin_viewport *svp);
/* %Vb while a lifted viewport renders: the colour is its veil's, and
 * bg_pattern stays the key. False if the viewport is not drawing lifted. */
bool skin_layer_set_bg(struct skin_viewport *svp, unsigned colour);
#else
static inline void skin_layer_begin(struct gui_wps *g, bool f)
{ (void)g; (void)f; }
static inline bool skin_layer_enter(struct gui_wps *g, struct skin_viewport *s,
                                    int z)
{ (void)g; (void)s; (void)z; return false; }
static inline void skin_layer_exit(struct skin_viewport *s) { (void)s; }
static inline void skin_layer_hide(struct screen *d, struct skin_viewport *s)
{ skin_layer_clear_viewport(d, s); }
static inline void skin_layer_leave(int skin) { (void)skin; }
static inline void skin_layer_flatten(void) { }
#define skin_layer_bg(svp) ((svp)->vp.bg_pattern)
#define skin_layer_set_bg(svp, colour) false
#endif

#endif /* _SKIN_LAYER_H_ */
