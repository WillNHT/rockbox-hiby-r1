# Bootloader Changes

## 1. Modified Keymap for Hiby R1
Button controls are aligned with my [Rockbox keymap](hibyr1-keymap.md).
* **Vol Up/Down** - Move Up / Down
* **Play** - Go Back
* **Next/Power** - Select

## 2. Bluetooth State Persistence
Bluetooth state is preserved across Rockbox reboots (checks for `.rockbox/rb_bt_on.txt` file presence).

## 3. Wi-Fi Left With Its Driver
The R1's Wi-Fi chip is no longer unbound from its driver before Rockbox starts - the driver cannot be bound again afterwards, which left [Wi-Fi](wifi.md) with no `wlan0`. The chip stays powered off until Wi-Fi is switched on.

## 4. Boot Picture
`r1.upt` (the patcher's `r1_rb.upt`, renamed for the release) is built on a modded stock image whose own boot picture (a hat and sunglasses) showed before Rockbox's logo. `tools/r1_patcher` now puts `tools/r1_patcher/logo.jpeg` in its place - a 480x800 baseline JPEG, the Rockbox logo on the theme's ground. Replace that file and rebuild `r1.upt` to use another.
