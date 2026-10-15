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
#ifndef __LCD_LAYERS_H__
#define __LCD_LAYERS_H__

#include "config.h"

#ifdef HAVE_LCD_LAYERS
#include <stdbool.h>
#include "lcd.h"

/* Retained layers above the Rockbox framebuffer. See lcd-layers.c.
 *
 * The framebuffer every screen draws into is the base. A layer is a
 * screen-sized buffer of its own that only its owner draws into, through a
 * viewport whose buffer is lcd_layer_fb(). lcd_update() and
 * lcd_update_rect() put the layers over the base on the way to the panel
 * and take them off again afterwards, so no screen ever finds an overlay's
 * pixels in the framebuffer and no overlay ever has to restore what was
 * underneath it. Pixels equal to LCD_LAYER_KEY are transparent. */

enum lcd_layer
{
    LCD_LAYER_OVERLAY,  /* the stick's gesture overlay and volume dial */
    LCD_LAYER_COUNT
};

/* Near black rather than black: a key of 0 would make every black pixel an
 * overlay draws disappear, and an antialiased edge blended towards this
 * key comes out as a dark fringe, which is what text on a dark ground
 * looks like anyway. */
#define LCD_LAYER_KEY ((fb_data)0x0001)

/* The buffer to point a viewport at to draw into the layer. */
struct frame_buffer_t *lcd_layer_fb(enum lcd_layer layer);

/* The owner is about to draw this rectangle: it becomes part of what the
 * layer contributes to the panel. */
void lcd_layer_mark(enum lcd_layer layer, int x, int y, int w, int h);

/* Make the layer transparent again. `cleared` is called with each
 * rectangle that had content, so the caller can push those to the panel -
 * there the base now shows through. */
void lcd_layer_clear(enum lcd_layer layer,
                     void (*cleared)(int x, int y, int w, int h));

bool lcd_layer_has_content(enum lcd_layer layer);

/* Put the layers into the framebuffer (true) or take them back out (false)
 * around something that reads the framebuffer as the picture on the panel:
 * a screen dump. lcd_update*() do this themselves. */
void lcd_layers_compose(bool on);

/* Surfaces: retained layers the size of a rectangle, for UI that sits over
 * other UI - a skin viewport declared after one it overlaps.
 *
 * A viewport whose buffer is a surface's fb draws into the surface and
 * never into the framebuffer, so nothing drawn underneath it later can
 * paint over it, and its own clears cannot wipe what is underneath. The
 * surfaces are composed over the framebuffer at present time, in
 * ascending z, under the layers above. A surface pixel equal to
 * LCD_LAYER_KEY is transparent; where it is, the surface's veils - its
 * background - are laid over what is beneath, in the order they were
 * added. A veil is a vertical two-stop gradient at an opacity, or, with
 * `src` set, that full-screen image copied straight through.
 *
 * Every surface pixel also has a coverage, 255 unless an antialiased
 * glyph edge landed on the key: then the pixel holds the ink and its
 * coverage says how much of it, and it is blended over what is beneath
 * at present time rather than against the key's near-black. */
#define LCD_SURFACE_VEILS 8

struct lcd_veil
{
    short x, y, w, h;           /* on the panel */
    fb_data top, bottom;
    unsigned char alpha;        /* 0..255 */
    const fb_data *src;
};

struct lcd_surface
{
    struct frame_buffer_t fb;   /* point a viewport's buffer here */
    const void *owner;          /* NULL while free */
    short x, y, w, h;           /* on the panel */
    short row;                  /* where its rows are in the arena */
    short z;
    short tag;                  /* the owner's group, for put_tag() */
    bool fresh;                 /* nothing has been drawn into it yet */
    unsigned char nveils;
    struct lcd_veil veil[LCD_SURFACE_VEILS];
};

/* The owner's surface, made transparent and fresh if it had to be
 * created. NULL when there is no room: draw the old way then. */
struct lcd_surface *lcd_surface_get(const void *owner, int tag, int x, int y,
                                    int w, int h, int z);
struct lcd_surface *lcd_surface_find(const void *owner);
struct lcd_surface *lcd_surface_from_fb(const struct frame_buffer_t *fb);
void lcd_surface_put(struct lcd_surface *s);
void lcd_surface_put_tag(int tag);
void lcd_surface_put_all(void);
/* For the lcd driver: the coverage of a pixel that is in a surface, or
 * NULL if it is not in one. */
unsigned char *lcd_surface_coverage(const fb_data *px);
/* Back to transparent, veils and all. */
void lcd_surface_clear(struct lcd_surface *s);
void lcd_surface_veil(struct lcd_surface *s, int x, int y, int w, int h,
                      fb_data top, fb_data bottom, unsigned alpha,
                      const fb_data *src);
/* Compose the surfaces into the framebuffer for good and drop them, for
 * something that draws over the screen without knowing they are there.
 * False if there were none. */
bool lcd_surface_flatten(void);

/* The target's own update functions, renamed by the driver. */
void lcd_update_base(void);
void lcd_update_rect_base(int x, int y, int width, int height);

#endif /* HAVE_LCD_LAYERS */
#endif /* __LCD_LAYERS_H__ */
