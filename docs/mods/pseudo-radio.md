---
title: Pseudo-Radio
---

The R1 has no tuner, so this is not one. It is a folder of long recordings
and a way of dropping into one part-way through, which turns out to be most
of what makes a radio feel like a radio: what you hear is already underway,
and you did not pick it.

Tuning in picks a recording at random and starts it at a random point. There
is no station database and no scan - the folder tree is the dial.

## Tuning in

The **radio** entry in the main menu does not open a list. It puts you back
on the station you were on last, or picks one when there is no last - a
radio hands you sound, not a menu. The station list is what you get by
coming back to the entry from a station that is already playing, and that is
also what stops it from tuning again on the way out.

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

Pausing does not stop the station. Come back after more than half a minute
and playback drops in as far along as the time you were away, wrapping round
the recording - so a pause over lunch returns to a different part of the
programme, not to the syllable you left on. Shorter pauses resume where they
were, because a phone call is not an afternoon. This is measured from the
running clock, so a pause across a power cycle is not counted.

### Station cover

A station's artwork is `cover.jpg` (or `.png`/`.bmp`) in the station folder:

```
/radio/Late Night/cover.jpg
```

It is used for every file in the station, however many subfolders deep the
file itself sits, and it wins over the ordinary album-art search. Without one
the normal album-art rules apply.

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

### Tuning Static

Every transition the dial makes has its own noise, because a radio you
cannot hear yourself operating is a file player with different words on it.
There are five:

| When | What it sounds like |
| --- | --- |
| Tuning in | hiss swelling up, with a carrier sliding into place |
| Prev/next | shorter and harsher, a whistle sweeping past everything between |
| Pause | the carrier falling away underneath |
| Resume | the same, rising |
| Leaving | a low thump, the sound a set makes going off |

They go through the same mixer channel as the keyclick and duck the music
the same way, at the Cue depth in
[Audio Prioritisation](audio-prioritisation.md).

All of them are generated rather than played from a file. The beep channel
takes raw PCM with no decoder behind it, so a set of sound files would mean
a WAV reader, five more files that have to be on the card, and a card read
on a UI event - for noises that are three numbers each. **Static Strength**
scales all of them together, from a tenth to twice; **Tuning Static** turns
the lot off.

### Band Noise

Every hour or two, something drifts past on its own: a station passing
behind this one, a moment of interference, a signal fading out and back, a
squeal from somewhere adjacent, distant weather on the band. One of five, at
a random gap of 75 minutes give or take 45, and ducked shallower than a cue
because it is meant to sound like it is coming *through* the programme
rather than instead of it. **Band Noise** turns it off.

### The station cover

A station's artwork is `cover.jpg` (or `.png`/`.bmp`) in the station folder.
See [Station cover](#station-cover) above.

## A radio WPS

**Radio WPS** in the settings picks a skin used only while a station is
playing. `SnappyRadio` ships with the theme family: Snappy Animated with
the parts that do not apply to a radio taken out - no elapsed, no total, no
"3 of 52" - and the station where the album would be. Its big line is the
station name, its small line is whatever file is on, and its footer says
`on air` or `paused`.

The station name comes from `%rs`, a skin tag this fork adds. It is the
station folder's name, and it is empty when what is playing is not a
station track, so `%?rs<...|...>` is how any skin can tell it is on the
radio at all.

## In config.cfg

```
radio folder path: /radio
radio last station:
radio minimum length: 10
radio static: on
radio static strength: 100
radio band noise: on
radio wps:
radio screensaver:
```

## Screenshots

| Stations | Tuned in, part-way through | Settings |
| --- | --- | --- |
| ![Station list](screenshots/radio-stations.png) | ![Playing](screenshots/radio-wps.png) | ![Settings](screenshots/radio-settings.png) |

| The radio WPS | The menu entry |
| --- | --- |
| ![SnappyRadio](screenshots/radio-skin.png) | ![Radio icon](screenshots/radio-icon.png) |

The station in that shot is `/radio/Late Night`, the file on air is three
folders below it, and the cover is the station's - `cover.jpg` next to the
station folder, not next to the file, and not the artwork the file happens
to embed.
