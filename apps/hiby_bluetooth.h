#ifndef __HIBY_BLUETOOTH_H__
#define __HIBY_BLUETOOTH_H__

#include <stdbool.h>
#include <stddef.h>

int hiby_bluetooth_menu(void);
void hiby_debug_log(const char *format, ...);

void bt_boot_init(void);
bool bt_is_enabled_fast(void);
bool bt_is_starting_fast(void);
bool bt_is_connected_fast(void);
bool bt_is_dual_fast(void);
int bt_rx_state_fast(void);
int bt_rx_status_fast(void);
const char *bt_rx_info(int what, char *buf, size_t len);
void bt_rx_play_pause(void);
void bt_rx_skip(int dir);
void bt_rx_volume_step(int steps);
void bt_mix_changed(int value);
int bt_pending_event(void);
void bt_sync(void);
void bt_unroute(bool resume_later);
void bt_toggle_power(void);
const char *bt_qs_val(char *buf, size_t len);

#endif /* __HIBY_BLUETOOTH_H__ */
