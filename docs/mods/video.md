# Video: animated covers, music videos, screensavers and a player

Four things that move, all from one decoder:

| | |
|---|---|
| **Animated covers (canvas)** | A short clip loops where the album art is, while the track plays. |
| **Music videos** | The track's video, silent, following the track's own clock. The sound is always the track: switching between the video and the cover changes only the picture. |
| **Screensavers** | Skins (`.ss`) that come up when the player is left alone — one for battery, one for charging. |
| **Video player** | Tap a video file and watch it, with sound. |

Everything here is off until you turn it on in **Settings > Video**, and
everything needs the decoder installed.

## Install the decoder

The decoder is a separate download because it is a few megabytes:
**`rockbox-hibyr1-video.zip`** from the release (the `…-sdcard.zip`
bundle already contains it). Unzip it over the root of the card; it puts
`/.rockbox/lib/librbvideo.so` there. **Settings > Video > Decoder** says
which version is installed, or that it is missing.

It is FFmpeg and libwebp: H.264, HEVC, MPEG-4, MPEG-1/2, VP8, VP9,
Theora, WMV and MJPEG, in MP4, MKV, WebM, FLV, AVI, MOV, MPEG-TS and
MPEG-PS, plus animated GIF, WebP and APNG. See
[tools/rbvideo/README.md](../../tools/rbvideo/README.md).

## Settings > Video

| Setting | What it does |
|---|---|
| Video | The master switch. Off by default; nothing decodes while it is off. |
| Animated Covers | Canvas clips in a WPS that asks for them. |
| Music Videos | Music videos in a WPS that asks for them. |
| Video Latency | Holds the picture back, in ms, to match the sound. |
| Bluetooth Latency | Added while Bluetooth is playing (LDAC and AAC are around 200 ms). |
| Frame Rate on Battery | Caps the frames a second while unplugged. `Full` for no cap. |
| Screensaver ▸ | See below. |
| Decoder | Which decoder is installed. |

## Files next to the music

Nothing is scanned or indexed: the player looks for a clip beside the
track when it needs one, in this order.

```
Artist/Album/cover.jpg                 the cover (as before)
Artist/Album/<track>.video.mp4         this track's music video (silent)
Artist/Album/<track>.video.cfg         how it lines up with the track
Artist/Album/<track>.canvas.mp4        this track's animated cover
Artist/Album/canvas.mp4                the album's animated cover
Artist/Album/cover.gif                 an animated cover, as an image
Artist/Album/cover.webp
```

Any extension the decoder knows works in place of `.mp4` (`.mkv`,
`.webm`, `.mov`, `.flv`, `.avi`, `.mpg`, and `.gif`, `.webp`, `.apng`
for the canvas). Files named `*.video.*` and `*.canvas.*`, and an
album's `canvas.*`, are hidden from the file browser and left out of the
database: they belong to the track, they are not tracks.

### Lining a music video up

A music video is often cut from a different master than the album track,
so it gets a sidecar:

```
# <track>.video.cfg
offset_ms = 1240      # the video starts 1.24 s after the track
rate      = 1.0       # video speed against the track's (drift)
start_ms  = 0         # trim at the start of the video
end_ms    = 0         # trim at its end, 0 = none
outside   = cover     # outside the video: cover, or hold the end frame
```

The position shown is `(track elapsed − latency − offset_ms) × rate`,
and outside that range the WPS shows the cover again (or holds the first
or last frame with `outside = hold`). Seeking or skipping in the track
moves the video with it.

`tools/r1-transcode.py` writes this file for you — it cross-correlates
the video's own sound with the track's:

```bash
tools/r1-transcode.py video "Take On Me (video).mp4" --track "03 Take On Me.flac"
tools/r1-transcode.py video CLIP --track SONG --check-drift   # also measure rate
tools/r1-transcode.py canvas CLIP --track SONG                # a canvas
tools/r1-transcode.py clip   CLIP                             # for a screensaver
```

It needs `ffmpeg` and `ffprobe`, and prints how confident the match is;
under 0.3, check it by ear or pass `--offset-ms`. Converting is optional
(the player decodes most files as they are) but worth it: a 1 GHz MIPS
core with no video hardware plays 480 px comfortably and 1080 px badly.

### Switching between the video and the cover

The WPS context menu (long Power in the WPS) has **Show Music Video** /
**Show Cover** while the playing track has one. The track's sound never
stops or restarts — the video has no sound of its own.

## Skin tags

| Tag | What it does |
|---|---|
| `%Cv(x, y, w, h [, cover\|contain\|stretch])` | The playing track's moving picture: its music video when Show Music Video is on, else its animated cover. Draws nothing when there is none. |
| `%?CV<canvas\|music video\|none>` | Which one `%Cv` would show. `%?CV<yes\|no>` works too. |
| `%Cf(x, y, w, h, file [, fit])` | A clip from the skin's own folder, looping. For screensavers. |
| `%Cp(x, y, size)` | The playing playlist's cover; an animated `.gif`/`.webp` one moves. |

Notes:

- A viewport holding `%Cv` or `%Cf` is redrawn many times a second, so
  keep it to itself: anything drawn over it disappears under the next
  frame, and anything that clears its rectangle (Snappy Animated's
  sheen, for instance) rubs the picture out. The demo themes turn that
  sheen off.
- `%Cv` draws nothing when the track has no clip, so put it in a
  viewport enabled by `%?CV` and let the cover show underneath.
- A picture is decoded only while it is being drawn: leave the WPS, or
  cover the viewport, and the decoder goes to sleep.

## Screensavers

A screensaver is a skin like a WPS, with the extension `.ss`, in
`/.rockbox/wps`; its pictures live in the folder of the same name. It
comes up after **Start After** seconds with no input, from whatever
screen is up, and any key or touch ends it (that press does nothing
else).

| Setting | |
|---|---|
| On Battery / While Charging | Which saver to show, or Off. |
| Start After | Idle seconds, or Off. |
| Frame Rate on Battery | Caps the frames a second while unplugged. |
| Brightness on Battery | The panel's brightness while the saver is up. |
| Keep Screen On | Never / While Charging / Always. "Never" lets the backlight dim and go off as usual. |
| Preview | Show the chosen one now. |

Three are shipped:

| Saver | |
|---|---|
| Saver Clock Dim | The time and the track in grey on black; the clock moves every half minute. For the battery. |
| Saver Canvas | The playing track's music video or animated cover, large, with the time under it. For the charger. |
| Saver Drift | A looping clip of its own (`%Cf`), for when nothing is playing. |

A saver can use any WPS tag, so a clock, the cover, the track, a peak
meter or `%Cv` are all available.

## The video player

Tapping a video file plays it: the player asks the decoder whether an
`.mp4` holds a picture, and hands it to the video player rather than the
audio codec if it does. Other extensions (`.mkv`, `.webm`, `.flv`, …)
open it directly, and `Open with` always offers it.

| Key | |
|---|---|
| Power | Play / pause |
| Long Power | Stop |
| Play / Next | Back / forward 10 s |
| Vol Up / Down | Volume |
| Power + Next | Turn the picture (a wide clip starts sideways) |
| Power + Play | How it fits: contain, fill, stretch |

Music playback stops while a video plays — one sound at a time.

## Demo theme

**Snappy Canvas** is Snappy Animated with the track's canvas (or its
music video) where the cover is. It turns video on, and it needs the
decoder installed.

## What to expect from the hardware

The X1600E has one ~1 GHz MIPS core and no video decoder of its own, and
it is also playing your music. In practice:

- 480×480 or 480×270 H.264 at 24–30 fps: comfortable, alongside FLAC.
- 480×800 H.264 at 30 fps: usually fine, tighter over Bluetooth.
- 720p and above, HEVC, VP9: they decode, slowly. Convert instead.
- Animated GIF and WebP: cheap at cover size, expensive full screen.

The decoder runs at the lowest priority the kernel has, so when it cannot
keep up the picture slows or freezes and the music carries on. `Frame
Rate on Battery` is there to trade frames for battery on purpose.

## For developers

- `apps/video/video_lib.c` loads `librbvideo.so` (through the same
  `lc_open` the plugins use, so paths work on the device and in the
  simulator) and answers what a file is.
- `apps/video/video_surface.c` is one moving picture of a fixed size: a
  stream, a clock, and a blit of the newest frame.
- `apps/video/video_art.c` picks what the WPS shows for the playing
  track (`%Cv`, `%?CV`, `%Cf`), reads the `.video.cfg` sidecar and
  applies the latency.
- `apps/video/screensaver.c` is the idle timer, the input filter and the
  loop that draws the `.ss` skin; the skin engine has a new skinnable
  screen for it (`SCREENSAVER_SKIN`).
- `apps/plugins/videoplayer.c` is the standalone player: the same
  decoder, plus the PCM mixer for the sound.
- `tools/rbvideo/` is the decoder itself, and `tools/r1-transcode.py`
  the converter.
