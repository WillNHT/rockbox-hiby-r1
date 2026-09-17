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
#ifndef _ABDB_H_
#define _ABDB_H_

/* The audiobook library: an index of the books under the audiobooks
 * folder (abdb.c) and what has been listened to of each (abprogress.c).
 * Neither is the music database; tagcache never sees these files. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "rbpaths.h"

#define AB_DB_FILE        ROCKBOX_DIR "/audiobooks.db"
#define AB_PROGRESS_FILE  ROCKBOX_DIR "/audiobooks.progress"

#define AB_MAX_BOOKS      2048
#define AB_MAX_FILES      1024    /* files one book may have */

enum ab_str
{
    AB_S_PATH = 0,   /* folder, or the file of a one-file book; no slash */
    AB_S_TITLE,
    AB_S_AUTHOR,
    AB_S_NARRATOR,
    AB_S_SERIES,
    AB_S_SERIES_NO,  /* as written: "3", "2.5" */
    AB_S_COVER,      /* picture file, or the audio file holding one */
    AB_S_DESC,
    AB_NUM_STR
};

struct ab_book
{
    uint32_t id;         /* crc32 of the path; the progress key */
    uint32_t mtime;      /* of the folder (or file) when scanned */
    uint32_t added;      /* when the scan first saw the book */
    uint32_t length;     /* ms, all files */
    uint32_t lengths;    /* pool offset of nfiles u32 lengths in ms */
    uint16_t nfiles;
    uint8_t  single;     /* the path is the book's only file */
    uint8_t  cover_type; /* embedded picture type; 0 = a picture file */
    uint32_t cover_pos;  /* embedded picture offset and size */
    uint32_t cover_size;
    uint32_t str[AB_NUM_STR]; /* pool offsets */
};

/* --- index ------------------------------------------------------------ */

/* Loads the index from disk if it is not in memory yet. */
bool ab_db_load(void);
int  ab_db_count(void);
/* Pointers stay valid until the next scan. */
const struct ab_book *ab_db_book(int index);
const char *ab_db_str(const struct ab_book *b, enum ab_str s);
const uint32_t *ab_db_lengths(const struct ab_book *b);
int  ab_db_find_id(uint32_t id);
/* The book a file belongs to, and which of its files it is. */
int  ab_db_find_file(const char *path, int *file_index);
/* Whether path is inside the audiobooks folder. */
bool ab_path_in_library(const char *path);

/* Brings the index up to date with the folder. Only books whose folder
 * changed are read again unless full is set. With quiet set nothing is
 * shown unless there is work to do. Returns false when cancelled or when
 * the library could not be read. */
bool ab_db_scan(bool full, bool quiet);

/* The files of a book in play order, as full paths.
 * Returns the count; names stay valid until the next call. */
int  ab_db_list_files(const char *path, bool single, const char ***names);

/* --- progress --------------------------------------------------------- */

struct ab_progress
{
    uint32_t id;
    uint16_t file;        /* file being listened to */
    uint8_t  finished;
    uint8_t  reserved;
    uint32_t elapsed;     /* ms into that file */
    uint32_t position;    /* ms into the book */
    int32_t  speed;       /* timestretch; 0 = never set */
    int32_t  pitch;       /* sound pitch; 0 = never set */
    uint32_t last_played; /* time_t */
    uint32_t started;     /* time_t of the first listen */
};

void ab_progress_load(void);
/* NULL if the book has never been played. */
struct ab_progress *ab_progress_get(uint32_t id);
/* Finds or makes the entry. */
struct ab_progress *ab_progress_touch(uint32_t id);
void ab_progress_forget(uint32_t id);
void ab_progress_dirty(void);
/* Writes the file if something changed. */
void ab_progress_save(void);
/* The most recently played unfinished book, or -1. */
int  ab_progress_last_book(void);

/* 0..1000 */
int  ab_book_permille(const struct ab_book *b, const struct ab_progress *p);

uint32_t ab_now(void);

#endif /* _ABDB_H_ */
