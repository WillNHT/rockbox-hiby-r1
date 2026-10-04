# Bluetooth Support for Hosted Hiby Rockbox Build

This build integrates [bidhata's patches](https://github.com/bidhata/hiby-r1-rockbox-bt) and drives the standard BlueZ (`bluetoothctl`) and bluealsa (`bluealsa-cli`) tools instead of HiBy's proprietary `sys_server`.

## Using it

Everything is on one screen, **Main Menu → Bluetooth**, laid out like a phone's:

*   **Bluetooth: On / Off** - select to switch. Turning it on reconnects the last headset that played.
*   **Paired Devices** - select one to connect. A connected one shows `(Connected, 80%)` when the headset reports its battery; select it, or long-press any paired device, for its page.
*   **Other Devices** - found while the screen is open (`Searching...`). Select one to pair and connect. A device that shows a code asks you to confirm it; one that wants a PIN gets `0000`.
*   **Forget All Devices**, **Wired Sync Offset** - at the bottom.

![The Bluetooth screen](screenshots/bluetooth-screen.png)
![A connected headset's page](screenshots/bluetooth-device.png)

A device's page shows its status, codec, sample rate and battery, and has **Connect/Disconnect** and **Forget This Device**. Select the codec to change it; playback pauses for the switch and carries on afterwards.

The quickscreen's **Sound** page has a Bluetooth on/off toggle in its bottom slot.

Headset buttons work as media keys, and the headset's own volume buttons move the player's volume. Earbuds put back in the case and taken out again reconnect on their own.

## How it behaves

*   Nothing Bluetooth does blocks the UI: the commands run on a thread of their own and the screen updates as they finish.
*   When the headset goes away (switched off, out of range), playback pauses and the jack takes over, as on a phone. A headset that disappears in the middle of a route falls back to the jack instead of crashing.
*   Bluetooth comes back on after a reboot if it was on. The stock bootloader suspends the stack at boot; turning Bluetooth on resumes it, so the custom bootloader is no longer required, only faster.

## Debug log

Nothing is logged by default. Create an empty `rockbox-bt-debug.log` at the root of the SD card to switch logging on; it is cut back once it passes 512 KB.
