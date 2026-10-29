# Rendering overhaul: analysis and plan

Status: proposal. Supersedes #117, closes #42 when done, unblocks #116.

## What is wrong

### One root cause

Rockbox is immediate mode: every screen paints straight into one shared
framebuffer, and nothing owns the final frame. Each time that bit us, the fork
added another place pixels can live, with its own rule for transparency:

| # | Where pixels live | Transparency rule | Added for |
| --- | --- | --- | --- |
| 1 | Rockbox framebuffer | none, last write wins | upstream |
| 2 | LCD backdrop (`%VB`, `%Cb`) | a clear copies it back | glass over the cover |
| 3 | `%Vt` veil / `%dr` alpha (`skin_layer.c`) | blend into the clear, once | scrims, sheen |
| 4 | `lcd-layers.c` overlay | colour key `0x0001` | stick overlay (#2) |
| 5 | `canvas_glue.c` backing store | save and restore what was under | stick backdrop, HUD |
| 6 | `lcd-transition.c` | its own snapshot of the panel | track/page transitions |
| 7 | page-flip planes (`lcd-linuxfb.c`) | dirty-rect replay from #1 | tearing |
| 8 | #117 surfaces (unmerged) | colour key plus coverage plane | z order for viewports |

Meanwhile `apps/canvas.c` already has a real compositor - RGB565 surfaces with
an A8 alpha plane, layers, opacity, damage rects, blur, rounded rects, easing -
and it is not in the display path. It is used as a drawing library.

Every recurring bug is a seam between two of those rows: a viewport beneath
redraws a dynamic line and paints through one above it (1 vs 3), glass over a
viewport wipes it (2 vs 1), overlay ink left behind or flicker (4/5 vs 7), a
transition starting from the wrong frame (6 vs 7), chrome fading and popping
back on a track change (#103, #112: 2 vs 6), a cover edge lost (#134). Fixing
one seam moves the pressure to the next, which is why the same handful of
issues come back.

### The skin language works around the engine

`SnappyAnimated.wps` is 1,098 generated lines: 2,440 `%dr`, 1,333 `%Vd`, 584
`%Vt(0)`. Its own header says it: *"The skin engine cannot move a viewport, so
anything that travels is one viewport per stop with `%an` choosing which is
enabled."* `gen_skins.py` (1,093 lines, the most-churned theme file) exists to
emit that. A skin author cannot say "this box is 60% opaque, slides in 40 px,
over whatever is beneath" - so every requirement is approximated, and the
approximation is checked on the device.

### Why local testing does not predict the device

* The simulator has no page flipping, no vsync and an x86 CPU: anything that
  depends on present timing (flicker, frame drops, transitions, the sheen at
  20 fps) cannot show up there.
* Simulator screenshots read the framebuffer plus the layers it composes, not
  what the device's back plane holds after a replay. Rows 6-7 are invisible.
* There are no reference images. Nobody can say "this is what correct looks
  like" before the flash, so the first flash is where the requirement is
  discovered, and the second and third are where it is met.
* Nothing compares a build to the last good one, so regressions are found by
  eye, on the device, later.

## What to build

### 1. One compositor that owns the frame

Promote `apps/canvas.c` into the present path and retire rows 2-8.

* A **layer** is a rectangle, RGB565 pixels, an A8 alpha plane, opacity, an
  x/y offset, a z band and a visible flag. No colour key anywhere.
* Z bands, bottom to top: base (menus, plugins, every upstream screen, i.e.
  today's framebuffer, untouched), skin backdrop, skin viewports in declaration
  order, status bar, splash/modal, transition, stick overlay.
* **Every** skin viewport is a layer, not only ones that overlap. A viewport
  draws through the existing `lcd_*` API into its layer's buffer - the same
  `frame_buffer_t` redirect `lcd-layers.c` and #117 already prove works.
  Clearing sets alpha to 0. Background colour plus `%Vt(n)` is a fill at
  n% alpha. Glyph edges write coverage into alpha, so text antialiases against
  whatever ends up beneath it, with no fringe.
* **Present**: for each damage rect, compose bottom to top straight into the
  panel back plane, starting at the topmost opaque layer that covers the rect.
  No copy-out/put-back of the framebuffer, no replay bookkeeping: both planes
  are recomposed from layers, which are the truth.
* Moving or fading a layer changes its offset or opacity and damages the old
  and new rects. No repaint of the layer's content. That is what makes real
  animation cheap.
* Screen dumps, transitions and the simulator all read the composed result,
  from the same function the panel gets.

Memory: 3 bytes/px, allocated per layer at its own size. Snappy Animated is a
full-screen backdrop (1.1 MB) plus small layers, roughly 1.5-2 MB in all,
against today's 768 KB layer + 768 KB saved copy + 768 KB canvas pool +
#117's 1.1 MB. Cost per frame is what has to be measured on the R1 first (see
phase 1); the existing present stats (`HAVE_LCD_PRESENT_STATS`) already time
copy and present.

### 2. Skin tags that say what the author means

With layers, the missing CSS properties are a few tags each:

| CSS | Tag | Today |
| --- | --- | --- |
| `background: rgba()` | `%Vt(n)` with live alpha | veil baked into backdrop |
| `opacity` | `%Vo(n)` | not possible |
| `transform: translate()` | `%Vx(dx,dy)` | one viewport per stop |
| `transition: x 200ms ease` | `%Va(prop,ms,ease)` via `canvas_anim` | `%an` frame counters |
| `z-index` | declaration order, live | full redraw only |

Snappy Animated's "march", "dial border" and "hide while armed" helpers in
`gen_skins.py` become a handful of lines each, or disappear.

### 3. #116: HTML/CSS that compiles to `.wps`

Once tags map 1:1 to CSS properties, a compiler from a strict HTML/CSS subset
to `.wps` is a translation, not an emulation. Built last, on top of phases 1-2.
`gen_skins.py` is the first thing ported to it.

### 4. Change the loop, not only the code

* **Reference images first.** For a visual issue, the first artifact is a
  simctl script and the screenshots it should produce - mocked if the code
  does not exist yet. The human approves the picture in the PR *before*
  anything is flashed. Requirement mismatches (pain 3) are caught in the
  simulator, at the cost of a comment, not a flash.
* **Golden-image regression in CI.** Every skin x {menu, WPS, WPS with
  overlay, transition mid-frame, status bar} is a simctl script with committed
  PNGs. The `simulator` job diffs against them and fails on change; an
  intended change updates the goldens in the same PR, so the diff shows the
  visual change for review. This is what stops pain 4.
* **Device frames, not descriptions.** A key combo saves the composed frame
  and present stats to the SD card. The human pastes the PNG and the frame
  time instead of describing what is wrong, and it is compared with the
  golden for the same state. Closes the gap in pain 2.
* **Timing in the simulator.** The simulator presents through the same
  double-buffered path with a fixed-rate vsync, so ordering bugs (row 7) show
  there. CPU cost still needs the device: phase 1 measures it once.

## Plan

Each phase is one PR and at most one flash.

| Phase | Lands | Flash | Exit |
| --- | --- | --- | --- |
| 0 | Golden harness, baseline shots of every skin as it is now, device frame dump | none for harness; dump rides with phase 1 | CI diffs pass on master |
| 1 | Compositor in the present path; base + stick overlay ported; rows 4, 5, 7 gone; old/new behind a debug toggle | 1 | Frame time on R1 within budget (target: full-screen recompose under 16 ms, typical damage under 4 ms), stick overlay unchanged |
| 2 | Skin viewports as layers; `%Vt` live; rows 2, 3, 6 gone; toggle removed | 1 | Goldens identical except where glass/scrim is now right; #42, #103, #112 verified; #117 closed |
| 3 | `%Vo`, `%Vx`, `%Va`; Snappy Animated and transitions ported; `gen_skins.py` shrinks | 1 | Animated skins match goldens, frame time holds |
| 4 | HTML/CSS compiler (#116) | none (compiles to the same tags) | Snappy V2 rebuilt from HTML/CSS, goldens identical |

Phase 1 is the go/no-go: if the R1 cannot recompose fast enough, the fallback
is to keep the framebuffer as base and compose only skin layers (closer to
#117, but with alpha instead of a key), and that is decided on measured
numbers, not guessed.

## What #117 got right, and why not merge it

Right: redirecting a viewport's drawing into a retained surface, composing at
present time, keeping glyph coverage instead of blending to the key, and its
`layering.txt` simctl test. All of that carries over.

Not merged as is because it adds an eighth mechanism instead of replacing
seven: only *overlapping* viewports are lifted (so behaviour depends on
geometry), transparency is a colour key plus a separate coverage plane, the
backdrop trick and the overlay layer stay, and every update still copies the
framebuffer out and back. It moves the seams; it does not remove them.
