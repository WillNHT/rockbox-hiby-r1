# Wi-Fi

Main Menu > **Wi-Fi** brings the R1's radio up the way HiBy OS does: the driver is
already loaded at boot, `wpa_supplicant` joins a network and `udhcpc` asks it for an
address. Nothing listens on the network - Rockbox only ever talks out.

* **Wi-Fi: On/Off** - select to switch it. It starts off on every boot: the radio
  is the biggest battery cost the player has, so leave it off when nothing needs it.
* **Network** - select to scan. Pick a network to join; a secured one asks for its
  password on the keyboard. Saved networks are starred and are joined on their own
  next time Wi-Fi comes on. Long-press a saved one to forget it. A password that
  fails is not kept.
* **IP Address** - what the network gave the player.

Networks and passwords are kept by `wpa_supplicant` in `/.rockbox/wifi.conf`, in plain
text, like HiBy OS keeps its own.

If your main menu has a custom order (`root menu order` in `config.cfg`), add Wi-Fi to
it from Settings > General Settings > Main Menu Layout.

Nothing uses the connection yet: this is the base for internet radio, clock sync and
the rest of [#29](https://github.com/WillNHT/rockbox-hiby-r1/issues/29).
