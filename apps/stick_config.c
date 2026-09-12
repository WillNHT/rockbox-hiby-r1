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
#include "gui/list.h"

#define LOGF_ENABLE
#include "logf.h"

#define STICK_CFG_FILE ROCKBOX_DIR "/stick.cfg"

static const uint8_t preset_4way[STICK_MAX_SECTORS] =
{
    STICK_BIND_SCROLL_UP,     /* up    */
    STICK_BIND_SELECT,        /* right */
    STICK_BIND_SCROLL_DOWN,   /* down  */
    STICK_BIND_BACK,          /* left  */
};

/* The WPS is the one screen that cannot be left with the stick's ordinary
 * table: its keymap spends UP and DOWN on browse and hotkey and puts every
 * way out behind a POWER combination, so a stick-only user was stuck there.
 * These are the four directions plus a hold on each, which is the whole
 * transport without a physical button. */
static const uint8_t wps_bind[STICK_MAX_SECTORS] =
{
    STICK_BIND_MENU,          /* up    */
    STICK_BIND_NEXT,          /* right */
    STICK_BIND_PLAY_PAUSE,    /* down  */
    STICK_BIND_PREV,          /* left  */
};

static const uint8_t wps_hold[STICK_MAX_SECTORS] =
{
    STICK_BIND_QUICKSCREEN,   /* up    - shuffle and repeat */
    STICK_BIND_SEEK_FWD,      /* right */
    STICK_BIND_STOP,          /* down  */
    STICK_BIND_SEEK_BACK,     /* left  */
};

/* Kept because the engine still supports any sector count; nothing in the
 * settings reaches it while we are only designing for four ways. */
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
        /* How long the slow tap has to last before the dial arms, and how
         * far the thumb may wander from the pivot before the pivot follows
         * it. Both here rather than in the settings screen because they are
         * feel, and feel is what a file is for. */
        else if (!strcmp(key, "dialArmMs"))
            cfg->dial_arm_ms = atoi(rest);
        else if (!strcmp(key, "dialMaxPx"))
            cfg->dial_max_px = atoi(rest);
        /* The scroll feel, for tuning on the device without a rebuild.
         * scrollPx is the slow baseline; accelV0 and accelMaxQ8 decide how
         * hard speed shrinks it. accelMaxQ8 256 means no acceleration. */
        else if (!strcmp(key, "scrollPx"))
            cfg->scroll_px = atoi(rest);
        else if (!strcmp(key, "accelV0"))
            cfg->accel_v0 = atoi(rest);
        else if (!strcmp(key, "accelMaxQ8"))
            cfg->accel_max_q8 = atoi(rest);
        /* Sustained scrolling: park the thumb and the list keeps going.
         * sustainMs 0 turns it off. */
        else if (!strcmp(key, "sustainMs"))
            cfg->sustain_ms = atoi(rest);
        else if (!strcmp(key, "sustainEdgePx"))
            cfg->sustain_edge_px = atoi(rest);
        else if (!strcmp(key, "sustainV0"))
            cfg->sustain_v0 = atoi(rest);
        else if (!strcmp(key, "sustainVmax"))
            cfg->sustain_vmax = atoi(rest);
        else if (!strcmp(key, "sustainRampMs"))
            cfg->sustain_ramp_ms = atoi(rest);
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
        else if (!strcmp(key, "holdMs"))
            cfg->hold_ms = atoi(rest);
        else if (!strcmp(key, "bind") || !strcmp(key, "hold"))
        {
            bool is_hold = (key[0] == 'h');
            char *idx = strtok_r(NULL, " \t", &rest);
            char *name = strtok_r(NULL, " \t", &rest);
            int i = idx ? atoi(idx) : -1;
            if (i >= 0 && i < STICK_MAX_SECTORS && name)
            {
                if (is_hold)
                    cfg->hold[i] = parse_binding(name);
                else
                    cfg->bind[i] = parse_binding(name);
            }
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
        /* Always available, and it costs the sectors nothing now: the dial
         * is armed by a slow tap rather than owning every gesture, so the
         * theme setting that used to turn it on has nothing left to
         * decide. Circle after the hold and the volume follows, which is
         * the click wheel the user asked for. */
        return STICK_DIAL_VOLUME;
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
    /* Edge swipes coexist with the stick because they are part of it: a
     * press on the strip never becomes a stick gesture, and a stick gesture
     * never starts on the strip, so there is nothing to arbitrate. */
    base_cfg.edge_px = global_settings.stick_edge_swipe
                           ? STICK_DEF_EDGE_PX : 0;

    /* One scrolling speed for the device, not one per input scheme: the
     * stick reads Rockbox's own Scroll Speed rather than owning a knob. */
    base_cfg.scroll_px =
        stick_scroll_px_for_speed(global_settings.scroll_speed);

    /* And one way of slowing down: the list's kinetic deceleration, so a
     * coast the stick starts winds down exactly like one a swipe starts. */
    base_cfg.decel_a0 =
        global_settings.kinetic_scroll_decel.a0 >> LIST_KINETIC_FRACBITS;
    base_cfg.decel_a1 = global_settings.kinetic_scroll_decel.a1;
    base_cfg.deg_per_detent = global_settings.stick_deg_per_detent;

    preset = (global_settings.stick_preset == STICK_PRESET_8WAY)
                 ? preset_8way : preset_4way;
    for (i = 0; i < STICK_MAX_SECTORS; i++)
        base_cfg.bind[i] = preset[i];
    /* A release with no travel is what a tap that overran the arm window
     * looks like, so the centre has to be the harmless thing the user
     * meant by tapping. playPause resolved to Resume Playback in the
     * browser, which jumps to a full screen on what felt like a tap. */
    base_cfg.centre = STICK_BIND_SELECT;

    if (global_settings.stick_preset == STICK_PRESET_CUSTOM)
    {
        if (!load_config_file(&base_cfg))
            logf("stick: no " STICK_CFG_FILE ", keeping the built-in table");
    }

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
        base_cfg.centre = STICK_BIND_SELECT;
    }

    base_valid = true;
}

void stick_config_invalidate(void)
{
    base_valid = false;
}

/* The only screen whose table differs. Everywhere else the stick emits the
 * four ordinary keys and lets the screen decide what they mean, which is
 * what keeps a remapped keypad working. */
static void apply_wps_table(struct stick_config *cfg)
{
    int i;

    for (i = 0; i < STICK_MAX_SECTORS; i++)
    {
        cfg->bind[i] = wps_bind[i];
        cfg->hold[i] = wps_hold[i];
    }
    cfg->sectors = 4;
    /* A tap on the artwork must not skip the track. */
    cfg->centre = STICK_BIND_NONE;
}

void stick_build_config(struct stick_config *cfg, int context)
{
    if (!base_valid)
        build_base();

    *cfg = base_cfg;
    cfg->dial = dial_for_context(context);

    if ((context & 0xff) == CONTEXT_WPS &&
        global_settings.stick_preset != STICK_PRESET_CUSTOM)
        apply_wps_table(cfg);

    /* The quickscreen and the pitchscreen both bind all four directions to
     * their own settings, so the ordinary back gesture means "change the
     * left-hand one" and there is no way out. The centre tap becomes the
     * way out on both: it emits POWER|LEFT, which those keymaps now
     * cancel and exit on. */
    if ((context & 0xff) == CONTEXT_QUICKSCREEN ||
        (context & 0xff) == CONTEXT_PITCHSCREEN)
        cfg->centre = STICK_BIND_MENU;
}

#endif /* HAVE_TOUCHSCREEN */
