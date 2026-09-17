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

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "system.h"
#include "file.h"
#include "pathfuncs.h"
#include "string-extra.h"
#include "misc.h"
#include "playlist.h"
#include "playlist_cover.h"

/* In the order they are tried. The animated ones are found, so a cover
 * that exists is not silently replaced by the first track's art, but are
 * not drawn yet - see covers.c and issue #19. */
static const char * const cover_exts[] =
{
    ".jpg", ".jpeg", ".png", ".gif", ".webp",
};

static bool is_playlist(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));
}

static bool exists(const char *path)
{
    return file_exists(path);
}

/* The first track named in the playlist, resolved against its folder. */
static bool first_track(const char *playlist, char *buf, size_t size)
{
    char line[MAX_PATH];
    bool found = false;
    int fd = open_utf8(playlist, O_RDONLY);
    if (fd < 0)
        return false;

    while (!found && read_line(fd, line, sizeof(line)) > 0)
    {
        char *s = line;
        char *end;

        /* a UTF-8 byte order mark on the first line */
        if ((unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
            (unsigned char)s[2] == 0xbf)
            s += 3;
        while (isspace((unsigned char)*s))
            s++;
        if (!*s || *s == '#')
            continue;
        end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1]))
            *--end = '\0';
        for (char *p = s; *p; p++)
            if (*p == '\\')
                *p = '/';

        if (*s == '/')
            strmemccpy(buf, s, size);
        else
        {
            /* relative to the playlist's own folder */
            const char *slash = strrchr(playlist, '/');
            size_t dirlen = slash ? (size_t)(slash - playlist) + 1 : 0;
            if (dirlen >= size)
                break;
            memcpy(buf, playlist, dirlen);
            strmemccpy(buf + dirlen, s, size - dirlen);
        }
        found = exists(buf);
    }
    close(fd);
    return found;
}

bool playlist_cover_find(const char *playlist, char *buf, size_t size)
{
    char base[MAX_PATH];
    char *dot;

    if (!playlist || !is_playlist(playlist))
        return false;

    strmemccpy(base, playlist, sizeof(base));
    dot = strrchr(base, '.');
    *dot = '\0';

    for (size_t i = 0; i < ARRAYLEN(cover_exts); i++)
    {
        if (snprintf(buf, size, "%s%s", base, cover_exts[i]) >= (int)size)
            continue;
        if (exists(buf))
            return true;
    }

    return first_track(playlist, buf, size);
}

bool playlist_cover_current(char *buf, size_t size)
{
    if (!playlist_get_name(NULL, buf, size))
        return false;
    return is_playlist(buf);
}
