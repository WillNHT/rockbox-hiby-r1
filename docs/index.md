---
title: Rockbox for the HiBy R1
---

A Rockbox fork for the HiBy R1, built on top of
[bahusoid's Rockbox Audiobook Mod](https://github.com/bahusoid/rockbox/tree/Mod25.12.07).
The [README](https://github.com/WillNHT/rockbox-hiby-r1#readme) lists everything the
fork adds; the pages below are the per-feature documentation.

Grab a build from the [releases page](https://github.com/WillNHT/rockbox-hiby-r1/releases).

## Using the player

* [Rockpocket Stick](mods/rockpocket-stick.md) - the relative virtual stick: drag to
  scroll, flick for actions, hold for the volume dial.
* [Touch controls](mods/touch-controls.md) - what the touchscreen does outside the stick.
* [Keymap](mods/hibyr1-keymap.md) - the physical keys, and touchless navigation.
* [Key remapping](mods/keyremap.md) - changing those keys yourself.

## Library and playback

* [Library views and playlist covers](mods/library-views.md) - thumbnails, shelves,
  grid and carousel; `%Cp` in a WPS.
* [Audiobooks library](mods/audiobooks-library.md) - covers, series, narrators,
  progress, Continue Listening, per-book speed, sleep timer.
* [Audiobook Mod](mods/audiobook-mod.md) - the audiobook profile from the base mod.
* [Pseudo-radio](mods/pseudo-radio.md) - a folder of long recordings, entered
  part-way through.
* [Shared recent bookmarks](mods/shared-recent-bookmarks.md) - resume on another player
  from the same SD card.

## Look and feel

* [Transitions](mods/transitions.md) - animated menu and track changes.
* [Themes](https://github.com/WillNHT/rockbox-hiby-r1/tree/master/themes) - the Snappy
  family and `gen_skins.py`.

## Device internals

* [Bootloader changes](mods/hiby-bootloader-changes.md)
* [Bluetooth](mods/bluetooth-hiby-x1600.md)
* [Battery charge limit](mods/battery-protection-hiby-x1600.md)

## Contributing

* [CLAUDE.md](https://github.com/WillNHT/rockbox-hiby-r1/blob/master/CLAUDE.md) - how this
  repo is worked on, by people and by Claude.
* [Upstream Rockbox docs](https://www.rockbox.org/wiki/) for anything this fork does not change.
