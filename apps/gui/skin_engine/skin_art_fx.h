/***************************************************************************
 * Album art effects for skins: see skin_art_fx.c.
 ****************************************************************************/
#ifndef _SKIN_ART_FX_H_
#define _SKIN_ART_FX_H_

#include <stdbool.h>
#include "lcd.h"

struct gui_wps;
struct viewport;
struct wps_data;

#if defined(HAVE_ALBUMART) && defined(HAVE_COMPOSITOR) && !defined(__PCTOOL__)

/* %Cb(x, y, w, h, radius, veil) - the cover, scaled to cover the
 * rectangle, blurred, with `veil` percent of the viewport's background
 * colour laid over it. Not drawn to the panel: composed, with the cover
 * and its reflection, into the LCD backdrop - see skin_art_fx.c. */
bool skin_art_backdrop(struct gui_wps *gwps, struct viewport *vp,
                       int x, int y, int w, int h,
                       int radius, int veil_pct, bool full);

/* %Cm(x, y, w, h, top, bottom) - the bottom of the cover, flipped, fading
 * from alpha `top` at its top edge to `bottom` at its bottom. */
bool skin_art_mirror(struct gui_wps *gwps, struct viewport *vp,
                     int x, int y, int w, int h, int top, int bottom,
                     bool full);

/* %Cr - a record turning at deg_per_sec while playing, in step_deg steps,
 * with the cover as its label. See skin_art_fx.c. */
struct skin_vinyl;
bool skin_art_vinyl(struct gui_wps *gwps, struct viewport *vp,
                    const struct skin_vinyl *v);

/* This skin is being reloaded: forget the backdrop it owned. */
void skin_art_fx_reset(const struct wps_data *data);

/* The screen that owns the backdrop is going away: stop showing it. The
 * next pass of the owning skin shows it again. */
void skin_art_fx_leave(void);

/* Reserve the backdrop buffer. Called when a skin using %Cb is loaded:
 * allocating while rendering would make the audio buffer shrink under a
 * playing track. */
bool skin_art_fx_reserve(void);

/* %Cd drew the cover in this viewport: compose it into the backdrop too. */
void skin_art_note_cover(struct gui_wps *gwps, struct viewport *vp);

/* Before a skin's first viewport: render the blurred cover if the picture
 * changed, and make it the LCD backdrop - or take it away - so every
 * viewport of this pass clears onto the right thing. */
void skin_art_fx_prepare(struct gui_wps *gwps);

/* The buffer the LCD should use as its backdrop when a skin has none of its
 * own, or NULL. */
fb_data *skin_art_backdrop_buffer(void);

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
struct skin_vinyl;
static inline bool skin_art_vinyl(struct gui_wps *g, struct viewport *v,
                                  const struct skin_vinyl *s)
{ (void)g; (void)v; (void)s; return false; }
static inline void skin_art_fx_reset(const struct wps_data *d) { (void)d; }
static inline void skin_art_fx_leave(void) { }
static inline bool skin_art_fx_reserve(void) { return false; }
static inline void skin_art_note_cover(struct gui_wps *g, struct viewport *v)
{ (void)g; (void)v; }
static inline void skin_art_fx_prepare(struct gui_wps *g) { (void)g; }
static inline fb_data *skin_art_backdrop_buffer(void) { return NULL; }
#endif

#endif /* _SKIN_ART_FX_H_ */
