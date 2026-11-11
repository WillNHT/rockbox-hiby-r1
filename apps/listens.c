/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Listening history: one line per listen in /.rockbox/listens.tsv.
 *
 * Recaps, charts, rediscovery and the recommenders all ask the same
 * question of the past - what was played, when, for how long, and how it
 * ended - so this records exactly that and nothing else (#28). It is not
 * in the database: the database indexes what is on the card, this is what
 * happened, and it has to outlive a rebuild and files that move.
 *
 * An append with no structure to corrupt, written when a track finishes
 * for any reason. Nothing here reads it back; no query ever runs on the
 * playback path. The station column is the radio's: a dynamic station
 * plays library tracks, so the path alone does not say which it was. Tab separated, so a spreadsheet opens it as it is.
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

#include <time.h>
#include "config.h"
#include "file.h"
#include "rbpaths.h"
#include "appevents.h"
#include "audio.h"
#include "metadata.h"
#include "settings.h"
#include "pradio.h"
#include "audiobooks/audiobooks.h"
#include "listens.h"

/* About 70000 listens; the older ones move aside, once */
#define LISTENS_ROTATE   (8L * 1024 * 1024)

static void listens_track_finish(unsigned short id, void *ev_data)
{
    (void)id;
    struct track_event *te = ev_data;
    struct mp3entry *id3 = te->id3;

    /* Never started: a skip through a playlist, not a listen */
    if (!global_settings.listen_log || !id3 || id3->elapsed == 0)
        return;

    int fd = open(LISTENS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    off_t size = lseek(fd, 0, SEEK_END);
    if (size == 0)
        fdprintf(fd, "path\tstarted\tplayed_ms\tlength_ms\tended\tsource"
                     "\tstation\n");

    const char *source = "music";
    char station[MAX_PATH] = "";
    if (pradio_is_station_track(id3->path))
    {
        source = "radio";
        pradio_station_name(id3->path, station, sizeof(station));
    }
    else if (audiobooks_is_book(id3->path))
        source = "audiobook";

    /* shortcut: a tab or newline in a file name splits the row; FAT names
       cannot hold either, so only a card formatted otherwise would */
    fdprintf(fd, "%s\t%ld\t%lu\t%lu\t%s\t%s\t%s\n", id3->path,
             (long)time(NULL) - (long)(id3->elapsed / 1000),
             id3->elapsed, id3->length,
             (te->flags & TEF_AUTO_SKIP) ? "finished" : "skipped", source,
             station);
    close(fd);

    if (size >= LISTENS_ROTATE)
        rename(LISTENS_FILE, LISTENS_OLD_FILE);
}

void listens_init(void)
{
    add_event(PLAYBACK_EVENT_TRACK_FINISH, listens_track_finish);
}
