/* Stand-in for firmware/include/core_alloc.h. The test implementation
 * deliberately moves the block on every allocation, because buflib may:
 * anything in canvas_glue.c that cached a pointer would fail here. */
#ifndef _STUB_CORE_ALLOC_H
#define _STUB_CORE_ALLOC_H

#include <stddef.h>

int core_alloc(size_t size);
int core_free(int handle);
void *core_get_data(int handle);

#endif
