/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockpocket Stick - the tuning lab.
 *
 * Runs the real engine against the real panel while the normal UI is
 * nowhere near it: nothing here can emit a button, so a misconfigured
 * stick cannot strand you, and the exit key always works. This is where
 * the pixel radii, the detent size and the arming window get tuned before
 * they are frozen as defaults.
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
#include "plugin.h"
#include "stick.h"

/* The engine has no dependencies at all - not even libc - so the lab links
 * its own copy rather than growing the plugin API with a dozen entries that
 * only this plugin would ever call. */
#include "../stick.c"

#ifdef HAVE_LCD_COLOR
#define LAB_INK(r, g, b)  rb->lcd_set_foreground(LCD_RGBPACK(r, g, b))
#else
#define LAB_INK(r, g, b)  do { } while (0)
#endif

#if (CONFIG_KEYPAD == HIBY_R1_PAD)
#define LAB_QUIT     BUTTON_POWER
#define LAB_SECTORS  BUTTON_UP
#define LAB_ZONE     BUTTON_DOWN
#define LAB_DIAL     BUTTON_RIGHT
#define LAB_TRACE    BUTTON_LEFT
#else
#define LAB_QUIT     BUTTON_POWER
#define LAB_SECTORS  BUTTON_NONE
#define LAB_ZONE     BUTTON_NONE
#define LAB_DIAL     BUTTON_NONE
#define LAB_TRACE    BUTTON_NONE
#endif

#define TRACE_FILE   "/stick_trace.log"

static struct stick_config cfg;
static struct stick_state  st;

static int  zone_preset;
static int  fires, taps, drops, cancels;
static int  dial_total;
static int  last_binding = -1;
static int  worst_us, last_us;

static int  trace_fd = -1;
static long trace_t0;

/* ------------------------------------------------------------------ zones */

static const char * const zone_names[] = { "full screen", "bottom half", "plate" };

static void build_zones(void)
{
    stick_config_default(&cfg, LCD_WIDTH, LCD_HEIGHT);

    switch (zone_preset)
    {
    case 1:
        cfg.arm.shape = STICK_SHAPE_BOX;
        cfg.arm.a = 0; cfg.arm.b = LCD_HEIGHT / 2;
        cfg.arm.c = LCD_WIDTH; cfg.arm.d = LCD_HEIGHT / 2;
        cfg.work = cfg.arm;
        break;
    case 2:
        cfg.arm.shape = STICK_SHAPE_CIRCLE;
        cfg.arm.a = LCD_WIDTH / 2; cfg.arm.b = (LCD_HEIGHT * 81) / 100;
        cfg.arm.c = (LCD_WIDTH * 30) / 100;
        cfg.work.shape = STICK_SHAPE_CIRCLE;
        cfg.work.a = cfg.arm.a; cfg.work.b = cfg.arm.b;
        cfg.work.c = (LCD_WIDTH * 46) / 100;
        break;
    default:
        break;
    }

    stick_config_validate(&cfg, LCD_WIDTH, LCD_HEIGHT);
    stick_reset(&st, &cfg);
}

/* ------------------------------------------------------------------ trace */

static void trace_open(void)
{
    if (trace_fd >= 0)
    {
        rb->close(trace_fd);
        trace_fd = -1;
        rb->splash(HZ, "Trace closed");
        return;
    }

    trace_fd = rb->open(TRACE_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (trace_fd < 0)
    {
        rb->splash(HZ, "Cannot open " TRACE_FILE);
        return;
    }

    trace_t0 = *rb->current_tick;
    rb->fdprintf(trace_fd, "# sectors %d\n", cfg.sectors);
    rb->fdprintf(trace_fd, "# rotation %d\n", cfg.rotation);
    rb->fdprintf(trace_fd, "# arm_ms %d\n", cfg.arm_ms);
    rb->fdprintf(trace_fd, "# dial %d\n", cfg.dial);
    rb->splash(HZ, "Tracing to " TRACE_FILE);
}

static void trace_event(const char *what, int x, int y)
{
    if (trace_fd < 0)
        return;
    rb->fdprintf(trace_fd, "%5ld %s %d %d\n",
                 (long)(*rb->current_tick - trace_t0) * (1000 / HZ),
                 what, x, y);
}

/* ----------------------------------------------------------------- drawing */

static void draw_zone(const struct stick_zone *z, const char *label)
{
    if (z->shape == STICK_SHAPE_CIRCLE)
    {
        rb->lcd_drawrect(z->a - z->c, z->b - z->c, z->c * 2, z->c * 2);
        rb->lcd_putsxy(z->a - z->c + 2, z->b - z->c + 2, label);
    }
    else if (z->shape == STICK_SHAPE_BOX)
    {
        rb->lcd_drawrect(z->a, z->b, z->c, z->d);
        rb->lcd_putsxy(z->a + 2, z->b + 2, label);
    }
}

static const char *phase_name(int phase)
{
    switch (phase)
    {
    case STICK_PHASE_IDLE:      return "idle";
    case STICK_PHASE_ABSOLUTE:  return "absolute";
    case STICK_PHASE_ARMING:    return "arming";
    case STICK_PHASE_ARMED:     return "armed";
    case STICK_PHASE_DIAL:      return "dial";
    case STICK_PHASE_CANCELLED: return "CANCELLED";
    case STICK_PHASE_DEAD:      return "dead";
    default:                    return "?";
    }
}

static void draw(void)
{
    int line = 0;
    int h = rb->font_get(FONT_UI)->height;
    char buf[64];

    rb->lcd_clear_display();

    LAB_INK(60, 60, 60);
    draw_zone(&cfg.work, "WORK");
    LAB_INK(80, 180, 130);
    draw_zone(&cfg.arm, "ARM");

    /* The live gesture: origin, current point, and the detent radius the
     * sector only exists outside of. */
    if (stick_phase(&st) != STICK_PHASE_IDLE)
    {
        LAB_INK(220, 200, 90);
        rb->lcd_drawrect(st.ox - cfg.detent_px, st.oy - cfg.detent_px,
                         cfg.detent_px * 2, cfg.detent_px * 2);
        rb->lcd_drawline(st.ox, st.oy, st.x, st.y);
        rb->lcd_fillrect(st.x - 3, st.y - 3, 7, 7);
    }

    LAB_INK(240, 240, 240);

    rb->snprintf(buf, sizeof(buf), "STICK LAB  %s", phase_name(stick_phase(&st)));
    rb->lcd_putsxy(2, line, buf); line += h;

    rb->snprintf(buf, sizeof(buf), "zone %s  N=%d rot=%d",
                 zone_names[zone_preset], cfg.sectors, cfg.rotation);
    rb->lcd_putsxy(2, line, buf); line += h;

    rb->snprintf(buf, sizeof(buf), "sector %d  defl %d/255",
                 stick_sector(&st), stick_deflection(&st));
    rb->lcd_putsxy(2, line, buf); line += h;

    rb->snprintf(buf, sizeof(buf), "binding %s",
                 last_binding >= 0 ? stick_binding_name(last_binding) : "-");
    rb->lcd_putsxy(2, line, buf); line += h;

    rb->snprintf(buf, sizeof(buf), "dial %s  %d detents (%d deg/step)",
                 cfg.dial ? "on" : "off", dial_total, cfg.deg_per_detent);
    rb->lcd_putsxy(2, line, buf); line += h;

    rb->snprintf(buf, sizeof(buf), "fires %d taps %d drop %d cancel %d",
                 fires, taps, drops, cancels);
    rb->lcd_putsxy(2, line, buf); line += h;

    /* Spec 10.6: the per-event cost has to sit far under the 40 ms touch
     * tick. Show it rather than assert it, so it can be watched. */
    rb->snprintf(buf, sizeof(buf), "cost %d us (worst %d) budget 40000",
                 last_us, worst_us);
    rb->lcd_putsxy(2, line, buf); line += h;

    rb->snprintf(buf, sizeof(buf), "trace %s", trace_fd >= 0 ? "ON" : "off");
    rb->lcd_putsxy(2, line, buf); line += h;

    line = LCD_HEIGHT - h * 2;
    rb->lcd_putsxy(2, line, "VOL+ sectors  VOL- zone"); line += h;
    rb->lcd_putsxy(2, line, "NEXT dial  PLAY trace  POWER quit");

    rb->lcd_update();
}

/* ------------------------------------------------------------------- main */

static void feed(int type, int x, int y)
{
    struct stick_output out;
    long t0 = *rb->current_tick;

    stick_process(&st, type, x, y,
                  (long)(*rb->current_tick) * (1000 / HZ), &out);

    /* current_tick has 10 ms resolution, so this only ever shows a coarse
     * figure; it is enough to catch an event path that has gone badly
     * wrong, which is what the budget line is for. */
    last_us = (int)((*rb->current_tick - t0) * (1000000 / HZ));
    if (last_us > worst_us)
        worst_us = last_us;

    switch (out.action)
    {
    case STICK_ACT_FIRE:
        fires++;
        last_binding = out.binding;
        break;
    case STICK_ACT_DIAL:
        dial_total += out.dial_steps;
        break;
    case STICK_ACT_TAP:
        taps++;
        break;
    case STICK_ACT_DROP:
        drops++;
        break;
    default:
        break;
    }

    if (out.cue == STICK_CUE_REJECT)
        cancels++;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    rb->lcd_setfont(FONT_UI);
    /* The lab reads raw touch, so it needs the panel in point mode. The
     * R1 defaults to the 3x3 button grid, in which BUTTON_TOUCHSCREEN
     * never reaches a plugin and the lab would sit there idle. */
    rb->touchscreen_set_mode(TOUCHSCREEN_POINT);
    build_zones();
    draw();

    while (1)
    {
        int button = rb->button_get_w_tmo(HZ / 10);
        short x, y;

        if (button == LAB_QUIT)
            break;

        if (button == LAB_SECTORS)
        {
            cfg.sectors = cfg.sectors >= STICK_MAX_SECTORS ? 1 : cfg.sectors + 1;
            stick_reset(&st, &cfg);
        }
        else if (button == LAB_ZONE)
        {
            zone_preset = (zone_preset + 1) % 3;
            build_zones();
        }
        else if (button == LAB_DIAL)
        {
            cfg.dial = cfg.dial ? STICK_DIAL_OFF : STICK_DIAL_VOLUME;
            dial_total = 0;
            stick_reset(&st, &cfg);
        }
        else if (button == LAB_TRACE)
        {
            trace_open();
        }
        else if (button & BUTTON_TOUCHSCREEN)
        {
            intptr_t data = rb->button_get_data();
            x = (data >> 16) & 0xffff;
            y = data & 0xffff;

            if (button & BUTTON_REL)
            {
                feed(STICK_TOUCH_RELEASE, x, y);
                trace_event("up", x, y);
            }
            else if (button & BUTTON_REPEAT)
            {
                feed(STICK_TOUCH_CONTACT, x, y);
                trace_event("move", x, y);
            }
            else
            {
                feed(STICK_TOUCH_PRESS, x, y);
                trace_event("down", x, y);
            }
        }
        else if (button == BUTTON_NONE)
        {
            /* Quiet tick: this is what reaps a lost release as a cancel. */
            feed(STICK_TOUCH_IDLE, 0, 0);
        }
        else if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
        {
            if (trace_fd >= 0)
                rb->close(trace_fd);
            rb->touchscreen_set_mode(rb->global_settings->touch_mode);
            return PLUGIN_USB_CONNECTED;
        }

        draw();
    }

    if (trace_fd >= 0)
        rb->close(trace_fd);
    rb->touchscreen_set_mode(rb->global_settings->touch_mode);

    return PLUGIN_OK;
}
