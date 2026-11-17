/* The cross toolchain's sysroot is glibc 2.27, the R1's libc is older, and
 * a program that asks for a symbol version the device does not have does
 * not start. build.sh links with --wrap for each symbol below; see
 * tools/rbvideo/glibc_compat.c for the same thing done for the decoder. */

#include <stddef.h>
#include <time.h>

__asm__(".symver old_clock_gettime,clock_gettime@GLIBC_2.2");

int old_clock_gettime(clockid_t, struct timespec *);

int __wrap_clock_gettime(clockid_t c, struct timespec *t)
{
    return old_clock_gettime(c, t);
}

/* glibc 2.25; mbedTLS wipes its keys with it */
void __wrap_explicit_bzero(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}
