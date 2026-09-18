---
title: Audio Prioritisation
---

A keyclick or a lock chirp is a few milliseconds of square wave mixed in on
top of whatever is playing. With music under it at anything like a normal
listening level the two fight for the same headroom and the device sound
loses - which is a problem, because the device sound is the one carrying
information the user asked for.

Audio prioritisation steps the music aside for the length of the sound. The
playback mixer channel drops by a set amount, the sound plays over the gap,
and the channel comes back a fraction of a second later.

## The settings

**Settings - General Settings - System - Audio Prioritisation**

| Setting | Default | What it does |
| --- | --- | --- |
| Audio Prioritisation | On | Master switch. Off means the music is never touched. |
| Click Depth | 25% | Keyclicks and the list edge beeps. |
| Alert Depth | 45% | Track skip, and the end of a playlist. |
| Cue Depth | 70% | Lock, unlock and the stick arming run. |

Three depths rather than one because the sounds are not equally important.
A keyclick happens on every press, so a deep duck would pump the music all
the way through a menu; a lock cue happens once and has to be heard with
the player already in a pocket.

Depth is how far the music drops, so 0% is no ducking at all and 90% is
nearly silent. The music returns 150 ms after the sound ends, which is long
enough that a run of chirps - the arming run, or the two-note lock cue -
holds one duck rather than flapping the volume between notes.

## In config.cfg

```
audio prioritization: on
audio prioritization click: 25
audio prioritization alert: 45
audio prioritization cue: 70
```

## Screenshots

| The menu | A depth |
| --- | --- |
| ![Audio Prioritisation menu](screenshots/audio-prioritisation-menu.png) | ![Click Depth](screenshots/audio-prioritisation-depth.png) |
