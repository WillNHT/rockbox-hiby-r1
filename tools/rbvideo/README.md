# librbvideo — the video decoder

`librbvideo.so` is FFmpeg (and libwebp, for animated WebP) behind the
small API in [apps/video/rbvideo.h](../../apps/video/rbvideo.h). Rockbox
loads it at run time from `/.rockbox/lib/librbvideo.so` and never sees an
FFmpeg header, so the two can be built and updated separately.

## Build

```bash
tools/rbvideo/build.sh r1   OUTDIR    # for the player (mipsel toolchain on PATH)
tools/rbvideo/build.sh host OUTDIR    # for this machine, for the simulator
```

Sources (pinned: FFmpeg 8.0, libwebp 1.5.0) are downloaded into
`$RBV_DL` (default `~/.cache/rbvideo`) and built static; the result is one
`OUTDIR/librbvideo.so` with nothing to install beside it. Copy it to
`/.rockbox/lib/` on the card — the release does that for you in
`rockbox-hibyr1-video.zip`.

FFmpeg is configured for decoding only: the containers and codecs people
have video in (H.264, HEVC, MPEG-4, MPEG-1/2, VP8, VP9, Theora, WMV,
MJPEG, GIF, APNG, plus AAC, MP3, Vorbis, Opus, FLAC and AC-3 for sound),
no encoders, no network, no programs.

## How it behaves

- One operating-system thread per open stream, at `SCHED_IDLE` and nice
  19, with every signal blocked: Rockbox's own threads are user-space
  contexts switched on its timer signal, and that signal must not land in
  the decoder. Audio always wins the processor.
- Rockbox says what time it is (`set_clock`) and takes the newest frame
  that is due (`lock_frame` / `get_frame`). When the decoder falls behind
  it drops frames, then skips decoding the frames nothing refers to, then
  seeks. A clip too heavy for the player is a slow picture, never a gap in
  the music.
- Frames come out already scaled to the size the caller asked for, as
  RGB565 in the machine's byte order, cropped (`cover`), letterboxed
  (`contain`) or stretched.

## Licences

The build takes FFmpeg's default LGPL v2.1+ configuration (no
`--enable-gpl`, no GPL-only parts), and libwebp is BSD-licensed; both are
compatible with Rockbox's GPL v2+. The exact sources are the pinned
release tarballs above, and this script is the complete recipe used to
build them.
