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

/* A crash log that survives the reboot.
 *
 * The on-screen backtrace in system-hosted.c is gone the moment the device
 * restarts, and on a player in a pocket nobody reads it. This writes the
 * same information to a file on the card and fsyncs it.
 *
 * Only libc here, and only calls that are safe inside a signal handler:
 * open/write/fsync/close and backtrace_symbols_fd(). No Rockbox file API
 * (its open() is a macro over a mutex-protected layer), and no settings
 * save - a crash must never write config.cfg. The path is resolved once at
 * startup, while it is still safe to ask where the card is. */

#include <execinfo.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "crashlog-linux.h"

static char crashlog_path[256];

void crashlog_init(const char *ospath)
{
    if (!ospath)
        return;
    strncpy(crashlog_path, ospath, sizeof(crashlog_path) - 1);
    crashlog_path[sizeof(crashlog_path) - 1] = '\0';
}

static void put_str(int fd, const char *s)
{
    ssize_t n = write(fd, s, strlen(s));
    (void)n;
}

static void put_hex(int fd, unsigned long v)
{
    char buf[2 + 2 * sizeof(v) + 1];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    do {
        buf[--i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    } while (v && i > 2);
    buf[--i] = 'x';
    buf[--i] = '0';
    put_str(fd, buf + i);
}

static void put_dec(int fd, long v)
{
    char buf[24];
    int i = sizeof(buf) - 1;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    buf[i] = '\0';
    do {
        buf[--i] = '0' + (u % 10);
        u /= 10;
    } while (u && i > 1);
    if (neg)
        buf[--i] = '-';
    put_str(fd, buf + i);
}

void crashlog_write(int sig, unsigned long pc, void *addr)
{
    if (!crashlog_path[0])
        return;

    /* Append: a crash loop leaves every crash, not only the last one. */
    int fd = open(crashlog_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    put_str(fd, "=== crash: signal ");
    put_dec(fd, sig);
    put_str(fd, " (");
    put_str(fd, strsignal(sig));
    put_str(fd, ") pc ");
    put_hex(fd, pc);
    put_str(fd, " addr ");
    put_hex(fd, (unsigned long)addr);
    put_str(fd, "\nbacktrace:\n");

    void *frames[64];
    int count = backtrace(frames, sizeof(frames) / sizeof(frames[0]));
    backtrace_symbols_fd(frames, count, fd);
    put_str(fd, "\n");

    fsync(fd);
    close(fd);
}
