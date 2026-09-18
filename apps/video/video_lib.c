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
#include <string.h>
#include "system.h"
#include "file.h"
#include "rbpaths.h"
#include "load_code.h"
#include "string-extra.h"
#include "debug.h"
#include "settings.h"
#include "video_lib.h"

static const struct rbv_api *api;
static bool tried;
static const char *error = "";

const struct rbv_api *video_lib(void)
{
    void *handle;
    const struct rbv_api *a;

    if (tried)
        return api;
    tried = true;

    if (!file_exists(VIDEO_LIB_PATH))
    {
        error = "Video decoder not installed";
        return NULL;
    }
    /* The same loader plugins use: it knows where /.rockbox really is. */
    handle = lc_open(VIDEO_LIB_PATH, NULL, 0);
    if (!handle)
    {
        error = "Video decoder failed to load";
        return NULL;
    }
    a = lc_get_header(handle);
    if (!a || a->version != RBV_API_VERSION)
    {
        DEBUGF("librbvideo: version %d, want %d\n", a ? a->version : -1,
               RBV_API_VERSION);
        error = "Video decoder is the wrong version";
        lc_close(handle);
        return NULL;
    }
    DEBUGF("%s\n", a->build);
    api = a;
    return api;
}

const char *video_os_path(const char *path, char *buf, size_t size)
{
#ifdef SIMULATOR
    if (sim_get_os_path(buf, path, size) < 0)
        return NULL;
    return buf;
#elif defined(APPLICATION)
    return handle_special_dirs(path, 0, buf, size);
#else
    (void)buf; (void)size;
    return path;
#endif
}

const char *video_lib_error(void)
{
    return error;
}

static bool has_ext(const char *path, const char * const exts[], int n)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(dot + 1, exts[i]))
            return true;
    return false;
}

static const char * const video_exts[] =
{
    "mp4", "m4v", "mov", "mkv", "webm", "flv", "avi", "mpg", "mpeg",
    "ts", "m2ts", "wmv", "3gp", "ogv",
};

static const char * const anim_exts[] =
{
    "gif", "webp", "apng",
};

bool video_is_video_file(const char *path)
{
    return has_ext(path, video_exts, ARRAYLEN(video_exts));
}

bool video_has_picture(const char *path)
{
    const struct rbv_api *api;
    struct rbv_info info;
    char osbuf[MAX_PATH];
    const char *ospath;

    if (!global_settings.video_enabled || !video_is_video_file(path))
        return false;
    api = video_lib();
    if (!api)
        return false;
    ospath = video_os_path(path, osbuf, sizeof(osbuf));
    if (!ospath)
        return false;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (api->probe(ospath, &info) < 0)
        return false;
    return info.has_video != 0;
}

bool video_is_sidecar(const char *name)
{
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    const char *dot = strrchr(base, '.');
    const char *prev;
    if (!dot || dot == base || !has_ext(base, video_exts, ARRAYLEN(video_exts)))
        return false;
    /* An album's canvas.mp4 */
    if (dot - base == 6 && !strncasecmp(base, "canvas", 6))
        return true;
    for (prev = dot - 1; prev > base && *prev != '.'; prev--)
        ;
    if (*prev != '.')
        return false;
    return (dot - prev == 6 && !strncasecmp(prev, ".video", 6)) ||
           (dot - prev == 7 && !strncasecmp(prev, ".canvas", 7));
}

bool video_is_image_anim(const char *path)
{
    return has_ext(path, anim_exts, ARRAYLEN(anim_exts));
}
