/***************************************************************************
 * Album art effects for skins: see skin_art_fx.c.
 ****************************************************************************/
#ifndef _SKIN_ART_FX_H_
#define _SKIN_ART_FX_H_

#include <stdbool.h>

struct gui_wps;
struct viewport;

#if defined(HAVE_ALBUMART) && defined(HAVE_COMPOSITOR) && !defined(__PCTOOL__)

/* %Cb(x, y, w, h, radius, veil) - the cover, scaled to cover the
 * rectangle, blurred, with `veil` percent of the viewport's background
 * colour laid over it. Writes to the viewport's framebuffer, which inside
 * a %VB viewport is the backdrop buffer - the only surface that survives
 * later viewports clearing themselves. */
bool skin_art_backdrop(struct gui_wps *gwps, struct viewport *vp,
                       int x, int y, int w, int h,
                       int radius, int veil_pct, bool full);

/* %Cm(x, y, w, h, top, bottom) - the bottom of the cover, flipped, fading
 * from alpha `top` at its top edge to `bottom` at its bottom. */
bool skin_art_mirror(struct gui_wps *gwps, struct viewport *vp,
                     int x, int y, int w, int h, int top, int bottom,
                     bool full);

/* Forget which picture is on the backdrop - on a skin change, say. */
void skin_art_fx_reset(void);

#else
static inline bool skin_art_backdrop(struct gui_wps *g, struct viewport *v,
                                     int x, int y, int w, int h,
                                     int radius, int veil_pct, bool full)
{ (void)g;(void)v;(void)x;(void)y;(void)w;(void)h;(void)radius;(void)veil_pct;
  (void)full; return false; }
static inline bool skin_art_mirror(struct gui_wps *g, struct viewport *v,
                                   int x, int y, int w, int h,
                                   int top, int bottom, bool full)
{ (void)g;(void)v;(void)x;(void)y;(void)w;(void)h;(void)top;(void)bottom;
  (void)full; return false; }
static inline void skin_art_fx_reset(void) { }
#endif

#endif /* _SKIN_ART_FX_H_ */
