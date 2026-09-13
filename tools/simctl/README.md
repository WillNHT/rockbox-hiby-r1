# Driving the simulator headlessly

Tier 2 of the test plan — run the real UI, do something to it, look at the
result — had been down. Two separate causes, both about going through X for
something that happens inside the process:

- `Xvfb` will not start in this WSL instance: `/tmp/.X11-unix` has the wrong
  mode and `/usr/bin/xkbcomp` is missing. Both need root.
- Capturing from WSLg's own server with `import` returns a **stale
  composite**. A plain `lcd_fillrect` probe pushed with its own
  `lcd_update_rect()` did not appear in the capture, so a screenshot proved
  nothing about drawing.

Neither of those is fixed here. They are bypassed.

## How it works

`uisimulator/common/sim_ctrl.c` opens a FIFO when `RBSIM_CTL` names a path,
and turns each line into a **synthetic SDL event**. The simulator's own event
loop then handles it exactly as it handles a real mouse or key press — there
is no second input path to keep honest, and a gesture test is genuinely
end to end.

Pictures come back through Rockbox's own `screen_dump()` (the `F5` handler),
which reads the LCD framebuffer rather than the window. Nothing about the X
server can lie about it.

The simulator still needs a display to start, and WSLg's `:0` provides one
perfectly well. It is only *capturing* from it that was broken.

## The command language

Coordinates are LCD coordinates — the same ones a `.wps` and the stick engine
use. `sim_ctrl.c` adds the window skin's offset itself.

```
down X Y            thumb down
move X Y            thumb moves (delivered as motion with button 1 held)
up [X Y]            thumb up, where it is unless told otherwise
key NAME            tap: press, 60 ms, release
key NAME down|up    half of a press, for holds
press NAME          same as "key NAME down"
release NAME        same as "key NAME up"
sleep MS            wait
dump                screenshot
quit                shut the simulator down
# ...               comment
```

`NAME` is one of `power`, `next`, `play`, `volup`, `voldown` — the project's
names for the five physical keys, not the SDL keys the simulator happens to
map them to, so a test keeps working if `uisimulator/buttonmap/hiby-r1.c` is
rearranged.

## Running one

```bash
tools/simctl/simctl.sh script.txt [outdir]
```

Screenshots land in `outdir` as `shot01.png`, `shot02.png`, … in the order
they were taken. Compare them with ImageMagick:

```bash
compare-im6.q16 -metric AE a.png b.png null:
```

## Two things that will bite you

- **The backlight times out.** Roughly two seconds into an idle script the
  panel goes dark and every later shot is a flat gradient. That is the device
  behaving correctly, not the harness failing. Keep scripts moving, or raise
  the backlight timeout in the simulator's `config.cfg`.
- **Always run a negative control.** Two `dump`s with nothing between them
  must differ by zero pixels. If they do not, something in the harness is
  lying and no result from that run means anything.

`screen_dump()` uses numbered filenames rather than timestamped ones in
simulator builds. Timestamps have one-second resolution, and a script taking
several shots a second silently lost all but the last of them — which looks
exactly like a screen that did not change.
