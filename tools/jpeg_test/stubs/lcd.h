#ifndef _STUB_LCD_H
#define _STUB_LCD_H

#include "config.h"

typedef uint16_t fb_data;
#define FB_DATA_SZ 2

enum { FORMAT_MONO, FORMAT_NATIVE, FORMAT_ANY };

#define FORMAT_DITHER      0x20000000
#define FORMAT_RESIZE      0x08000000
#define FORMAT_KEEP_ASPECT 0x04000000
#define FORMAT_RETURN_SIZE 0x02000000

struct bitmap {
    int width;
    int height;
    int format;
    unsigned char *maskdata;
    int alpha_offset;
    unsigned char *data;
};

#endif
