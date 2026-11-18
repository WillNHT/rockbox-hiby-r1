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
#include "hiby_wifi.h"

#if defined(HIBY_LINUX) && !defined(SIMULATOR)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel.h"
#include "system.h"
#include "version.h"
#include "audio.h"
#include "action.h"
#include "keyboard.h"
#include "misc.h"
#include "splash.h"
#include "yesno.h"
#include "gui/list.h"
#include "hiby_bluetooth.h"

/* Wi-Fi brought up the way HiBy OS does it (wifi_on.sh): wpa_supplicant
 * joins a network, udhcpc gets an address. The driver powers the chip with
 * wlan0: up loads its firmware, down switches it off. A bootloader that
 * still unbinds the chip leaves no wlan0, and the driver cannot be bound
 * again. Off until switched on - the radio is the biggest battery cost the player
 * has - and it comes back the way it was left: wifi_on.txt is kept while it
 * is on. Nothing listens: Rockbox only ever talks out. Networks and
 * passwords are kept by wpa_supplicant itself, in wifi.conf. */
#define WIFI_IF        "wlan0"
#define WIFI_CONF      PIVOT_ROOT ROCKBOX_DIR "/wifi.conf"
#define WIFI_CTRL      "/var/run/wpa_supplicant/" WIFI_IF
#define WIFI_CLI       "wpa_cli -i " WIFI_IF " "
#define WIFI_MAX_NETS  32
#define WIFI_SSID_LEN  132  /* 32 bytes, escaped the way wpa_cli prints them */
#define WIFI_OUT_OF_RANGE (-1000)
#define WIFI_SDIO      "/sys/bus/sdio/drivers/bcmsdh_sdmmc/"
#define WIFI_LOG       PIVOT_ROOT "/rockbox-wifi-debug.log"
#define WIFI_BOOT_FILE PIVOT_ROOT ROCKBOX_DIR "/wifi_on.txt"

/* Updates: the release the fork's GitHub page calls latest, or the zip
 * update.url names - a laptop serving a build on the same network. */
#define UPDATE_RELEASES "https://github.com/WillNHT/rockbox-hiby-r1/releases"
#define UPDATE_URL     PIVOT_ROOT ROCKBOX_DIR "/update.url"
#define UPDATE_ZIP     PIVOT_ROOT ROCKBOX_DIR "/update.zip"
#define UPDATE_DIR     PIVOT_ROOT "/.rockbox-update"
/* curl with TLS GitHub accepts, shipped in the zip (tools/rbcurl): the
 * player's busybox wget fails the handshake. Run from /tmp, where it can
 * be executable. */
#define UPDATE_CURL    "/tmp/rbcurl"
#define UPDATE_GET     UPDATE_CURL " -sSfL --cacert " PIVOT_ROOT ROCKBOX_DIR "/lib/cacert.pem "

struct wifi_net
{
    char ssid[WIFI_SSID_LEN];
    int signal;     /* dBm, WIFI_OUT_OF_RANGE for a saved one not seen */
    int id;         /* saved network id, -1 when not saved */
    bool secured;
};

static struct wifi_net wifi_nets[WIFI_MAX_NETS];
static int wifi_net_count;
/* For %WF: 0 off, 1 on, 2 joined. Kept by wifi_worker(). */
static volatile int wifi_state;
/* The menu and the worker both bring the radio up */
static pthread_mutex_t wifi_lock = PTHREAD_MUTEX_INITIALIZER;

static bool wifi_is_on(void)
{
    return access(WIFI_CTRL, F_OK) == 0;
}

/* Name and address of the joined network; false (and both empty) if none */
static bool wifi_status(char *ssid, char *ip)
{
    char line[200];
    bool joined = false;
    FILE *fp;

    ssid[0] = ip[0] = '\0';
    fp = popen(WIFI_CLI "status 2>/dev/null", "r");
    if (!fp)
        return false;
    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "ssid=", 5))
            snprintf(ssid, WIFI_SSID_LEN, "%s", line + 5);
        else if (!strncmp(line, "ip_address=", 11))
            snprintf(ip, 16, "%s", line + 11);
        else if (!strcmp(line, "wpa_state=COMPLETED"))
            joined = true;
    }
    pclose(fp);
    if (!joined)
        ssid[0] = ip[0] = '\0';
    return joined;
}

/* A number a wpa_cli command prints (add_network), -1 on failure */
static int wifi_cli_int(const char *cmd)
{
    char line[32];
    int n = -1;
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    if (fgets(line, sizeof(line), fp) && line[0] >= '0' && line[0] <= '9')
        n = atoi(line);
    pclose(fp);
    return n;
}

/* s as one sh argument, inside the double quotes wpa_supplicant wants:
 * 'P"my net"' for an SSID as wpa_cli prints it, '"secret"' for a password */
static void wifi_quote(char *out, size_t len, const char *pre, const char *s)
{
    size_t n = snprintf(out, len, "'%s\"", pre);
    for (; *s && n + 8 < len; s++)
    {
        if (*s == '\'')
        {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        }
        else
            out[n++] = *s;
    }
    snprintf(out + n, len - n, "\"'");
}

/* udhcpc stays behind (-b), renewing, and asks again after a rejoin */
static void wifi_dhcp(void)
{
    system("killall udhcpc >/dev/null 2>&1; "
           "udhcpc -b -i " WIFI_IF " -x hostname:rockbox >/dev/null 2>&1 &");
}

/* No screen here: the worker calls it too, at boot, and it is not a
 * Rockbox thread, so it cannot sleep() the way one does */
static bool wifi_up(bool worker)
{
    int i;

    pthread_mutex_lock(&wifi_lock);
    if (!wifi_is_on())
    {
        system("[ -f " WIFI_CONF " ] || printf 'ctrl_interface=/var/run/wpa_supplicant\\n"
               "update_config=1\\n' > " WIFI_CONF);
        system("ifconfig " WIFI_IF " up && wpa_supplicant -B -D nl80211 -i " WIFI_IF
               " -c " WIFI_CONF " >/tmp/wifi.log 2>&1");
        for (i = 0; i < 30 && !wifi_is_on(); i++)
        {
            if (worker)
                usleep(100000);
            else
                sleep(HZ/10);
        }
        if (wifi_is_on())
            /* a saved network is joined on its own, the last one joined
             * first (wifi_connect() ranks them) */
            wifi_dhcp();
        else
            /* why, for whoever reads the card */
            system("{ cat /tmp/wifi.log; ls /sys/bus/sdio/devices " WIFI_SDIO "; ifconfig -a; "
                   "rfkill list; dmesg | tail -n 40; } > " WIFI_LOG " 2>&1");
    }
    pthread_mutex_unlock(&wifi_lock);
    return wifi_is_on();
}

static bool wifi_on(void)
{
    FILE *fp;

    if (wifi_is_on())
        return true;
    splash(0, ID2P(LANG_WAIT));
    if (!wifi_up(false))
    {
        splash(HZ*2, ID2P(LANG_WIFI_UNAVAILABLE));
        return false;
    }
    wifi_state = 1;
    /* on again at the next boot */
    fp = fopen(WIFI_BOOT_FILE, "a");
    if (fp)
        fclose(fp);
    return true;
}

static void wifi_off(void)
{
    int i;

    unlink(WIFI_BOOT_FILE);
    pthread_mutex_lock(&wifi_lock);
    system("killall udhcpc wpa_supplicant >/dev/null 2>&1; ifconfig " WIFI_IF " down");
    for (i = 0; i < 10 && wifi_is_on(); i++)
        sleep(HZ/10);
    pthread_mutex_unlock(&wifi_lock);
    wifi_state = 0;
}

/* HiBy's bt_resume brings up the radio chip Wi-Fi shares with Bluetooth
 * from scratch, and with Wi-Fi up on it no controller appeared. Wi-Fi
 * steps aside while it runs and comes back after (hiby_bluetooth.c). */
bool hiby_wifi_pause(void)
{
    if (!wifi_is_on())
        return false;
    pthread_mutex_lock(&wifi_lock);
    system("killall udhcpc wpa_supplicant >/dev/null 2>&1; ifconfig " WIFI_IF " down");
    pthread_mutex_unlock(&wifi_lock);
    return true;
}

void hiby_wifi_resume(void)
{
    wifi_up(true);
}

/* On, by the socket wpa_supplicant leaves, but is anyone there? A chip
 * reset under it takes wpa_supplicant away and leaves the socket: Wi-Fi
 * then reads as on with no network and nothing in a scan. */
static bool wifi_answers(void)
{
    char line[8];
    FILE *fp = popen(WIFI_CLI "ping 2>/dev/null", "r");
    if (!fp)
        return true;
    if (!fgets(line, sizeof(line), fp))
        line[0] = '\0';
    pclose(fp);
    return !strncmp(line, "PONG", 4);
}

/* Each line a command prints, into rockbox-bt-debug.log beside what
 * Bluetooth did to the chip, with the seconds since boot */
static void wifi_debug(const char *cmd)
{
    char line[200];
    FILE *fp = popen(cmd, "r");

    while (fp && fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = '\0';
        hiby_debug_log("wifi %ld: %s", current_tick / HZ, line);
    }
    if (fp)
        pclose(fp);
}

/* From scratch: the worker, when nobody is there or the chip sees nothing.
 * wlan0 down and up loads the chip's firmware again. */
static void wifi_restart(void)
{
    hiby_debug_log("wifi %ld: restart", current_tick / HZ);
    pthread_mutex_lock(&wifi_lock);
    system("killall udhcpc wpa_supplicant >/dev/null 2>&1; rm -f " WIFI_CTRL
           "; ifconfig " WIFI_IF " down");
    pthread_mutex_unlock(&wifi_lock);
    wifi_up(true);
}

static void wifi_add(const char *ssid, int signal, int id, bool secured)
{
    struct wifi_net *net = NULL;
    int i;

    for (i = 0; i < wifi_net_count; i++)
        if (!strcmp(wifi_nets[i].ssid, ssid))
            net = &wifi_nets[i];
    if (!net)
    {
        if (wifi_net_count >= WIFI_MAX_NETS)
            return;
        net = &wifi_nets[wifi_net_count++];
        snprintf(net->ssid, sizeof(net->ssid), "%s", ssid);
        net->signal = WIFI_OUT_OF_RANGE;
        net->id = -1;
        net->secured = false;
    }
    if (signal > net->signal)
        net->signal = signal;
    if (id >= 0)
        net->id = id;
    net->secured |= secured;
}

/* Split a tab separated line into n fields */
static bool wifi_split(char *line, char **f, int n)
{
    int i;

    line[strcspn(line, "\r\n")] = '\0';
    f[0] = line;
    for (i = 1; i < n; i++)
    {
        char *p = strchr(f[i - 1], '\t');
        if (!p)
            return false;
        *p = '\0';
        f[i] = p + 1;
    }
    return true;
}

static int wifi_net_cmp(const void *a, const void *b)
{
    return ((const struct wifi_net *)b)->signal - ((const struct wifi_net *)a)->signal;
}

/* Networks in range, strongest first, then saved ones that are not */
static void wifi_scan(void)
{
    char line[256];
    char *f[5];
    FILE *fp;

    splash(0, ID2P(LANG_WIFI_SCANNING));
    system(WIFI_CLI "scan >/dev/null 2>&1");
    sleep(HZ*3);

    wifi_net_count = 0;
    fp = popen(WIFI_CLI "scan_results 2>/dev/null", "r");
    while (fp && fgets(line, sizeof(line), fp))
    {
        /* bssid, frequency, signal, flags, ssid; hidden networks have none */
        if (!wifi_split(line, f, 5) || !f[4][0] || !strncmp(line, "bssid", 5))
            continue;
        wifi_add(f[4], atoi(f[2]), -1,
                 strstr(f[3], "WPA") || strstr(f[3], "WEP") || strstr(f[3], "SAE"));
    }
    if (fp)
        pclose(fp);

    fp = popen(WIFI_CLI "list_networks 2>/dev/null", "r");
    while (fp && fgets(line, sizeof(line), fp))
    {
        /* id, ssid, bssid, flags */
        if (!wifi_split(line, f, 4) || f[0][0] < '0' || f[0][0] > '9')
            continue;
        wifi_add(f[1], WIFI_OUT_OF_RANGE, atoi(f[0]), false);
    }
    if (fp)
        pclose(fp);

    qsort(wifi_nets, wifi_net_count, sizeof(wifi_nets[0]), wifi_net_cmp);
}

static void wifi_forget(int id)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), WIFI_CLI "remove_network %d >/dev/null", id);
    system(cmd);
    system(WIFI_CLI "save_config >/dev/null");
}

static void wifi_connect(const struct wifi_net *net)
{
    char cmd[400], arg[300], ssid[WIFI_SSID_LEN], ip[16];
    char pass[64] = "";
    int id = net->id, i;
    bool joined = false;

    if (id < 0)
    {
        if (net->secured && (kbd_input(pass, sizeof(pass), NULL) < 0 || strlen(pass) < 8))
            return;
        id = wifi_cli_int(WIFI_CLI "add_network");
        if (id < 0)
        {
            splash(HZ*2, ID2P(LANG_WIFI_CONNECT_FAILED));
            return;
        }
        wifi_quote(arg, sizeof(arg), "P", net->ssid);
        snprintf(cmd, sizeof(cmd), WIFI_CLI "set_network %d ssid %s >/dev/null", id, arg);
        system(cmd);
        if (net->secured)
        {
            wifi_quote(arg, sizeof(arg), "", pass);
            snprintf(cmd, sizeof(cmd), WIFI_CLI "set_network %d psk %s >/dev/null", id, arg);
        }
        else
            snprintf(cmd, sizeof(cmd), WIFI_CLI "set_network %d key_mgmt NONE >/dev/null", id);
        system(cmd);
    }

    splash(0, ID2P(LANG_BT_CONNECTING));
    snprintf(cmd, sizeof(cmd), WIFI_CLI "select_network %d >/dev/null", id);
    system(cmd);
    for (i = 0; i < 30 && !joined; i++)
    {
        sleep(HZ/2);
        joined = wifi_status(ssid, ip) && !strcmp(ssid, net->ssid);
    }

    if (!joined)
    {
        /* a mistyped password is not kept */
        if (net->id < 0)
            wifi_forget(id);
        system(WIFI_CLI "enable_network all >/dev/null");
        splash(HZ*2, ID2P(LANG_WIFI_CONNECT_FAILED));
        return;
    }
    /* select_network turned the others off: they may join again later,
     * this one ahead of them - the last network joined is the first one
     * tried when Wi-Fi next comes on */
    system(WIFI_CLI "enable_network all >/dev/null");
    snprintf(cmd, sizeof(cmd), "p=0; for i in $(" WIFI_CLI "list_networks | "
             "sed -n 's/^\\([0-9][0-9]*\\)\t.*/\\1/p'); do "
             "q=$(" WIFI_CLI "get_network $i priority); "
             "[ \"$q\" -gt $p ] 2>/dev/null && p=$q; done; "
             WIFI_CLI "set_network %d priority $((p + 1)) >/dev/null", id);
    system(cmd);
    system(WIFI_CLI "save_config >/dev/null");
    wifi_state = 2;

    wifi_dhcp();
    for (i = 0; i < 20 && !ip[0]; i++)
    {
        sleep(HZ/2);
        wifi_status(ssid, ip);
    }
    if (ip[0])
        splash(HZ, ID2P(LANG_BT_CONNECTED));
    else
        splash(HZ*2, ID2P(LANG_WIFI_NO_ADDRESS));
}

static const char *wifi_net_name_cb(int selected_item, void *data,
                                    char *buffer, size_t buffer_len)
{
    (void)data;
    if (selected_item == 0)
        return (const char *)str(LANG_WIFI_SCAN);
    selected_item--;
    if (selected_item < 0 || selected_item >= wifi_net_count)
        return "";
    /* saved networks are starred */
    snprintf(buffer, buffer_len, "%s%s", wifi_nets[selected_item].id >= 0 ? "* " : "",
             wifi_nets[selected_item].ssid);
    return buffer;
}

static int wifi_netlist_callback(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    if (action == ACTION_STD_CONTEXT && lists->selected_item > 0)
    {
        struct wifi_net *net = &wifi_nets[lists->selected_item - 1];
        if (net->id >= 0 && confirm_delete_yesno(net->ssid, NULL) == YESNO_YES)
        {
            wifi_forget(net->id);
            net->id = -1;
            return ACTION_REDRAW;
        }
    }
    return action;
}

static void wifi_show_networks(void)
{
    struct simplelist_info info;

    if (!wifi_on())
        return;
    wifi_scan();
    while (1)
    {
        simplelist_info_init(&info, (char *)str(LANG_WIFI_NETWORK), wifi_net_count + 1, NULL);
        info.get_name = wifi_net_name_cb;
        info.action_callback = wifi_netlist_callback;
        info.selection = wifi_net_count > 0 ? 1 : 0;
        info.title_icon = Icon_Submenu;
        simplelist_show_list(&info);

        if (info.selection < 0)
            return;
        if (info.selection == 0)
        {
            wifi_scan();
            continue;
        }
        if (info.selection <= wifi_net_count)
        {
            wifi_connect(&wifi_nets[info.selection - 1]);
            return;
        }
    }
}

/* First line a command prints, cut at anything that is not safe to put back
 * into a command: nothing checks who the answer came from */
static void wifi_line(const char *cmd, char *out, size_t len)
{
    FILE *fp = popen(cmd, "r");

    out[0] = '\0';
    if (!fp)
        return;
    if (!fgets(out, len, fp))
        out[0] = '\0';
    pclose(fp);
    out[strspn(out, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "0123456789.-_:/%~+=?&")] = '\0';
}

/* What wifi_update() came back with, said once the menu has drawn itself
 * again: said straight away, it sat on what the wait left behind. */
static int wifi_msg;

/* Fetch a release zip and unpack it over the card, then restart into it */
static void wifi_update(void)
{
    char url[300], tag[40], sum[70], cmd[1000];

    tag[0] = sum[0] = '\0';
    splash(0, ID2P(LANG_WAIT));
    system("cp " PIVOT_ROOT ROCKBOX_DIR "/lib/curl " UPDATE_CURL " && chmod +x " UPDATE_CURL);
    wifi_line("head -n 1 " UPDATE_URL " 2>/dev/null", url, sizeof(url));
    if (url[0])
        /* its second line, if there is one: the sha256 the zip must have */
        wifi_line("sed -n 2p " UPDATE_URL, sum, sizeof(sum));
    else
    {
        /* releases/latest redirects to releases/tag/<version>. What was
         * asked and answered goes to the card: a player that cannot reach
         * GitHub says why there, not only "failed". */
        wifi_line("{ date; " UPDATE_CURL " --version; cat /etc/resolv.conf; route -n; "
                  UPDATE_GET "-I -m 15 " UPDATE_RELEASES "/latest || "
                  "wget -S --spider -T 15 " UPDATE_RELEASES "/latest; } > " WIFI_LOG " 2>&1; "
                  "sed -n 's|.*/releases/tag/||p' " WIFI_LOG " | head -n 1",
                  tag, sizeof(tag));
        if (!tag[0])
        {
            wifi_msg = LANG_WIFI_UPDATE_FAILED;
            return;
        }
        if (!strcmp(tag, rbversion))
        {
            wifi_msg = LANG_WIFI_UP_TO_DATE;
            return;
        }
        snprintf(url, sizeof(url), UPDATE_RELEASES "/download/%s/rockbox-hibyr1-%s-sdcard.zip",
                 tag, tag);
    }
    snprintf(cmd, sizeof(cmd), "%s: %s", (const char *)str(LANG_WIFI_UPDATE),
             tag[0] ? tag : url);
    if (!yesno_pop(cmd))
        return;

    /* the codecs are about to be replaced */
    audio_stop();
    splash(0, ID2P(LANG_WIFI_DOWNLOADING));
    snprintf(cmd, sizeof(cmd), "{ " UPDATE_GET "-m 900 -o " UPDATE_ZIP " '%s' || wget -T 30 -O "
             UPDATE_ZIP " '%s'; } >> " WIFI_LOG " 2>&1%s%s%s", url, url,
             sum[0] ? " && [ \"$(sha256sum < " UPDATE_ZIP " | cut -c1-64)\" = \"" : "",
             sum, sum[0] ? "\" ]" : "");
    /* unpacked beside the install and moved in file by file: a download cut
     * short changes nothing, and a library in use is replaced, not rewritten */
    if (system(cmd) ||
        system("{ rm -rf " UPDATE_DIR "; mkdir " UPDATE_DIR
               " && unzip -o -q " UPDATE_ZIP " -d " UPDATE_DIR
               " && [ -f " UPDATE_DIR ROCKBOX_DIR "/" BOOTFILE " ] && cd " UPDATE_DIR
               " && find . -type d -exec mkdir -p " PIVOT_ROOT "/{} \\;"
               " && find . -type f -exec mv -f {} " PIVOT_ROOT "/{} \\;; } >> " WIFI_LOG " 2>&1"))
    {
        system("rm -rf " UPDATE_DIR " " UPDATE_ZIP);
        wifi_msg = LANG_WIFI_UPDATE_FAILED;
        return;
    }
    system("rm -rf " UPDATE_DIR " " UPDATE_ZIP "; sync");
    system_reboot();
}

static int wifi_ok_cancel(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    if (action == ACTION_NONE && wifi_msg)
    {
        splash(HZ*2, ID2P(wifi_msg));
        wifi_msg = 0;
        return ACTION_REDRAW;
    }
    return action;
}

/* Not a Rockbox thread: it blocks in system() and popen() */
static void *wifi_worker(void *arg)
{
    char ssid[WIFI_SSID_LEN] = "", ip[16];
    int unjoined = 0, patience = 6;

    (void)arg;
    /* Wi-Fi comes back the way it was left - once Bluetooth is done with
     * the radio chip the two share (HiBy's bt_init, and bt_resume when
     * Bluetooth comes back on at boot): a network joined while either runs
     * is dropped a little later, when it resets the chip */
    if (access(WIFI_BOOT_FILE, F_OK) == 0)
    {
        for (int i = 0; i < 90 &&
             (system("pgrep -f '[b]t_init' >/dev/null 2>&1") == 0 ||
              bt_is_starting_fast()); i++)
            usleep(1000000);
        hiby_debug_log("wifi %ld: up", current_tick / HZ);
        wifi_up(true);
    }
    while (1)
    {
        int was = wifi_state;
        /* off costs one access() a turn; on, one wpa_cli */
        wifi_state = !wifi_is_on() ? 0 : wifi_status(ssid, ip) ? 2 : 1;
        if (was != wifi_state)
            hiby_debug_log("wifi %ld: state %d -> %d %s", current_tick / HZ,
                           was, wifi_state, ssid);
        if (was == 2 && wifi_state != 2)
            /* the network went: why, for whoever reads the card */
            wifi_debug("{ " WIFI_CLI "status; " WIFI_CLI "scan_results; ifconfig " WIFI_IF
                       "; rfkill list; dmesg | tail -n 40; } 2>&1");
        unjoined = wifi_state == 1 ? unjoined + 1 : 0;
        if (wifi_state == 2)
            patience = 6;
        /* wanted on (a switch off unlinks the file before it kills
         * anything), and wpa_supplicant gone from under it, or a chip that
         * has seen no network at all for half a minute: start again, less
         * often each time it does not help */
        if (wifi_state == 1 && access(WIFI_BOOT_FILE, F_OK) == 0 &&
            (!wifi_answers() ||
             (unjoined >= patience &&
              wifi_cli_int(WIFI_CLI "scan_results 2>/dev/null | grep -c :") <= 0)))
        {
            wifi_restart();
            unjoined = 0;
            if (patience < 120)
                patience *= 2;
        }
        usleep(5000000);
    }
    return NULL;
}

void hiby_wifi_boot_init(void)
{
    pthread_t worker;

    if (pthread_create(&worker, NULL, wifi_worker, NULL) == 0)
        pthread_detach(worker);
}

int hiby_wifi_state_fast(void)
{
    return wifi_state;
}

int hiby_wifi_menu(void)
{
    struct simplelist_info info;
    char ssid[WIFI_SSID_LEN], ip[16];
    bool on;

    while (1)
    {
        on = wifi_is_on();
        if (on)
            wifi_status(ssid, ip);
        else
            ssid[0] = ip[0] = '\0';

        simplelist_info_init(&info, (char *)str(LANG_WIFI), 0, NULL);
        info.action_callback = wifi_ok_cancel;
        info.selection = -1;
        simplelist_reset_lines();
        simplelist_addline("%s: %s", (const char *)str(LANG_WIFI),
                           (const char *)str(on ? LANG_ON : LANG_OFF));
        simplelist_addline("%s: %s", (const char *)str(LANG_WIFI_NETWORK),
                           ssid[0] ? ssid : (const char *)str(LANG_BT_DISCONNECTED));
        simplelist_addline("%s: %s", (const char *)str(LANG_WIFI_ADDRESS),
                           ip[0] ? ip : "-");
        simplelist_addline("%s", (const char *)str(LANG_WIFI_UPDATE));
        info.count = simplelist_get_line_count();
        simplelist_show_list(&info);

        if (info.selection < 0)
            break;
        if (info.selection == 0)
        {
            if (on)
                wifi_off();
            else
                wifi_on();
        }
        else if (info.selection == 1)
            wifi_show_networks();
        else if (info.selection == 3)
        {
            if (ip[0])
                wifi_update();
            else
                splash(HZ*2, ID2P(LANG_BT_DISCONNECTED));
        }
        /* the address line: just re-show */
    }
    return 0;
}

#endif /* HIBY_LINUX */
