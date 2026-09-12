/* Stand-in for firmware/export/lcd.h: only what canvas_glue.c touches. */
#ifndef _STUB_LCD_H
#define _STUB_LCD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config.h"

typedef uint16_t fb_data;

#define LCD_NATIVE_STRIDE(s) (s)

struct frame_buffer_t
{
    union {
        void    *data;
        char    *ch_ptr;
        fb_data *fb_ptr;
    };
    void   *(*get_address_fn)(int x, int y);
    ptrdiff_t stride;
    size_t    elems;
};

void lcd_update_rect(int x, int y, int width, int height);

#endif
