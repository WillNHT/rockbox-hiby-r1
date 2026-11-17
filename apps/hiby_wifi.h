#ifndef __HIBY_WIFI_H__
#define __HIBY_WIFI_H__

#include <stdbool.h>

int hiby_wifi_menu(void);
/* Starts the worker that keeps the state below, and brings Wi-Fi back up
 * if it was on when the player was last switched off */
void hiby_wifi_boot_init(void);
/* For %WF: 0 off, 1 on, 2 joined to a network */
int hiby_wifi_state_fast(void);
/* Wi-Fi off while Bluetooth powers the chip they share; true if it was on,
 * and then hiby_wifi_resume() brings it back */
bool hiby_wifi_pause(void);
void hiby_wifi_resume(void);

#endif /* __HIBY_WIFI_H__ */
