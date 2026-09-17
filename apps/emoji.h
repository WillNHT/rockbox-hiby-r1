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
#ifndef _EMOJI_H_
#define _EMOJI_H_

#include <stdbool.h>
#include "rbpaths.h"

/* Colour emoji for text drawn on the main LCD.
 *
 * The pictures come from one file, EMOJI_PACK, made by
 * tools/emojipack.py. Without it text looks the way it always did, except
 * that variation selectors and zero width joiners no longer show up as
 * boxes. */
#define EMOJI_PACK ROCKBOX_DIR "/emoji/emoji.rbe"

/* Opens the pack (again) and registers it with the font code. Safe to call
 * more than once, e.g. after USB. Returns whether emoji are available. */
bool emoji_init(void);
void emoji_close(void);

#endif /* _EMOJI_H_ */
