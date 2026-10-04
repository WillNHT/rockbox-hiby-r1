/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/_ \ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026
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
#include "lang.h"
#include "settings.h"
#include "hiby_bluetooth.h"

#ifdef HAVE_HIBY_BLUETOOTH

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel.h"
#include "audio.h"
#include "action.h"
#include "button.h"
#include "menu.h"
#include "misc.h"
#include "splash.h"
#include "gui/list.h"
#include "yesno.h"
#include "sound.h"
#include "talk.h"
#include "pcm.h"
#ifndef SIMULATOR
#include "button-devinput.h"
#endif

/* Pipes and processes here are the OS's, not Rockbox's files */
#undef open
#undef read
#undef write
#undef close

#define S(id) ((const char *)str(id))

/* Bluetooth is BlueZ (bluetoothctl) and bluealsa (bluealsa-cli), both only
 * reachable through their command line tools, and every one of them can
 * take seconds. Rockbox's threads all share one OS thread here, so they run
 * on a pthread of their own - the worker - and the UI never waits on them:
 * it posts a job and redraws from the state the worker publishes.
 *
 * The worker never calls into Rockbox. What has to happen on Rockbox's side
 * (switching the PCM, the headset's keys, its volume) is picked up by the
 * button tick through bt_pending_event() and done in bt_sync() on whatever
 * thread is reading events, the way a headphone plug is. */

#define BT_MAX_DEVICES   24
#define BT_NAME_LEN      64
#define BT_MAX_CODECS    8
#define BT_CODEC_LEN     16
#define BT_LOCAL_PLAYBACK_DEVICE "plughw:0,0"
#define BT_REMOTE_INPUT_IDX 4   /* the one poll slot for a dynamic node */
#define BT_FIRST_DYNAMIC_NODE 4 /* /dev/input/event4 on: added at run time */
#define BT_HCI           "/sys/class/bluetooth/hci0"
#define BT_DIR           PIVOT_ROOT ROCKBOX_DIR
#define BT_BOOT_FILE     BT_DIR "/rb_bt_on.txt"   /* the bootloader reads it too */
#define BT_LAST_FILE     BT_DIR "/rb_bt_last.txt"
#define BT_LOG_FILE      "/data/mnt/sd_0/rockbox-bt-debug.log"
#define BT_LOG_MAX       (512*1024)

/* hiby/pcm-alsa-hiby.c and pcm-alsa.c */
int pcm_alsa_switch_playback_device(const char *device);
void hiby_pcm_set_bt_mac(const char *mac);
const char *hiby_pcm_get_bt_mac(void);
/* hiby/hibylinux_codec.c */
int hiby_bt_mixer_fds(struct pollfd *pfd, int max);
int hiby_bt_mixer_remote_volume(struct pollfd *pfd, int n);
void hiby_bt_mixer_close(void);

struct bt_device
{
    char mac[18];
    char name[BT_NAME_LEN];
    bool paired;
    signed char battery;    /* percent, -1 if the headset does not say */
};

enum { BT_OFF, BT_STARTING, BT_ON };

enum bt_job
{
    BT_JOB_NONE, BT_JOB_ON, BT_JOB_OFF, BT_JOB_CONNECT, BT_JOB_DISCONNECT,
    BT_JOB_FORGET, BT_JOB_FORGET_ALL, BT_JOB_CODEC,
};

/* Everything the worker shares with the UI, under bt_mtx. The worker only
 * holds it to copy, never across a command. */
static struct
{
    struct bt_device dev[BT_MAX_DEVICES];
    int count;
    char link[18];          /* the headset whose A2DP sink is up */
    char codec[BT_CODEC_LEN];
    char codecs[BT_MAX_CODECS][BT_CODEC_LEN];
    int ncodecs;
    int rate;
    char busy[18];          /* being paired or connected */
    char last[18];          /* the last headset that played */
    int msg;                /* lang id the screen shows once, 0 if none */
    char passkey[8];        /* waiting for the user to confirm it */
    int answer;             /* to the passkey: 1 yes, 0 no, -1 not yet */
    bool screen;            /* the Bluetooth screen is open: discover */
    bool scanning;
    enum bt_job job;        /* UI -> worker, one slot: the latest wins */
    char job_mac[18];
    char job_arg[BT_CODEC_LEN];
} bt;

static pthread_mutex_t bt_mtx = PTHREAD_MUTEX_INITIALIZER;
static int bt_wake[2] = { -1, -1 };

/* Read without the lock, from the tick too */
static volatile int bt_power = BT_OFF;
static volatile int bt_gen;         /* bumped on any change: screens redraw */
static volatile int bt_link_gen;    /* the link changed, or wants routing again */
static volatile bool bt_linked;
static volatile int bt_input_node = -1;
static volatile int bt_remote_vol = INT_MIN;

/* --------------------------------------------------------------- logging */

/* Only when the log file is already there: create an empty
 * rockbox-bt-debug.log on the card to switch it on */
void hiby_debug_log(const char *format, ...)
{
    va_list ap;
    FILE *fp = fopen(BT_LOG_FILE, "r+");

    if (!fp)
        return;
    fseek(fp, 0, SEEK_END);
    if (ftell(fp) > BT_LOG_MAX)
        fp = freopen(BT_LOG_FILE, "w", fp);
    if (!fp)
        return;
    va_start(ap, format);
    vfprintf(fp, format, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* ------------------------------------------------- the commands, or a fake */

#ifdef SIMULATOR
/* No BlueZ on a desktop: a few made up devices, so the screens can be
 * looked at. One headset is connected from the start. */
static struct { const char *mac, *name; bool paired; } sim_dev[] = {
    { "00:1B:66:A1:B2:C3", "Sony WH-1000XM4",  true  },
    { "7C:2E:BD:10:20:30", "Galaxy Buds2 Pro", true  },
    { "F8:DF:15:40:50:60", "JBL Flip 5",       false },
    { "34:88:5D:70:80:90", "Car Audio",        false },
};
static char sim_link[18] = "00:1B:66:A1:B2:C3";
static char sim_out[512];
/* POSIX 2008; the simulator's feature macros leave it undeclared */
FILE *fmemopen(void *buf, size_t size, const char *mode);

static FILE *bt_popen(const char *cmd)
{
    const char *c = strchr(cmd, ':');
    char mac[18] = "";
    size_t n = 0;
    unsigned i;

    if (c && c - cmd >= 2)
        snprintf(mac, sizeof(mac), "%.17s", c - 2);
    sim_out[0] = '\0';
    if (strstr(cmd, "paired-devices") || strstr(cmd, "devices"))
    {
        bool paired_only = strstr(cmd, "aired") != NULL;
        for (i = 0; i < sizeof(sim_dev)/sizeof(sim_dev[0]); i++)
            if (sim_dev[i].paired || !paired_only)
                n += snprintf(sim_out + n, sizeof(sim_out) - n, "Device %s %s\n",
                              sim_dev[i].mac, sim_dev[i].name);
    }
    else if (strstr(cmd, "list-pcms") && sim_link[0])
    {
        char u[18];
        for (i = 0; i < 17; i++)
            u[i] = sim_link[i] == ':' ? '_' : sim_link[i];
        u[17] = '\0';
        snprintf(sim_out, sizeof(sim_out),
                 "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink\n", u);
    }
    else if (strstr(cmd, "bluealsa-cli info"))
        strcpy(sim_out, "Sampling: 96000 Hz\nAvailable codecs: SBC AAC LDAC\n"
                        "Selected codec: LDAC\n");
    else if (strstr(cmd, "bluetoothctl info") && !strcmp(mac, sim_link))
        strcpy(sim_out, "\tBattery Percentage: 0x50 (80)\n");
    else if (strstr(cmd, "power"))
        strcpy(sim_out, "Changing power succeeded\n");
    else if (strstr(cmd, "bluetoothctl connect"))
    {
        snprintf(sim_link, sizeof(sim_link), "%s", mac);
        strcpy(sim_out, "Connection successful\n");
    }
    else if (strstr(cmd, "bluetoothctl disconnect"))
        sim_link[0] = '\0';
    else if (strstr(cmd, "bluetoothctl pair") || strstr(cmd, "bluetoothctl remove"))
    {
        for (i = 0; i < sizeof(sim_dev)/sizeof(sim_dev[0]); i++)
            if (!strcmp(sim_dev[i].mac, mac))
                sim_dev[i].paired = strstr(cmd, "pair") != NULL;
        strcpy(sim_out, "Pairing successful\nhas been removed\n");
    }
    else if (strstr(cmd, "bluealsa-cli codec"))
        strcpy(sim_out, "ok\n");
    return fmemopen(sim_out, strlen(sim_out) + 1, "r");
}
#define bt_pclose fclose

static int bt_acl_count(void)
{
    return sim_link[0] ? 1 : 0;
}

#define bt_find_input_node(name) ((void)(name), -1)
#define bt_scan_open() fopen("/dev/null", "w")
#define bt_scan_close fclose
#else
#define bt_popen(cmd) popen(cmd, "r")
#define bt_pclose pclose
#define bt_scan_open() popen("bluetoothctl >/dev/null 2>&1", "w")
#define bt_scan_close pclose

/* One /sys/class/bluetooth/hci0:<handle> per link, audio or not: no fork.
 * Listed by hand: glob() needs glibc 2.27, newer than the player's, and
 * opendir() here is Rockbox's own */
static int bt_acl_count(void)
{
    char buf[1024];
    int fd = open("/sys/class/bluetooth", O_RDONLY);
    int n = 0, len, i;

    if (fd < 0)
        return 0;
    /* struct linux_dirent64: reclen at 16, name at 19 */
    while ((len = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0)
        for (i = 0; i < len; i += *(unsigned short *)(buf + i + 16))
            if (!strncmp(buf + i + 19, "hci0:", 5))
                n++;
    close(fd);
    return n;
}
#endif

/* Run cmd; true if its output has want in it (any output, for NULL) */
static bool bt_cmd(const char *cmd, const char *want)
{
    char line[256];
    bool found = false;
    FILE *fp = bt_popen(cmd);

    if (!fp)
        return false;
    while (fgets(line, sizeof(line), fp))
    {
        hiby_debug_log("  %s", line);
        if (!found && (want ? strstr(line, want) != NULL : line[0] != '\0'))
            found = true;
    }
    bt_pclose(fp);
    hiby_debug_log("bt: %s -> %d", cmd, found);
    return found;
}

static bool bt_ctl(const char *verb, const char *mac, const char *want)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "bluetoothctl %s %s 2>&1", verb, mac ? mac : "");
    return bt_cmd(cmd, want);
}

static void bt_pcm_path(const char *mac, char *path, size_t len)
{
    char u[18];
    int i;
    for (i = 0; i < 17 && mac[i]; i++)
        u[i] = mac[i] == ':' ? '_' : mac[i];
    u[i] = '\0';
    snprintf(path, len, "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", u);
}

/* The headset bluealsa has an A2DP sink for, "" if none */
static void bt_sink_mac(char *mac)
{
    char line[256];
    FILE *fp = bt_popen("bluealsa-cli list-pcms 2>/dev/null");
    char *p;

    mac[0] = '\0';
    while (fp && fgets(line, sizeof(line), fp))
    {
        if (!strstr(line, "/a2dpsrc/sink") || !(p = strstr(line, "/dev_"))
            || strlen(p) < 5 + 17)
            continue;
        for (int i = 0; i < 17; i++)
            mac[i] = p[5 + i] == '_' ? ':' : toupper((unsigned char)p[5 + i]);
        mac[17] = '\0';
        break;
    }
    if (fp)
        bt_pclose(fp);
}

static bool bt_wait_sink(const char *mac, int ms)
{
    char now[18];
    for (; ms > 0; ms -= 250)
    {
        bt_sink_mac(now);
        if (!strcmp(now, mac))
            return true;
        usleep(250000);
    }
    return false;
}

/* ---------------------------------------------------------- worker state */

static void bt_changed(void)
{
    bt_gen++;
}

static void bt_set_msg(int lang_id)
{
    pthread_mutex_lock(&bt_mtx);
    bt.msg = lang_id;
    pthread_mutex_unlock(&bt_mtx);
    bt_changed();
}

static struct bt_device *bt_find(struct bt_device *devs, int count, const char *mac)
{
    for (int i = 0; i < count; i++)
        if (!strcasecmp(devs[i].mac, mac))
            return &devs[i];
    return NULL;
}

static void bt_save_last(const char *mac)
{
    FILE *fp;

    pthread_mutex_lock(&bt_mtx);
    snprintf(bt.last, sizeof(bt.last), "%s", mac);
    pthread_mutex_unlock(&bt_mtx);
    if (!mac[0])
    {
        unlink(BT_LAST_FILE);
        return;
    }
    fp = fopen(BT_LAST_FILE, "w");
    if (fp)
    {
        fputs(mac, fp);
        fclose(fp);
    }
}

static void bt_load_last(void)
{
    FILE *fp = fopen(BT_LAST_FILE, "r");
    char mac[18] = "";

    if (fp)
    {
        if (fgets(mac, sizeof(mac), fp) && strlen(mac) != 17)
            mac[0] = '\0';
        fclose(fp);
    }
    snprintf(bt.last, sizeof(bt.last), "%s", mac);
}

/* Add "Device <mac> <name>" lines from cmd to devs */
static int bt_parse_devices(const char *cmd, struct bt_device *devs, int count,
                            bool paired)
{
    char line[256];
    FILE *fp = bt_popen(cmd);

    while (fp && fgets(line, sizeof(line), fp) && count < BT_MAX_DEVICES)
    {
        char *p = strstr(line, "Device ");
        struct bt_device *d;

        char *name;

        /* "[CHG] Device <mac> RSSI: ..." and the like are events */
        if (!p || strstr(line, "[CHG]") || strstr(line, "[DEL]")
            || strlen(p) < 7 + 17 || p[7 + 2] != ':')
            continue;
        p += 7;
        name = p[17] ? p + 18 : p + 17;
        p[17] = '\0';
        name[strcspn(name, "\r\n")] = '\0';
        d = bt_find(devs, count, p);
        if (!d)
        {
            /* unnamed ones (BLE beacons mostly) are named after their MAC */
            if (!paired && (!*name || (strlen(name) == 17 && name[2] == '-')))
                continue;
            d = &devs[count++];
            snprintf(d->mac, sizeof(d->mac), "%s", p);
            snprintf(d->name, sizeof(d->name), "%s", *name ? name : p);
            d->paired = false;
            d->battery = -1;
        }
        d->paired |= paired;
    }
    if (fp)
        bt_pclose(fp);
    return count;
}

static int bt_device_cmp(const void *a, const void *b)
{
    const struct bt_device *da = a, *db = b;
    if (da->paired != db->paired)
        return da->paired ? -1 : 1;
    return strcasecmp(da->name, db->name);
}

/* Paired ones (BlueZ 5.65 renamed the command), then what discovery saw */
static void bt_refresh_devices(void)
{
    static struct bt_device devs[BT_MAX_DEVICES];
    int count;

    memset(devs, 0, sizeof(devs));  /* compared whole, padding too */
    count = bt_parse_devices("out=$(bluetoothctl paired-devices 2>&1); case \"$out\" in "
                             "*nvalid*) bluetoothctl devices Paired 2>/dev/null;; "
                             "*) echo \"$out\";; esac", devs, 0, true);
    count = bt_parse_devices("bluetoothctl devices 2>/dev/null", devs, count, false);
    qsort(devs, count, sizeof(devs[0]), bt_device_cmp);

    pthread_mutex_lock(&bt_mtx);
    for (int i = 0; i < count; i++)
    {
        struct bt_device *old = bt_find(bt.dev, bt.count, devs[i].mac);
        if (old)
            devs[i].battery = old->battery;
    }
    if (count != bt.count || memcmp(devs, bt.dev, count * sizeof(devs[0])))
    {
        memcpy(bt.dev, devs, count * sizeof(devs[0]));
        bt.count = count;
        bt_changed();
    }
    pthread_mutex_unlock(&bt_mtx);
}

/* Codec, sample rate and battery of the connected headset */
static void bt_refresh_info(const char *mac)
{
    char cmd[128], path[64], line[256];
    char codec[BT_CODEC_LEN] = "";
    char codecs[BT_MAX_CODECS][BT_CODEC_LEN];
    int ncodecs = 0, rate = 0, battery = -1;
    FILE *fp;
    char *p, *save;

    bt_pcm_path(mac, path, sizeof(path));
    snprintf(cmd, sizeof(cmd), "bluealsa-cli info '%s' 2>/dev/null", path);
    fp = bt_popen(cmd);
    while (fp && fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "Selected codec:", 15))
            sscanf(line + 15, "%15s", codec);
        else if (!strncmp(line, "Sampling:", 9) || !strncmp(line, "Rate:", 5))
            rate = atoi(strchr(line, ':') + 1);
        else if (!strncmp(line, "Available codecs:", 17))
            for (p = strtok_r(line + 17, " \t", &save); p && ncodecs < BT_MAX_CODECS;
                 p = strtok_r(NULL, " \t", &save))
                snprintf(codecs[ncodecs++], BT_CODEC_LEN, "%s", p);
    }
    if (fp)
        bt_pclose(fp);

    /* "Battery Percentage: 0x50 (80)", where BlueZ knows it */
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    fp = bt_popen(cmd);
    while (fp && fgets(line, sizeof(line), fp))
        if ((p = strstr(line, "Battery Percentage:")) && (p = strchr(p, '(')))
            battery = atoi(p + 1);
    if (fp)
        bt_pclose(fp);

    pthread_mutex_lock(&bt_mtx);
    if (codec[0])
        snprintf(bt.codec, sizeof(bt.codec), "%s", codec);
    if (ncodecs)
    {
        memcpy(bt.codecs, codecs, sizeof(codecs));
        bt.ncodecs = ncodecs;
    }
    bt.rate = rate;
    struct bt_device *d = bt_find(bt.dev, bt.count, mac);
    if (d)
        d->battery = battery;
    pthread_mutex_unlock(&bt_mtx);
    bt_changed();
}

#ifndef SIMULATOR
/* BlueZ's AVRCP uinput node for the headset: the one with its name
 * (BlueZ names it after the device), else the newest dynamic one */
static int bt_find_input_node(const char *name)
{
    char line[256];
    int node = -1, best = -1, n;
    bool named = false;
    FILE *fp = fopen("/proc/bus/input/devices", "r");

    while (fp && fgets(line, sizeof(line), fp))
    {
        if (!strncmp(line, "N: Name=", 8))
            named = name[0] && strstr(line, name);
        else if (!strncmp(line, "H: Handlers=", 12) && strstr(line, "event")
                 && (n = atoi(strstr(line, "event") + 5)) >= BT_FIRST_DYNAMIC_NODE)
        {
            if (named)
                node = n;
            if (n > best)
                best = n;
        }
    }
    if (fp)
        fclose(fp);
    return node >= 0 ? node : best;
}
#endif

static void bt_set_link(const char *mac)
{
    pthread_mutex_lock(&bt_mtx);
    bool changed = strcmp(bt.link, mac) != 0;
    snprintf(bt.link, sizeof(bt.link), "%s", mac);
    pthread_mutex_unlock(&bt_mtx);
    if (!changed)
        return;
    hiby_debug_log("bt: link %s", mac[0] ? mac : "none");
    bt_linked = mac[0] != '\0';
    bt_input_node = -1;
    bt_link_gen++;
    bt_changed();
    if (mac[0])
    {
        bt_save_last(mac);
        bt_refresh_info(mac);
    }
}

/* ------------------------------------------------------------ pairing */

#ifdef SIMULATOR
#define bt_pair(mac) bt_ctl("pair", mac, "Pairing successful")
#else
static void bt_say(int fd, const char *s)
{
    if (write(fd, s, strlen(s)) < 0)
        hiby_debug_log("bt: pair pipe closed");
}

/* An interactive bluetoothctl with an agent, so a headset that asks for a
 * PIN gets 0000 and a device that shows a code has it confirmed here.
 * Prompts end without a newline, so the output is read as it comes. */
static bool bt_pair(const char *mac)
{
    int in[2], out[2];
    char buf[1024];
    size_t len = 0;
    bool ok = false;
    pid_t pid;
    int ms;

    if (pipe(in) < 0)
        return false;
    if (pipe(out) < 0)
    {
        close(in[0]);
        close(in[1]);
        return false;
    }
    pid = fork();
    if (pid == 0)
    {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        for (int fd = 3; fd < 256; fd++)
            close(fd);
        execlp("bluetoothctl", "bluetoothctl", (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0)
    {
        close(in[1]);
        close(out[0]);
        return false;
    }

    snprintf(buf, sizeof(buf), "agent DisplayYesNo\ndefault-agent\npair %s\n", mac);
    bt_say(in[1], buf);
    for (ms = 0; ms < 30000; )
    {
        struct pollfd pfd = { out[0], POLLIN, 0 };
        ssize_t n;

        if (poll(&pfd, 1, 500) <= 0)
        {
            ms += 500;
            continue;
        }
        if (len >= sizeof(buf) - 1)
            len = 0;
        n = read(out[0], buf + len, sizeof(buf) - 1 - len);
        if (n <= 0)
            break;
        len += n;
        buf[len] = '\0';
        hiby_debug_log("  pair: %s", buf + len - n);

        if (strstr(buf, "Pairing successful") || strstr(buf, "AlreadyExists"))
        {
            ok = true;
            break;
        }
        if (strstr(buf, "Failed to pair") || strstr(buf, "not available"))
            break;
        if (strstr(buf, "PIN code"))
            bt_say(in[1], "0000\n");
        else if (strstr(buf, "Confirm passkey"))
        {
            char *p = strstr(buf, "Confirm passkey") + 15;
            int wait;

            pthread_mutex_lock(&bt_mtx);
            sscanf(p, "%7s", bt.passkey);
            bt.answer = -1;
            pthread_mutex_unlock(&bt_mtx);
            bt_changed();
            for (wait = 0; wait < 30000 && bt.answer < 0; wait += 100)
                usleep(100000);
            pthread_mutex_lock(&bt_mtx);
            bt_say(in[1], bt.answer > 0 ? "yes\n" : "no\n");
            bt.passkey[0] = '\0';
            pthread_mutex_unlock(&bt_mtx);
            bt_changed();
        }
        else if (strstr(buf, "(yes/no)"))
            bt_say(in[1], "yes\n");   /* authorize: the user picked it */
        else
            continue;
        len = 0;
    }
    bt_say(in[1], "quit\n");
    close(in[1]);
    close(out[0]);
    for (ms = 0; ms < 2000 && waitpid(pid, NULL, WNOHANG) == 0; ms += 100)
        usleep(100000);
    if (ms >= 2000)
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    return ok;
}
#endif

/* ----------------------------------------------------------------- jobs */

static void bt_connect(const char *mac, bool quiet)
{
    struct bt_device *d;
    bool paired;
    char link[18];

    pthread_mutex_lock(&bt_mtx);
    snprintf(bt.busy, sizeof(bt.busy), "%s", mac);
    d = bt_find(bt.dev, bt.count, mac);
    paired = !d || d->paired;   /* the last device is not listed at boot */
    snprintf(link, sizeof(link), "%s", bt.link);
    pthread_mutex_unlock(&bt_mtx);
    bt_changed();

    /* one headset at a time */
    if (link[0] && strcmp(link, mac))
        bt_ctl("disconnect", link, NULL);

    if (!paired && !bt_pair(mac))
    {
        if (!quiet)
            bt_set_msg(LANG_BT_PAIR_FAILED);
    }
    else
    {
        bt_ctl("trust", mac, NULL);
        bt_ctl("connect", mac, "Connection successful");
        if (bt_wait_sink(mac, 8000))
            bt_set_link(mac);
        else if (!quiet)
            bt_set_msg(LANG_BT_CONNECT_FAILED);
        if (!paired)
            bt_refresh_devices();
    }

    pthread_mutex_lock(&bt_mtx);
    bt.busy[0] = '\0';
    pthread_mutex_unlock(&bt_mtx);
    bt_changed();
}

static void bt_power_on(void)
{
    char mac[18];
    FILE *fp;
    int i;

    bt_power = BT_STARTING;
    bt_changed();
    /* the stock bootloader suspends the stack: services gone, driver out */
    if (access(BT_HCI, F_OK) != 0)
        bt_cmd("/usr/bin/bt_resume 2>&1", NULL);
    /* HiBy's bt_init brings the stack up at boot and leaves it off */
    for (i = 0; i < 30 && bt_cmd("pgrep -f '[b]t_init'", NULL); i++)
        usleep(1000000);
    /* bluetoothd can still be settling: keep asking */
    for (i = 0; i < 30 && !bt_ctl("power", "on", "succeeded"); i++)
        usleep(1000000);
    if (i >= 30)
    {
        bt_power = BT_OFF;
        bt_set_msg(LANG_BT_UNAVAILABLE);
        return;
    }
    fp = fopen(BT_BOOT_FILE, "a");
    if (fp)
        fclose(fp);
    bt_power = BT_ON;
    bt_changed();

    /* back to the last headset, like a phone */
    bt_sink_mac(mac);
    if (mac[0])
        bt_set_link(mac);
    else if (bt.last[0])
    {
        snprintf(mac, sizeof(mac), "%s", bt.last);
        bt_connect(mac, true);
    }
}

static void bt_power_off(void)
{
    bt_ctl("power", "off", NULL);
    unlink(BT_BOOT_FILE);
    bt_power = BT_OFF;
    bt_set_link("");
}

static void bt_forget(const char *mac)
{
    if (!strcmp(bt.link, mac))
        bt_ctl("disconnect", mac, NULL);
    bt_ctl("remove", mac, NULL);
    if (!strcmp(bt.last, mac))
        bt_save_last("");
}

static void bt_set_codec(const char *mac, const char *codec)
{
    char cmd[128], path[64];

    bt_pcm_path(mac, path, sizeof(path));
    snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' %s 2>&1; echo rc=$?", path, codec);
    if (bt_cmd(cmd, "rc=0") && bt_wait_sink(mac, 6000))
    {
        /* asking bluealsa straight after a switch can hang it: trust it */
        pthread_mutex_lock(&bt_mtx);
        snprintf(bt.codec, sizeof(bt.codec), "%s", codec);
        pthread_mutex_unlock(&bt_mtx);
    }
    else
        bt_set_msg(LANG_BT_CODEC_CHANGE_FAILED);
    /* the UI took the PCM off the headset for the switch: back on it */
    bt_link_gen++;
    bt_changed();
}

static void bt_run_job(enum bt_job job, const char *mac, const char *arg)
{
    hiby_debug_log("bt: job %d %s %s", job, mac, arg);
    switch (job)
    {
        case BT_JOB_ON:
            bt_power_on();
            break;
        case BT_JOB_OFF:
            bt_power_off();
            break;
        case BT_JOB_CONNECT:
            bt_connect(mac, false);
            break;
        case BT_JOB_DISCONNECT:
            bt_ctl("disconnect", mac, NULL);
            break;
        case BT_JOB_FORGET:
            bt_forget(mac);
            bt_refresh_devices();
            break;
        case BT_JOB_FORGET_ALL:
            for (int i = bt.count - 1; i >= 0; i--)
                if (bt.dev[i].paired)
                    bt_forget(bt.dev[i].mac);
            bt_refresh_devices();
            break;
        case BT_JOB_CODEC:
            bt_set_codec(mac, arg);
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------- worker */

/* What the radio is doing, cheapest test first: no link at all needs no
 * fork; a link with audio is checked now and then; a link without audio
 * (earbuds out of the case keep it for their buttons) is asked to play. */
static void bt_poll_link(void)
{
    static long next_check, next_nudge;
    static int nudges;
    char mac[18];
    long now = current_tick;

    if (bt_acl_count() == 0)
    {
        bt_set_link("");
        nudges = 0;
        return;
    }
    if (bt_linked && bt_input_node < 0)
    {
        struct bt_device *d = bt_find(bt.dev, bt.count, bt.link);
        bt_input_node = bt_find_input_node(d ? d->name : "");
    }
    if (TIME_BEFORE(now, next_check))
        return;
    next_check = now + (bt_linked ? 15*HZ : 3*HZ);

    bt_sink_mac(mac);
    bt_set_link(mac);
    if (!mac[0] && bt.last[0] && nudges < 3 && TIME_AFTER(now, next_nudge))
    {
        nudges++;
        next_nudge = now + 10*HZ;
        hiby_debug_log("bt: link without audio, connecting %s", bt.last);
        bt_ctl("connect", bt.last, NULL);
        next_check = now;
    }
}

static void *bt_worker(void *arg)
{
    struct pollfd pfd[5];
    long next_refresh = 0, next_info = 0;
    FILE *scan = NULL;
    sigset_t sigpipe;
    (void)arg;

    /* Rockbox takes SIGPIPE for a crash: a bluetoothctl that went away
       makes a write here fail instead */
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigpipe, NULL);
    bt_load_last();
    for (;;)
    {
        enum bt_job job;
        char mac[18], codec[BT_CODEC_LEN];
        int n = 1, timeout;
        char c;

        pfd[0].fd = bt_wake[0];
        pfd[0].events = POLLIN;
#ifndef SIMULATOR
        if (bt_linked)
            n += hiby_bt_mixer_fds(pfd + 1, 4);
        else
            hiby_bt_mixer_close();
#endif
        timeout = bt_power == BT_OFF ? -1 : bt.screen ? 1000 : 3000;
        if (poll(pfd, n, timeout) > 0)
        {
            if (pfd[0].revents & POLLIN)
                while (read(bt_wake[0], &c, 1) == 1)
                    ;
#ifndef SIMULATOR
            if (n > 1)
            {
                int vol = hiby_bt_mixer_remote_volume(pfd + 1, n - 1);
                if (vol != INT_MIN)
                    bt_remote_vol = vol;
            }
#endif
        }

        pthread_mutex_lock(&bt_mtx);
        job = bt.job;
        bt.job = BT_JOB_NONE;
        snprintf(mac, sizeof(mac), "%s", bt.job_mac);
        snprintf(codec, sizeof(codec), "%s", bt.job_arg);
        pthread_mutex_unlock(&bt_mtx);
        if (job != BT_JOB_NONE)
        {
            bt_run_job(job, mac, codec);
            next_refresh = 0;
        }

        /* discovery while the screen is open; BlueZ forgets unpaired
         * devices when it stops, so it runs until the screen closes */
        if (bt.screen && bt_power == BT_ON && !scan && (scan = bt_scan_open()))
        {
            fputs("scan on\n", scan);
            fflush(scan);
        }
        else if ((!bt.screen || bt_power != BT_ON) && scan)
        {
            fputs("scan off\nexit\n", scan);
            bt_scan_close(scan);
            scan = NULL;
        }
        if (bt.scanning != (scan != NULL))
        {
            bt.scanning = scan != NULL;
            bt_changed();
        }
        if (bt_power != BT_ON)
            continue;
        bt_poll_link();
        if (bt.screen && TIME_AFTER(current_tick, next_refresh))
        {
            next_refresh = current_tick + 2*HZ;
            bt_refresh_devices();
        }
        if (bt.screen && bt_linked && TIME_AFTER(current_tick, next_info))
        {
            next_info = current_tick + 30*HZ;
            bt_refresh_info(bt.link);
        }
    }
    return NULL;
}

static void bt_post(enum bt_job job, const char *mac, const char *arg)
{
    pthread_mutex_lock(&bt_mtx);
    bt.job = job;
    snprintf(bt.job_mac, sizeof(bt.job_mac), "%s", mac ? mac : "");
    snprintf(bt.job_arg, sizeof(bt.job_arg), "%s", arg ? arg : "");
    pthread_mutex_unlock(&bt_mtx);
    if (write(bt_wake[1], "", 1) < 0)
        hiby_debug_log("bt: wake failed");
}

static void bt_wake_worker(void)
{
    if (write(bt_wake[1], "", 1) < 0)
        hiby_debug_log("bt: wake failed");
}

/* --------------------------------------------------------- Rockbox side */

/* Lists sleep a second between redraws: wake them often enough to show
 * the rune's quarter-second blink, for as long as it blinks */
static int bt_blink_wake(struct timeout *tmo)
{
    (void)tmo;
    if (!bt_is_starting_fast())
        return 0;
    button_queue_post(BUTTON_NONE, 0);
    return HZ/8;
}

/* Bluetooth comes back the way it was left: rb_bt_on.txt is kept while it
 * is on (the bootloader then skips bt_suspend) */
void bt_boot_init(void)
{
    static struct timeout blink;
    pthread_t worker;

    if (pipe(bt_wake) < 0)
        return;
    fcntl(bt_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(bt_wake[1], F_SETFL, O_NONBLOCK);
    if (pthread_create(&worker, NULL, bt_worker, NULL) != 0)
        return;
    pthread_detach(worker);
#ifndef SIMULATOR
    if (access(BT_BOOT_FILE, F_OK) != 0)
        return;
#endif
    bt_power = BT_STARTING;
    bt_post(BT_JOB_ON, NULL, NULL);
    timeout_register(&blink, bt_blink_wake, HZ/8, 0);
}

bool bt_is_enabled_fast(void)
{
    return bt_power == BT_ON;
}

bool bt_is_starting_fast(void)
{
    return bt_power == BT_STARTING;
}

bool bt_is_connected_fast(void)
{
    return bt_linked;
}

#ifndef SIMULATOR
static int bt_tried_gen = -1;       /* link_gen last routed or tried */
static int bt_routed_gen = -1;      /* link_gen the PCM is on the headset for */
static int bt_input_attached = -1;
static bool bt_resume_on_route;

/* From the button tick: what bt_sync() has to catch up with */
int bt_pending_event(void)
{
    bool routed = hiby_pcm_get_bt_mac() != NULL;

    if (routed && (!bt_linked || bt_link_gen != bt_routed_gen))
        return SYS_BT_UNPLUGGED;
    if ((bt_linked && bt_link_gen != bt_tried_gen)
        || (routed && bt_input_node != bt_input_attached)
        || bt_remote_vol != INT_MIN)
        return SYS_BT_PLUGGED;
    return 0;
}

static void bt_route(const char *mac)
{
    static char dev[2][64];
    static int next;
    char *route = dev[next ^= 1];
    int status = audio_status();
    bool playing = (status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE);

    snprintf(route, sizeof(dev[0]), "bluealsa:DEV=%s,PROFILE=a2dp", mac);
    /* set first: a headset the PCM cannot open clears it again, and the
       jack plays instead */
    bt_routed_gen = bt_tried_gen;
    hiby_pcm_set_bt_mac(mac);
    if (pcm_alsa_switch_playback_device(route) != 0 || !hiby_pcm_get_bt_mac())
    {
        hiby_debug_log("bt: no route to %s", mac);
        hiby_pcm_set_bt_mac(NULL);
        pcm_alsa_switch_playback_device(BT_LOCAL_PLAYBACK_DEVICE);
        return;
    }
    bt_input_attached = -2;
    /* the headset's own level from the player's */
    sound_set_volume(global_status.volume);
    if (playing)
    {
        audio_pause();
        sleep(HZ/4);
        audio_resume();
    }
    else if (bt_resume_on_route && (status & AUDIO_STATUS_PAUSE))
        audio_resume();
    bt_resume_on_route = false;
    hiby_debug_log("bt: routed to %s", mac);
}

void bt_sync(void)
{
    char link[18];

    if (bt_linked && bt_link_gen != bt_tried_gen)
    {
        bt_tried_gen = bt_link_gen;
        pthread_mutex_lock(&bt_mtx);
        snprintf(link, sizeof(link), "%s", bt.link);
        pthread_mutex_unlock(&bt_mtx);
        if (link[0])
            bt_route(link);
    }
    if (hiby_pcm_get_bt_mac() && bt_input_node != bt_input_attached)
    {
        int node = bt_input_node;
        if (node >= 0)
            button_add_input_node(BT_REMOTE_INPUT_IDX, node);
        else
            button_remove_input_device(BT_REMOTE_INPUT_IDX);
        bt_input_attached = node;
    }
    if (bt_remote_vol != INT_MIN)
    {
        global_status.volume = bt_remote_vol;
        bt_remote_vol = INT_MIN;
        setvol();
    }
}

/* Off the headset and paused, as a phone does when headphones go. The
 * switch is only safe with the stream stopped; a fade would leave it
 * running until the fade ends, and nobody is listening to it anyway. */
void bt_unroute(bool resume_later)
{
    int status = audio_status();
    bool fade = global_settings.fade_on_stop;

    if (!hiby_pcm_get_bt_mac())
        return;
    hiby_debug_log("bt: unrouting");
    global_settings.fade_on_stop = false;
    audio_pause();
    global_settings.fade_on_stop = fade;
    pcm_play_stop();
    button_remove_input_device(BT_REMOTE_INPUT_IDX);
    bt_input_attached = -1;
    hiby_pcm_set_bt_mac(NULL);
    pcm_alsa_switch_playback_device(BT_LOCAL_PLAYBACK_DEVICE);
    bt_resume_on_route = resume_later && (status & AUDIO_STATUS_PLAY)
                         && !(status & AUDIO_STATUS_PAUSE);
}
#else
void bt_unroute(bool resume_later)
{
    (void)resume_later;
}
#endif

void bt_toggle_power(void)
{
    if (bt_power == BT_OFF)
    {
        bt_power = BT_STARTING;
        bt_post(BT_JOB_ON, NULL, NULL);
    }
    else
    {
        bt_unroute(false);
        bt_power = BT_OFF;
        bt_post(BT_JOB_OFF, NULL, NULL);
    }
    bt_changed();
}

static const char *bt_power_str(void)
{
    return S(bt_power == BT_ON ? LANG_ON :
             bt_power == BT_STARTING ? LANG_BT_TURNING_ON : LANG_OFF);
}

const char *bt_qs_val(char *buf, size_t len)
{
    snprintf(buf, len, "%s", bt_power_str());
    return buf;
}

/* --------------------------------------------------------------- screens */

/* A copy of the shared state, taken when it changes */
static struct
{
    struct bt_device dev[BT_MAX_DEVICES];
    int count;
    char link[18], busy[18], codec[BT_CODEC_LEN];
    char codecs[BT_MAX_CODECS][BT_CODEC_LEN];
    int ncodecs, rate;
    bool scanning;
    int gen;
} snap;

static void bt_snapshot(void)
{
    int msg;
    char passkey[8];

    snap.gen = bt_gen;
    pthread_mutex_lock(&bt_mtx);
    memcpy(snap.dev, bt.dev, sizeof(snap.dev));
    snap.count = bt.count;
    strcpy(snap.link, bt.link);
    strcpy(snap.busy, bt.busy);
    strcpy(snap.codec, bt.codec);
    memcpy(snap.codecs, bt.codecs, sizeof(snap.codecs));
    snap.ncodecs = bt.ncodecs;
    snap.rate = bt.rate;
    snap.scanning = bt.scanning;
    msg = bt.msg;
    bt.msg = 0;
    strcpy(passkey, bt.passkey);
    pthread_mutex_unlock(&bt_mtx);

    if (msg)
        splash(HZ*2, ID2P(msg));
    /* a device that shows a code: the user says whether it matches */
    if (passkey[0] && bt.answer < 0)
    {
        const char *lines[] = { S(LANG_BT_PAIR_CONFIRM), passkey };
        const struct text_message message = { lines, 2 };
        bt.answer = gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES;
    }
}

static const char *bt_device_state(const struct bt_device *d, char *buf, size_t len)
{
    if (!strcmp(d->mac, snap.busy))
        snprintf(buf, len, "%s (%s)", d->name, S(LANG_BT_CONNECTING));
    else if (!strcmp(d->mac, snap.link) && d->battery >= 0)
        snprintf(buf, len, "%s (%s, %d%%)", d->name, S(LANG_BT_CONNECTED), d->battery);
    else if (!strcmp(d->mac, snap.link))
        snprintf(buf, len, "%s (%s)", d->name, S(LANG_BT_CONNECTED));
    else
        snprintf(buf, len, "%s", d->name);
    return buf;
}

/* The one Bluetooth screen: the switch, paired devices, devices around */
enum { ROW_POWER, ROW_PAIRED, ROW_OTHER, ROW_DEVICE, ROW_FORGET_ALL, ROW_OFFSET };
static struct { unsigned char kind, dev; } rows[BT_MAX_DEVICES + 5];
static int nrows;

static void bt_build_rows(void)
{
    bool other = false;
    int i;

    nrows = 0;
    rows[nrows++].kind = ROW_POWER;
    if (bt_power == BT_ON)
    {
        for (i = 0; i < snap.count; i++)
        {
            if (i == 0 && snap.dev[i].paired)
                rows[nrows++].kind = ROW_PAIRED;
            if (!snap.dev[i].paired && !other)
            {
                rows[nrows++].kind = ROW_OTHER;
                other = true;
            }
            rows[nrows].kind = ROW_DEVICE;
            rows[nrows++].dev = i;
        }
        if (!other)
            rows[nrows++].kind = ROW_OTHER;
        if (snap.count && snap.dev[0].paired)
            rows[nrows++].kind = ROW_FORGET_ALL;
    }
    rows[nrows++].kind = ROW_OFFSET;
}

static const char *bt_row_name(int i, void *data, char *buf, size_t len)
{
    (void)data;
    if (i < 0 || i >= nrows)
        return "";
    switch (rows[i].kind)
    {
        case ROW_POWER:
            snprintf(buf, len, "%s: %s", S(LANG_BT_BLUETOOTH), bt_power_str());
            return buf;
        case ROW_PAIRED:
            return S(LANG_BT_PAIRED_DEVICES);
        case ROW_OTHER:
            if (!snap.scanning)
                return S(LANG_BT_OTHER_DEVICES);
            snprintf(buf, len, "%s (%s)", S(LANG_BT_OTHER_DEVICES), S(LANG_BT_SEARCHING));
            return buf;
        case ROW_DEVICE:
            return bt_device_state(&snap.dev[rows[i].dev], buf, len);
        case ROW_FORGET_ALL:
            return S(LANG_BT_FORGET_ALL);
        case ROW_OFFSET:
            snprintf(buf, len, "%s: %d ms", S(LANG_BT_WIRED_OFFSET),
                     global_settings.bt_wired_offset);
            return buf;
    }
    return "";
}

static enum themable_icons bt_row_icon(int i, void *data)
{
    (void)data;
    if (i >= 0 && i < nrows && rows[i].kind == ROW_DEVICE
        && !strcmp(snap.dev[rows[i].dev].mac, snap.link))
        return Icon_Audio;
    return Icon_NOICON;
}

static bool bt_context;

/* A long press is "open it"; the worker's news redraws the rows */
static int bt_screen_cb(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_CONTEXT)
    {
        bt_context = true;
        return ACTION_STD_OK;
    }
    if (snap.gen != bt_gen)
    {
        bt_snapshot();
        bt_build_rows();
        gui_synclist_set_nb_items(lists, nrows);
        return ACTION_REDRAW;
    }
    return action;
}

static bool bt_page_stale;

/* The page's lines are fixed text: on news, leave and build them again */
static int bt_page_cb(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action != ACTION_STD_OK && snap.gen != bt_gen)
    {
        bt_page_stale = true;
        return ACTION_STD_CANCEL;
    }
    return action;
}

static void bt_codec_picker(void)
{
    struct simplelist_info info;
    char mac[18];
    int i;

    if (!snap.ncodecs)
    {
        splash(HZ, ID2P(LANG_BT_NO_CODECS));
        return;
    }
    simplelist_info_init(&info, (char *)S(LANG_BT_SELECT_CODEC), 0, NULL);
    simplelist_reset_lines();
    for (i = 0; i < snap.ncodecs; i++)
    {
        simplelist_addline("%s", snap.codecs[i]);
        if (!strcmp(snap.codecs[i], snap.codec))
            info.selection = i;
    }
    info.count = simplelist_get_line_count();
    simplelist_show_list(&info);
    if (info.selection < 0 || info.selection >= snap.ncodecs
        || !strcmp(snap.codecs[info.selection], snap.codec))
        return;
    /* bluealsa takes no new codec while its PCM is open */
    strcpy(mac, snap.link);
    bt_unroute(true);
    bt_post(BT_JOB_CODEC, mac, snap.codecs[info.selection]);
}

enum { DET_STATUS, DET_CODEC, DET_RATE, DET_BATTERY, DET_CONNECT, DET_FORGET };

/* A device's own page: what it is doing, and what can be done to it */
static void bt_device_page(const char *mac)
{
    struct simplelist_info info;
    unsigned char kind[8];
    char title[BT_NAME_LEN];
    int sel = 0;

    while (1)
    {
        const struct bt_device *d = bt_find(snap.dev, snap.count, mac);
        bool linked = !strcmp(snap.link, mac), busy = !strcmp(snap.busy, mac);
        int n = 0;

        if (!d)
            return;     /* forgotten */
        strcpy(title, d->name);
        simplelist_info_init(&info, title, 0, NULL);
        simplelist_reset_lines();
        simplelist_addline("%s: %s", S(LANG_BT_STATUS),
                           S(busy ? LANG_BT_CONNECTING :
                             linked ? LANG_BT_CONNECTED : LANG_BT_DISCONNECTED));
        kind[n++] = DET_STATUS;
        if (linked)
        {
            simplelist_addline("%s: %s", S(LANG_BT_CODEC),
                               snap.codec[0] ? snap.codec : S(LANG_UNKNOWN));
            kind[n++] = DET_CODEC;
            if (snap.rate)
            {
                simplelist_addline("%s: %d Hz", S(LANG_BT_SAMPLE_RATE), snap.rate);
                kind[n++] = DET_RATE;
            }
        }
        if (d->battery >= 0)
        {
            simplelist_addline("%s: %d%%", S(LANG_BATTERY_MENU), d->battery);
            kind[n++] = DET_BATTERY;
        }
        if (!busy)
        {
            simplelist_addline("%s", S(linked ? LANG_BT_DISCONNECT : LANG_BT_CONNECT));
            kind[n++] = DET_CONNECT;
        }
        if (d->paired)
        {
            simplelist_addline("%s", S(LANG_BT_FORGET));
            kind[n++] = DET_FORGET;
        }
        info.count = n;
        info.selection = sel < n ? sel : n - 1;
        info.action_callback = bt_page_cb;
        bt_page_stale = false;
        simplelist_show_list(&info);
        if (bt_page_stale)
        {
            bt_snapshot();
            continue;
        }
        if (info.selection < 0)
            return;
        sel = info.selection;
        switch (kind[sel])
        {
            case DET_CODEC:
                bt_codec_picker();
                break;
            case DET_CONNECT:
                bt_unroute(!linked);
                bt_post(linked ? BT_JOB_DISCONNECT : BT_JOB_CONNECT, mac, NULL);
                break;
            case DET_FORGET:
                if (confirm_delete_yesno(title, NULL) == YESNO_YES)
                {
                    if (linked)
                        bt_unroute(false);
                    bt_post(BT_JOB_FORGET, mac, NULL);
                    return;
                }
                break;
        }
        bt_snapshot();
    }
}

int hiby_bluetooth_menu(void)
{
    struct simplelist_info info;
    int sel = 0;

    bt.screen = true;
    bt_wake_worker();
    bt_snapshot();
    while (1)
    {
        bt_build_rows();
        simplelist_info_init(&info, (char *)S(LANG_BT_BLUETOOTH), nrows, NULL);
        info.get_name = bt_row_name;
        info.get_icon = bt_row_icon;
        info.action_callback = bt_screen_cb;
        info.selection = sel < nrows ? sel : nrows - 1;
        info.title_icon = Icon_Submenu;
        bt_context = false;
        simplelist_show_list(&info);
        if (info.selection < 0)
            break;
        sel = info.selection;

        switch (rows[sel].kind)
        {
            case ROW_POWER:
                bt_toggle_power();
                break;
            case ROW_DEVICE:
            {
                char mac[18];
                strcpy(mac, snap.dev[rows[sel].dev].mac);
                /* a tap connects; a connected one, or a long press, opens */
                if (bt_context || !strcmp(mac, snap.link))
                    bt_device_page(mac);
                else if (strcmp(mac, snap.busy))
                {
                    bt_unroute(true);
                    bt_post(BT_JOB_CONNECT, mac, NULL);
                }
                break;
            }
            case ROW_FORGET_ALL:
            {
                const char *lines[] = { S(LANG_BT_FORGET_ALL) };
                const struct text_message message = { lines, 1 };
                if (gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
                {
                    bt_unroute(false);
                    bt_post(BT_JOB_FORGET_ALL, NULL, NULL);
                }
                break;
            }
            case ROW_OFFSET:
                /* + holds the jack back more, - less, when it plays
                   beside a headset */
                set_int(S(LANG_BT_WIRED_OFFSET), "ms", UNIT_MS,
                        &global_settings.bt_wired_offset, NULL, 10, -500, 500,
                        NULL);
                settings_save();
                break;
        }
        bt_snapshot();
    }
    bt.screen = false;
    bt_wake_worker();
    return 0;
}

#endif /* HAVE_HIBY_BLUETOOTH */
