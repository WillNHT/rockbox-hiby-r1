#ifndef _STUB_SYSTEM_H
#define _STUB_SYSTEM_H

#include <stdint.h>
#include <sys/types.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define BIT_N(n) (1U << (n))

/* firmware/export/system.h */
#define ALIGN_DOWN(n, a)     ((typeof(n))((uintptr_t)(n)/(a)*(a)))
#define ALIGN_UP(n, a)       ALIGN_DOWN((n)+((a)-1),a)
#define ALIGN_BUFFER(ptr, size, align) \
({                                           \
    size_t    __sz = (size);                 \
    size_t   __ali = (align);                \
    uintptr_t __a1 = (uintptr_t)(ptr);       \
    uintptr_t __a2 = __a1 + __sz;            \
    __a1 = ALIGN_UP(__a1, __ali);            \
    __a2 = ALIGN_DOWN(__a2, __ali);          \
    (ptr)  = (typeof (ptr))__a1;             \
    (size) = __a2 > __a1 ?  __a2 - __a1 : 0; \
})

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

off_t filesize(int fd);
void yield(void);

#endif
