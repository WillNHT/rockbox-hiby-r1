---
title: Pseudo-Radio
---

The R1 has no tuner, so this is not one. It is a folder of long recordings
and a way of dropping into one part-way through, which turns out to be most
of what makes a radio feel like a radio: what you hear is already underway,
and you did not pick it.

Tuning in picks a recording at random and starts it at a random point. There
is no station database and no scan - the folder tree is the dial.

## Stations

Stations are the immediate subfolders of the radio folder, and the tracks
sit directly inside a station:

```
/radio
  /Late Night
     set-01.mp3
     set-02.mp3
  /Field Tapes
     harbour.flac
  /Long Sets
     ...
```

A radio folder with no subfolders is one station, so the simplest setup is a
folder with files in it and nothing else to configure. **Any Station** at the
top of the list picks a station for you first, then tunes it.

A station is everything under its folder, any number of levels deep, so a
station can be organised into its own subfolders without breaking scanning.

## While tuned in

Prev/next moves the dial instead of stepping through the station's files -
a radio does not let you skip to the next track of the thing playing, it
switches you to something else. A station with more than one file also
plays them in a shuffled order rather than always the same one.

## Settings

**Main Menu - Radio - Radio Settings**

| Setting | Default | What it does |
| --- | --- | --- |
| Radio Folder | `/radio` | Where the stations are. |
| Minimum Length | 10 min | How long a recording must be to be preferred. |
| Tuning Static | On | The hiss played while tuning in. |
| Radio WPS | Same as Music | The WPS used while a station plays. |
| Radio Screensaver | Same as usual | The screensaver used while a station plays. |

Point the radio folder somewhere else the same way as the audiobooks folder:
the folder's context menu in Files, under **Set As**, or from the main menu
layout editor.

### Minimum Length

A three minute pop song entered at 60% is a song with its first minute cut
off. An hour-long set entered at 60% is a radio. The floor is what keeps the
difference. Ten minutes by default, which is low enough to let a long album
track or a podcast episode count and high enough to keep singles out.

It is a preference rather than a filter: tuning reads the tags of a handful
of randomly chosen files and takes the first one over the floor, so a station
holding nothing long enough still plays something instead of refusing. Set it
to Off to take whatever comes.

Reading tags for a handful of candidates is also why a large radio folder
costs nothing to tune - there is no up-front scan of the folder.

### Where it drops in

Uniformly over the whole recording except the last minute. A radio has no
reason to prefer the beginning, and the beginning is the one part you could
have had by pressing play.

The generator is seeded from the tick on every tune. Rockbox seeds `rand()`
with a constant, so without that every boot would tune to the same track at
the same second - which is the one thing a radio must not do.

### The static

A short burst of white noise over the gap between the station list and the
first sample - the gap it exists to cover. Tuning in while something else is
still playing is the ordinary case, so the static goes through the same mixer
channel as the keyclick and ducks the music the same way, at the Cue depth in
[Audio Prioritisation](audio-prioritisation.md).

## In config.cfg

```
radio folder path: /radio
radio minimum length: 10
radio static: on
radio wps:
radio screensaver:
```

## Screenshots

| Stations | Tuned in, part-way through | Settings |
| --- | --- | --- |
| ![Station list](screenshots/radio-stations.png) | ![Playing](screenshots/radio-wps.png) | ![Settings](screenshots/radio-settings.png) |
