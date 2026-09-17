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
/*
 * Where each book is up to: one small file of fixed records, separate from
 * the bookmarks so the library screens can rely on it. The records are
 * kept by book id, so a rescan does not lose them.
 */
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "abdb.h"

#define PROGRESS_MAGIC  "ABPR0001"
#define MAX_PROGRESS    AB_MAX_BOOKS

static struct ab_progress entries[MAX_PROGRESS];
static int nentries;
static bool loaded, dirty;
static struct mutex lock;   /* the playback watcher writes too */

void ab_progress_load(void)
{
    if (loaded)
        return;
    mutex_init(&lock);
    loaded = true;
    nentries = 0;

    int fd = open(AB_PROGRESS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    char magic[8];
    uint32_t count;
    if (read(fd, magic, 8) == 8 && !memcmp(magic, PROGRESS_MAGIC, 8) &&
        read(fd, &count, 4) == 4)
    {
        if (count > MAX_PROGRESS)
            count = MAX_PROGRESS;
        ssize_t want = count * sizeof(struct ab_progress);
        if (read(fd, entries, want) == want)
            nentries = count;
    }
    close(fd);
}

struct ab_progress *ab_progress_get(uint32_t id)
{
    for (int i = 0; i < nentries; i++)
        if (entries[i].id == id)
            return &entries[i];
    return NULL;
}

struct ab_progress *ab_progress_touch(uint32_t id)
{
    struct ab_progress *p = ab_progress_get(id);
    if (p)
        return p;

    mutex_lock(&lock);
    if (nentries == MAX_PROGRESS)
    {
        /* Full: drop the entry listened to longest ago. */
        int oldest = 0;
        for (int i = 1; i < nentries; i++)
            if (entries[i].last_played < entries[oldest].last_played)
                oldest = i;
        entries[oldest] = entries[--nentries];
    }
    p = &entries[nentries++];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->started = ab_now();
    dirty = true;
    mutex_unlock(&lock);
    return p;
}

void ab_progress_forget(uint32_t id)
{
    mutex_lock(&lock);
    for (int i = 0; i < nentries; i++)
        if (entries[i].id == id)
        {
            entries[i] = entries[--nentries];
            dirty = true;
            break;
        }
    mutex_unlock(&lock);
}

void ab_progress_dirty(void)
{
    dirty = true;
}

void ab_progress_save(void)
{
    if (!dirty)
        return;

    mutex_lock(&lock);
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", AB_PROGRESS_FILE);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        uint32_t count = nentries;
        ssize_t want = count * sizeof(struct ab_progress);
        bool ok = write(fd, PROGRESS_MAGIC, 8) == 8 &&
                  write(fd, &count, 4) == 4 &&
                  write(fd, entries, want) == want;
        close(fd);
        if (ok)
        {
            remove(AB_PROGRESS_FILE);
            if (rename(tmp, AB_PROGRESS_FILE) == 0)
                dirty = false;
        }
        else
            remove(tmp);
    }
    mutex_unlock(&lock);
}

int ab_progress_last_book(void)
{
    int best = -1;
    uint32_t when = 0;
    for (int i = 0; i < nentries; i++)
    {
        if (entries[i].finished || entries[i].last_played < when)
            continue;
        int b = ab_db_find_id(entries[i].id);
        if (b < 0)
            continue;
        best = b;
        when = entries[i].last_played;
    }
    return best;
}

int ab_book_permille(const struct ab_book *b, const struct ab_progress *p)
{
    if (!p)
        return 0;
    if (p->finished)
        return 1000;
    if (!b->length)
        return 0;
    uint64_t v = (uint64_t)p->position * 1000 / b->length;
    return v > 1000 ? 1000 : (int)v;
}
