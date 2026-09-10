/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - turning settings (and an optional config file) into an
 * engine config. This is spec layer 2: everything here is the user's to
 * change, and nothing here can reach past what layer 1 allows.
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

#include "config.h"

#ifdef HAVE_TOUCHSCREEN

#include <stdlib.h>
#include <string.h>

#include "action.h"
#include "file.h"
#include "lcd.h"
#include "misc.h"
#include "pathfuncs.h"
#include "rbpaths.h"
#include "settings.h"
#include "stick.h"
#include "stick_glue.h"

#define LOGF_ENABLE
#include "logf.h"

#define STICK_CFG_FILE ROCKBOX_DIR "/stick.cfg"

/* In "Both" mode the stick keeps clear of the regions the classic touch
 * scheme owns: the screen edges, where edge swipes start, and the strip at
 * the top holding the status bar and the list header. */
#define BOTH_EDGE_INSET   (LCD_WIDTH / 12)
#define BOTH_TOP_INSET    96

static const uint8_t preset_4way[STICK_MAX_SECTORS] =
{
    STICK_BIND_SCROLL_UP,     /* up    */
    STICK_BIND_SELECT,        /* right */
    STICK_BIND_SCROLL_DOWN,   /* down  */
    STICK_BIND_BACK,          /* left  */
};

static const uint8_t preset_8way[STICK_MAX_SECTORS] =
{
    STICK_BIND_SCROLL_UP,     /* up         */
    STICK_BIND_VOL_UP,        /* up-right   */
    STICK_BIND_SELECT,        /* right      */
    STICK_BIND_NEXT,          /* down-right */
    STICK_BIND_SCROLL_DOWN,   /* down       */
    STICK_BIND_VOL_DOWN,      /* down-left  */
    STICK_BIND_BACK,          /* left       */
    STICK_BIND_PREV,          /* up-left    */
};

/* ------------------------------------------------------------------ zones */

static void set_box(struct stick_zone *z, int l, int t, int w, int h)
{
    z->shape = STICK_SHAPE_BOX;
    z->a = l; z->b = t; z->c = w; z->d = h;
}

static void set_circle(struct stick_zone *z, int cx, int cy, int r)
{
    z->shape = STICK_SHAPE_CIRCLE;
    z->a = cx; z->b = cy; z->c = r; z->d = 0;
}

static void apply_zone_preset(struct stick_config *cfg, int preset)
{
    switch (preset)
    {
    case STICK_ZONE_BOTTOM_HALF:
        set_box(&cfg->arm, 0, LCD_HEIGHT / 2, LCD_WIDTH, LCD_HEIGHT / 2);
        cfg->work = cfg->arm;
        break;

    case STICK_ZONE_PLATE:
        /* Low enough that the whole circle is inside a thumb's reach on a
         * 51 mm wide body, with a work circle wider than the arm circle so
         * a thumb that wanders off the plate cancels rather than
         * scrolling to nowhere - the "wheel homage" arrangement. */
        set_circle(&cfg->arm, LCD_WIDTH / 2, (LCD_HEIGHT * 81) / 100,
                   (LCD_WIDTH * 30) / 100);
        set_circle(&cfg->work, LCD_WIDTH / 2, (LCD_HEIGHT * 81) / 100,
                   (LCD_WIDTH * 46) / 100);
        break;

    case STICK_ZONE_FULLSCREEN:
    default:
        set_box(&cfg->arm, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        cfg->work = cfg->arm;
        break;
    }
}

/* Shrink a box arm zone clear of the regions the classic scheme owns. A
 * circular plate is already well inside them, so it is left alone. */
static void inset_for_both_mode(struct stick_config *cfg)
{
    int l, t, r, b;

    if (cfg->arm.shape != STICK_SHAPE_BOX)
        return;

    l = cfg->arm.a;
    t = cfg->arm.b;
    r = cfg->arm.a + cfg->arm.c;
    b = cfg->arm.b + cfg->arm.d;

    if (l < BOTH_EDGE_INSET)
        l = BOTH_EDGE_INSET;
    if (r > LCD_WIDTH - BOTH_EDGE_INSET)
        r = LCD_WIDTH - BOTH_EDGE_INSET;
    if (t < BOTH_TOP_INSET)
        t = BOTH_TOP_INSET;
    if (b > LCD_HEIGHT - BOTH_EDGE_INSET)
        b = LCD_HEIGHT - BOTH_EDGE_INSET;

    if (r - l < 40 || b - t < 40)
        return;                     /* the inset would leave nothing */

    set_box(&cfg->arm, l, t, r - l, b - t);

    /* Work stays generous: leaving the arm zone should not cancel, only
     * leaving the screen region the gesture belongs to. */
    if (cfg->work.shape == STICK_SHAPE_BOX)
        set_box(&cfg->work, 0, 0, LCD_WIDTH, LCD_HEIGHT);
}

/* ------------------------------------------------------------ config file */

static int parse_binding(const char *s)
{
    int b = stick_binding_from_name(s);
    return b < 0 ? STICK_BIND_NONE : b;
}

static bool parse_zone(struct stick_zone *z, char *args)
{
    char *kind = strtok_r(args, " \t", &args);
    int v[4] = { 0, 0, 0, 0 };
    int i;

    if (!kind)
        return false;

    for (i = 0; i < 4; i++)
    {
        char *tok = strtok_r(NULL, " \t", &args);
        if (!tok)
            break;
        v[i] = atoi(tok);
    }

    if (!strcmp(kind, "box") && i >= 4)
    {
        set_box(z, v[0], v[1], v[2], v[3]);
        return true;
    }
    if (!strcmp(kind, "circle") && i >= 3)
    {
        set_circle(z, v[0], v[1], v[2]);
        return true;
    }
    return false;
}

/* Reads /.rockbox/stick.cfg over the top of cfg. Anything missing keeps the
 * value it already had; anything malformed is skipped and then caught by
 * stick_config_validate(), which falls back rather than refusing to run. */
static bool load_config_file(struct stick_config *cfg)
{
    int fd = open(STICK_CFG_FILE, O_RDONLY);
    char line[128];

    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof(line)) > 0)
    {
        char *rest = line;
        char *key;

        if (line[0] == '#' || line[0] == '\0')
            continue;

        key = strtok_r(rest, " \t", &rest);
        if (!key)
            continue;

        if (!strcmp(key, "sectors"))
            cfg->sectors = atoi(rest);
        else if (!strcmp(key, "rotation"))
            cfg->rotation = atoi(rest);
        else if (!strcmp(key, "armMs"))
            cfg->arm_ms = atoi(rest);
        else if (!strcmp(key, "degPerDetent"))
            cfg->deg_per_detent = atoi(rest);
        else if (!strcmp(key, "detentPx"))
            cfg->detent_px = atoi(rest);
        else if (!strcmp(key, "travelPx"))
            cfg->travel_px = atoi(rest);
        else if (!strcmp(key, "dialMinPx"))
            cfg->dial_min_px = atoi(rest);
        else if (!strcmp(key, "centre") || !strcmp(key, "center"))
            cfg->centre = parse_binding(strtok_r(NULL, " \t", &rest));
        else if (!strcmp(key, "arm"))
            parse_zone(&cfg->arm, rest);
        else if (!strcmp(key, "work"))
            parse_zone(&cfg->work, rest);
        else if (!strcmp(key, "dead"))
        {
            if (cfg->n_dead < STICK_MAX_DEAD &&
                parse_zone(&cfg->dead[cfg->n_dead], rest))
                cfg->n_dead++;
        }
        else if (!strcmp(key, "bind"))
        {
            char *idx = strtok_r(NULL, " \t", &rest);
            char *name = strtok_r(NULL, " \t", &rest);
            int i = idx ? atoi(idx) : -1;
            if (i >= 0 && i < STICK_MAX_SECTORS && name)
                cfg->bind[i] = parse_binding(name);
        }
    }

    close(fd);
    return true;
}

/* ------------------------------------------------------------------ build */

static int dial_for_context(int context)
{
    switch (context & 0xff)
    {
    case CONTEXT_WPS:
        return global_settings.stick_dial_wps ? STICK_DIAL_VOLUME
                                              : STICK_DIAL_OFF;
    case CONTEXT_LIST:
    case CONTEXT_TREE:
    case CONTEXT_MAINMENU:
        return global_settings.stick_dial_lists ? STICK_DIAL_SCROLL
                                                : STICK_DIAL_OFF;
    default:
        return STICK_DIAL_OFF;
    }
}

/* Everything except the dial, which is the only part that varies by
 * screen. Built once and cached: it reads stick.cfg, and that must never
 * happen on the touch path. */
static struct stick_config base_cfg;
static bool base_valid;

static void build_base(void)
{
    const uint8_t *preset;
    int i;

    stick_config_default(&base_cfg, LCD_WIDTH, LCD_HEIGHT);

    apply_zone_preset(&base_cfg, global_settings.stick_zone);

    base_cfg.sectors = global_settings.stick_sectors;
    base_cfg.rotation = global_settings.stick_rotation;
    base_cfg.arm_ms = global_settings.stick_arm_ms;
    base_cfg.deg_per_detent = global_settings.stick_deg_per_detent;

    preset = (global_settings.stick_preset == STICK_PRESET_8WAY)
                 ? preset_8way : preset_4way;
    for (i = 0; i < STICK_MAX_SECTORS; i++)
        base_cfg.bind[i] = preset[i];
    base_cfg.centre = STICK_BIND_PLAY_PAUSE;

    if (global_settings.stick_preset == STICK_PRESET_CUSTOM)
    {
        if (!load_config_file(&base_cfg))
            logf("stick: no " STICK_CFG_FILE ", keeping the built-in table");
    }

    if (global_settings.touch_nav_mode == TOUCH_NAV_BOTH)
        inset_for_both_mode(&base_cfg);

    if (!stick_config_validate(&base_cfg, LCD_WIDTH, LCD_HEIGHT))
        logf("stick: config had out of range values, defaults used");

    /* An armed screen with nothing bound at all is not allowed to ship -
     * spec 10.3. Fall back to the 4-way table rather than leaving the user
     * with a screen that swallows touches and does nothing. */
    if (stick_config_is_inert(&base_cfg))
    {
        logf("stick: table is inert, falling back to the 4-way default");
        for (i = 0; i < STICK_MAX_SECTORS; i++)
            base_cfg.bind[i] = preset_4way[i];
        base_cfg.sectors = 4;
        base_cfg.centre = STICK_BIND_PLAY_PAUSE;
    }

    base_valid = true;
}

void stick_config_invalidate(void)
{
    base_valid = false;
}

void stick_build_config(struct stick_config *cfg, int context)
{
    if (!base_valid)
        build_base();

    *cfg = base_cfg;
    cfg->dial = dial_for_context(context);
}

#endif /* HAVE_TOUCHSCREEN */
