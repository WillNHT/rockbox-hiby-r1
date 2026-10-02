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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel.h"
#include "action.h"
#include "keyboard.h"
#include "misc.h"
#include "splash.h"
#include "yesno.h"
#include "gui/list.h"

/* Wi-Fi brought up the way HiBy OS does it (wifi_on.sh): the driver is
 * loaded at boot, wpa_supplicant joins a network, udhcpc gets an address.
 * Off until switched on - the radio is the biggest battery cost the player
 * has - and nothing listens: Rockbox only ever talks out. Networks and
 * passwords are kept by wpa_supplicant itself, in wifi.conf. */
#define WIFI_IF        "wlan0"
#define WIFI_CONF      PIVOT_ROOT ROCKBOX_DIR "/wifi.conf"
#define WIFI_CTRL      "/var/run/wpa_supplicant/" WIFI_IF
#define WIFI_CLI       "wpa_cli -i " WIFI_IF " "
#define WIFI_MAX_NETS  32
#define WIFI_SSID_LEN  132  /* 32 bytes, escaped the way wpa_cli prints them */
#define WIFI_OUT_OF_RANGE (-1000)

struct wifi_net
{
    char ssid[WIFI_SSID_LEN];
    int signal;     /* dBm, WIFI_OUT_OF_RANGE for a saved one not seen */
    int id;         /* saved network id, -1 when not saved */
    bool secured;
};

static struct wifi_net wifi_nets[WIFI_MAX_NETS];
static int wifi_net_count;

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

static bool wifi_on(void)
{
    int i;

    if (wifi_is_on())
        return true;
    splash(0, ID2P(LANG_WAIT));
    system("[ -f " WIFI_CONF " ] || printf 'ctrl_interface=/var/run/wpa_supplicant\\n"
           "update_config=1\\n' > " WIFI_CONF);
    system("rfkill unblock wifi; ifconfig " WIFI_IF " up && wpa_supplicant -B -D nl80211 -i " WIFI_IF
           " -c " WIFI_CONF " >/dev/null 2>&1");
    for (i = 0; i < 30 && !wifi_is_on(); i++)
        sleep(HZ/10);
    if (!wifi_is_on())
    {
        splash(HZ*2, ID2P(LANG_WIFI_UNAVAILABLE));
        return false;
    }
    /* a saved network is joined on its own */
    wifi_dhcp();
    return true;
}

static void wifi_off(void)
{
    int i;

    system("killall udhcpc wpa_supplicant >/dev/null 2>&1; ifconfig " WIFI_IF " down");
    for (i = 0; i < 10 && wifi_is_on(); i++)
        sleep(HZ/10);
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
    /* select_network turned the others off: they may join again later */
    system(WIFI_CLI "enable_network all >/dev/null");
    system(WIFI_CLI "save_config >/dev/null");

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

static int wifi_ok_cancel(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
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
        /* the address line: just re-show */
    }
    return 0;
}

#endif /* HIBY_LINUX */
