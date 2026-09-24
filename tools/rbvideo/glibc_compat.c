/* The cross toolchain's sysroot is glibc 2.27, the R1's libc is older, and
 * a library that asks for a symbol version the device does not have fails
 * to dlopen at all ("Video decoder failed to load"). The firmware itself
 * needs nothing past GLIBC_2.3.2, so the decoder is held to the same:
 * build.sh links with --wrap for each symbol below, and each wrapper binds
 * to the oldest version of it. getauxval has no older version; it only
 * feeds FFmpeg's CPU detection, and the R1 build has every optional MIPS
 * extension disabled anyway. */

#define _GNU_SOURCE
#include <glob.h>
#include <sched.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

__asm__(".symver old_powf,powf@GLIBC_2.0");
__asm__(".symver old_log2f,log2f@GLIBC_2.2");
__asm__(".symver old_exp2f,exp2f@GLIBC_2.2");
__asm__(".symver old_glob64,glob64@GLIBC_2.2");
__asm__(".symver old_clock_gettime,clock_gettime@GLIBC_2.2");
__asm__(".symver old_vsscanf,vsscanf@GLIBC_2.0");

float old_powf(float, float);
float old_log2f(float);
float old_exp2f(float);
int old_glob64(const char *, int, int (*)(const char *, int), glob64_t *);
int old_clock_gettime(clockid_t, struct timespec *);
int old_vsscanf(const char *, const char *, va_list);

float __wrap_powf(float x, float y) { return old_powf(x, y); }
float __wrap_log2f(float x) { return old_log2f(x); }
float __wrap_exp2f(float x) { return old_exp2f(x); }
int __wrap_glob64(const char *p, int f, int (*e)(const char *, int), glob64_t *g)
{
    return old_glob64(p, f, e, g);
}
int __wrap_clock_gettime(clockid_t c, struct timespec *t)
{
    return old_clock_gettime(c, t);
}
unsigned long __wrap_getauxval(unsigned long type) { (void)type; return 0; }

/* C99 scanf only differs from the old one on "%a", which FFmpeg never uses. */
int __wrap___isoc99_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = old_vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* No affinity: FFmpeg then counts CPUs with sysconf, which is enough. */
int __wrap_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
    (void)pid; (void)size; (void)set;
    return -1;
}
int __wrap___sched_cpucount(size_t size, const cpu_set_t *set)
{
    (void)size; (void)set;
    return 1;
}

int __wrap___xpg_strerror_r(int err, char *buf, size_t len)
{
    if (len)
    {
        strncpy(buf, strerror(err), len - 1);
        buf[len - 1] = '\0';
    }
    return 0;
}
