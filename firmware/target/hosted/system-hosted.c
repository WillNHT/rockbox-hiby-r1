/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2016 Amaury Pouly
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
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#ifdef HAVE_SHUTDOWN_WATCHDOG
#include <pthread.h>
#include <time.h>
#include <sys/reboot.h>
#endif
#include <backtrace.h>

#include "system.h"
#include "mv.h"
#include "font.h"
#include "power.h"
#include "button.h"
#include "button-devinput.h"
#include "backlight-target.h"
#include "lcd.h"
#include "filesystem-hosted.h"
#include "logf.h"
#include "rbpaths.h"
#include "crashlog-linux.h"

const char *handle_special_dirs(const char *dir, unsigned flags,
                                char *buf, const size_t bufsize);

/* to make thread-internal.h happy */
uintptr_t *stackbegin;
uintptr_t *stackend;

/* forward-declare */
bool os_file_exists(const char *ospath);

static void sig_handler(int sig, siginfo_t *siginfo, void *context)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

    /* get context info */
    ucontext_t *uc = (ucontext_t *)context;
    unsigned long pc = uc->uc_mcontext.pc;
    unsigned long sp = uc->uc_mcontext.gregs[29];

    /* The file first: the screen is gone after the reboot, the card is not.
       Only once, for the same reason the backtrace is only taken once. */
    if (!triggered && sig != SIGTERM && sig != SIGPIPE)
        crashlog_write(sig, pc, siginfo->si_addr);

    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();

    lcd_putsf(0, line++, "%s at %08lx", strsignal(sig), pc);

    if(sig == SIGILL || sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) {
        lcd_putsf(0, line++, "address %p", siginfo->si_addr);
    }

    if(!triggered)
    {
        triggered = true;
        rb_backtrace(pc, sp, &line);
    }

#ifdef ROCKBOX_HAS_LOGF
    lcd_putsf(0, line++, "logf:");
    logf_panic_dump(&line);
#endif

    lcd_update();

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

#ifdef HAVE_SHUTDOWN_WATCHDOG
/* A clean shutdown can stall anywhere between the "Shutting Down..."
 * splash and the kernel cutting power: a driver that blocks on close,
 * /sbin/poweroff waiting on an init that never answers, or a poweroff
 * that returns having done nothing. Any of those left the splash lit
 * until the power button was held. The watchdog is a plain OS thread,
 * so it fires whatever the Rockbox threads are stuck in, and it asks
 * the kernel directly. */
#define SHUTDOWN_WATCHDOG_SECS 20

static void *shutdown_watchdog_fn(void *arg)
{
    int cmd = (int)(intptr_t)arg;
    struct timespec ts = { SHUTDOWN_WATCHDOG_SECS, 0 };
    while (nanosleep(&ts, &ts) != 0)
        ;
    sync();
    reboot(cmd);
    return NULL;
}

void shutdown_watchdog_arm(bool reboot_instead)
{
    static bool armed = false;
    if (armed)
        return;

    pthread_t t;
    pthread_attr_t attr;
    intptr_t cmd = reboot_instead ? RB_AUTOBOOT : RB_POWER_OFF;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    armed = pthread_create(&t, &attr, shutdown_watchdog_fn,
                           (void *)cmd) == 0;
    pthread_attr_destroy(&attr);
}

/* init only gets signalled by poweroff/reboot; if it has not acted
 * within a few seconds, go to the kernel ourselves */
static void kernel_reboot_fallback(int cmd)
{
    sleep(5);
    sync();
    reboot(cmd);
}
#endif

void power_off(void)
{
#ifdef HAVE_SHUTDOWN_WATCHDOG
    shutdown_watchdog_arm(false);
#endif
    backlight_hw_on();
    button_close_device();
    sync();
    system("/sbin/poweroff");
#ifdef HAVE_SHUTDOWN_WATCHDOG
    kernel_reboot_fallback(RB_POWER_OFF);
#endif
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_init(void)
{
    int *s;
    /* fake stack, to make thread-internal.h happy */
    stackbegin = stackend = (uintptr_t*)&s;

    /* Resolve the crash log's real path now; a signal handler cannot. */
    char crashlog_buf[256];
    crashlog_init(handle_special_dirs(ROCKBOX_DIR "/crash.log", 0,
                                      crashlog_buf, sizeof(crashlog_buf)));

   /* catch some signals for easier debugging */
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &sig_handler;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void system_reboot(void)
{
#ifdef HAVE_SHUTDOWN_WATCHDOG
    shutdown_watchdog_arm(true);
#endif
    backlight_hw_off();
    system("/sbin/reboot");
#ifdef HAVE_SHUTDOWN_WATCHDOG
    kernel_reboot_fallback(RB_AUTOBOOT);
#endif
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_exception_wait(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    /* wait until button press and release */
#ifdef HAVE_BUTTON_DATA
    int bdata;
#define BDATA &bdata
#else
#define BDATA
#endif
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
}

bool hostfs_removable(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
        return true;
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return true;
#else
        return false; /* internal: always present */
#endif
}

bool hostfs_present(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
#if defined(MULTIDRIVE_DEV)
        return os_file_exists(MULTIDRIVE_DEV);
#else
        return true; // FIXME?
#endif
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return os_file_exists(ROOTDRIVE_DEV);
#else
        return true; /* internal: always present */
#endif
}

#ifdef HAVE_MULTIDRIVE
int volume_drive(int drive)
{
    return drive;
}
#endif /* HAVE_MULTIDRIVE */

#ifdef CONFIG_STORAGE_MULTI
int hostfs_driver_type(int drive)
{
#if (CONFIG_STORAGE & STORAGE_USB)
    return drive > 0 ? STORAGE_USB_NUM : STORAGE_HOSTFS_NUM;
#else
    return drive > 0 ? STORAGE_SD_NUM : STORAGE_HOSTFS_NUM;
#endif
}
#endif /* CONFIG_STORAGE_MULTI */

int hostfs_init(void)
{
    return 0;
}

int hostfs_flush(void)
{
    sync();
    return 0;
}

#ifdef HAVE_HOTSWAP
bool volume_removable(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_removable(volume);
}

bool volume_present(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_present(volume);
}
#endif

int volume_partition(int volume)
{
    (void)volume;
    /* Hosted only implement a single parition per "drive" */
    return 0;
}
