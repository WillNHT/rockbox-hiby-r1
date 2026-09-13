/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * A control channel for driving the simulator headlessly.
 *
 * Tier 2 of the test plan needs to press keys, draw gestures and look at
 * the result. It used to do that with xdotool and an X screen grab, which
 * broke twice over: Xvfb will not start here without root, and a grab off
 * WSLg's compositor returns a stale frame, so a screenshot proved nothing
 * about drawing.
 *
 * Both problems are the same problem - going through X for something that
 * happens inside the process. This does neither. A FIFO takes commands,
 * and they are delivered as *synthetic SDL events*: the simulator's own
 * event loop handles them exactly as it handles a real mouse or key, so
 * there is no second input path to keep honest. The picture comes back
 * through Rockbox's own screen_dump(), which reads the LCD framebuffer
 * rather than the window, so nothing about the X server can lie about it.
 *
 * Development only; SIMULATOR builds, and only when RBSIM_CTL is set.
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

#ifdef SIMULATOR

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <SDL.h>

#include "sim_ctrl.h"
#include "sim-ui-defines.h"

/* The LCD's origin inside the simulator window. Commands speak LCD
 * coordinates - the same ones a .wps file and the stick engine use - so a
 * test never has to know the window skin is there. */
#ifdef UI_LCD_POSX
#define CTL_LCD_X   UI_LCD_POSX
#define CTL_LCD_Y   UI_LCD_POSY
#else
#define CTL_LCD_X   0
#define CTL_LCD_Y   0
#endif

static pthread_t ctl_thread;
static int  touch_x, touch_y;

static void push_mouse(Uint32 type, int x, int y)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));

    if (type == SDL_MOUSEMOTION)
    {
        ev.type = SDL_MOUSEMOTION;
        ev.motion.x = x + CTL_LCD_X;
        ev.motion.y = y + CTL_LCD_Y;
        /* button-sdl.c only treats motion as a touch while button 1 is
         * held, which is what a dragging thumb is. */
        ev.motion.state = SDL_BUTTON(SDL_BUTTON_LEFT);
    }
    else
    {
        ev.type = type;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = (type == SDL_MOUSEBUTTONDOWN) ? SDL_PRESSED
                                                        : SDL_RELEASED;
        ev.button.clicks = 1;
        ev.button.x = x + CTL_LCD_X;
        ev.button.y = y + CTL_LCD_Y;
    }
    SDL_PushEvent(&ev);
}

static void push_key(SDL_Keycode sym, bool down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.sym = sym;
    ev.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    SDL_PushEvent(&ev);
}

/* The five physical keys, by the name the project calls them rather than
 * by the SDL key the simulator happens to map them to. A test that says
 * "power" keeps working if uisimulator/buttonmap/hiby-r1.c is rearranged. */
static SDL_Keycode key_by_name(const char *name)
{
    if (!strcmp(name, "power"))   return SDLK_RETURN;
    if (!strcmp(name, "volup"))   return SDLK_UP;
    if (!strcmp(name, "voldown")) return SDLK_DOWN;
    if (!strcmp(name, "play"))    return SDLK_LEFT;
    if (!strcmp(name, "next"))    return SDLK_RIGHT;
    return SDLK_UNKNOWN;
}

static void ctl_sleep_ms(int ms)
{
    if (ms > 0)
        usleep((useconds_t)ms * 1000);
}

/* One command per line. Unknown words are reported and ignored rather
 * than fatal: a script that outruns the build should say so and carry on,
 * not die halfway through a gesture and leave a thumb pressed. */
static void run_line(char *line)
{
    char verb[32];
    int a = 0, b = 0, n;

    n = sscanf(line, "%31s %d %d", verb, &a, &b);
    if (n < 1 || verb[0] == '#')
        return;

    if (!strcmp(verb, "down") && n >= 3)
    {
        touch_x = a; touch_y = b;
        push_mouse(SDL_MOUSEBUTTONDOWN, a, b);
    }
    else if (!strcmp(verb, "move") && n >= 3)
    {
        touch_x = a; touch_y = b;
        push_mouse(SDL_MOUSEMOTION, a, b);
    }
    else if (!strcmp(verb, "up"))
    {
        /* Coordinates are optional: a release happens where the thumb
         * already is unless the test says otherwise. */
        if (n >= 3) { touch_x = a; touch_y = b; }
        push_mouse(SDL_MOUSEBUTTONUP, touch_x, touch_y);
    }
    else if (!strcmp(verb, "sleep") && n >= 2)
    {
        ctl_sleep_ms(a);
    }
    else if (!strcmp(verb, "dump"))
    {
        push_key(SDLK_F5, true);
        push_key(SDLK_F5, false);
    }
    else if (!strcmp(verb, "key") || !strcmp(verb, "press") ||
             !strcmp(verb, "release"))
    {
        char kname[32], state[16];
        SDL_Keycode sym;
        int kn = sscanf(line, "%31s %31s %15s", verb, kname, state);
        if (kn < 2)
            return;
        sym = key_by_name(kname);
        if (sym == SDLK_UNKNOWN)
        {
            fprintf(stderr, "sim_ctrl: unknown key '%s'\n", kname);
            return;
        }
        if (!strcmp(verb, "press"))
            push_key(sym, true);
        else if (!strcmp(verb, "release"))
            push_key(sym, false);
        else if (kn >= 3 && !strcmp(state, "down"))
            push_key(sym, true);
        else if (kn >= 3 && !strcmp(state, "up"))
            push_key(sym, false);
        else
        {
            /* A bare "key power" is a tap: long enough for the action
             * layer to see a press and a release as separate events. */
            push_key(sym, true);
            ctl_sleep_ms(60);
            push_key(sym, false);
        }
    }
    else if (!strcmp(verb, "quit"))
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_QUIT;
        SDL_PushEvent(&ev);
    }
    else
    {
        fprintf(stderr, "sim_ctrl: unknown command '%s'\n", verb);
    }
    fflush(stderr);
}

static void *ctl_main(void *arg)
{
    const char *path = (const char *)arg;

    for (;;)
    {
        char buf[512];
        FILE *f;

        /* Blocks until a writer opens the other end, and returns EOF when
         * the writer closes. Reopening rather than exiting is what lets a
         * test run several scripts against one simulator instance. */
        f = fopen(path, "r");
        if (!f)
        {
            fprintf(stderr, "sim_ctrl: cannot open %s: %s\n",
                    path, strerror(errno));
            return NULL;
        }

        while (fgets(buf, sizeof(buf), f))
        {
            char *nl = strpbrk(buf, "\r\n");
            if (nl)
                *nl = 0;
            run_line(buf);
        }
        fclose(f);
    }
    return NULL;
}

void sim_ctrl_init(void)
{
    static char path[256];
    const char *env = getenv("RBSIM_CTL");

    if (!env || !*env)
        return;

    snprintf(path, sizeof(path), "%s", env);

    if (mkfifo(path, 0666) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "sim_ctrl: mkfifo %s: %s\n", path, strerror(errno));
        return;
    }

    if (pthread_create(&ctl_thread, NULL, ctl_main, path) != 0)
    {
        fprintf(stderr, "sim_ctrl: cannot start control thread\n");
        return;
    }
    pthread_detach(ctl_thread);
    fprintf(stderr, "sim_ctrl: listening on %s\n", path);
    fflush(stderr);
}

#endif /* SIMULATOR */
