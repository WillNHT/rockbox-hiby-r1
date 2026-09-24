# Rockpocket Stick

A relative virtual stick for the HiBy R1's touchscreen. Press anywhere in the
arm zone, push in a direction, release. The direction resolves to one of N
equal sectors, each sector carries its own binding, and the gesture is only
committed at release — so it stays correctable mid-flight, and sliding out of
the work zone cancels it entirely.

It is **off by default**. With `Touch Navigation` set to `Classic` the engine
is not in the input path at all, and the device behaves exactly as it did
before this feature existed.

---

## Turning it on

*Settings → General Settings → Display → Touchscreen Settings → Rockpocket Stick*

| Setting | Meaning |
|---|---|
| **Touch Navigation** | `Classic` (default), `Stick`, or `Both` |
| **Arm Zone** | Where a press is allowed to start a gesture: whole screen, bottom half, or a thumb plate |
| **Sectors** | 1–8. At 1 the whole disc is a single sector: hold-only, no direction |
| **Rose Rotation** | Rotates the sector rose. Sector 0 is always centred on this angle |
| **Arming Window** | 0–400 ms. A window, not a dwell — see below |
| **Binding Table** | `4-way`, `8-way`, or `Custom` from `/.rockbox/stick.cfg` |
| **Volume Dial in WPS** | The armed area becomes a volume dial while the WPS is up |
| **Scroll Dial in Lists** | The armed area becomes a list-scroll dial |
| **Degrees Per Detent** | 8–30. How far the dial turns per step |

### Touch Navigation modes

- **Classic** — edge swipes, kinetic scrolling and header taps, exactly as
  documented in [Touch Controls](touch-controls.md). The stick is entirely
  absent.
- **Stick** — the stick owns the arm zone. With the arm zone set to the whole
  screen, that means edge swipes and kinetic scrolling are off; that is the
  trade, and it is deliberate.
- **Both** — the stick arms only clear of the regions the classic scheme owns:
  the screen edges where swipes start, and the strip at the top holding the
  status bar and the list header. Presses there still reach the classic
  handlers, so both schemes work at once.

`Both` is the setting to start with.

---

## The gesture

```
press ──┬── in a dead zone ─────────────► dropped entirely
        ├── outside the arm zone ───────► plain absolute touch, for its whole life
        └── inside the arm zone ────────► arming
                                            │
                    release, no travel ◄────┤────► release with travel
                    = a plain tap           │      = the gesture resolves
                                            ▼
                                    window closes: armed
                                            │
                    leaves the work zone ◄──┤──► release
                    = cancelled, nothing    │    = the sector under the thumb fires
                      is committed          │
                    re-entry resumes  ──────┘
```

The **arming window is a window, not a dwell**. Movement during it accrues
normally from the touch point; output simply begins when the window closes.
Nothing is discarded and the thumb never has to hold still. A release inside
the window with no travel is a plain tap, hit-tested at the press point; a
release inside it *with* travel is evaluated once on the way out, so a fast
flick still registers.

Inside an 18 px detent radius around the press point no sector is selected. A
release there, after the window has closed, fires the **centre** binding.

### The dial

When a screen is configured as a dial, the whole armed area becomes one and
the sector table is ignored there. Angle is measured about the arm zone's
centre — not the touch point — and only outside a 34 px radius, which is the
jitter floor that stops a resting thumb producing phantom steps.

Angle accumulates unwrapped, so **re-circling is unlimited**: three loops in
one press is a hundred-odd detents. Release commits. Dragging outside the
work zone cancels the whole spin and reverts to the value it had when the
gesture armed — not to the last detent.

---

## What a binding actually does

The engine never invents an action. It emits the **same button code a
physical key would have produced**, and the ordinary keymap decides what that
means on the current screen. Three things follow from that:

1. Bindings are context-sensitive for free — `select` opens an item in a list
   and skips a track in the WPS, because that is what the key does.
2. Key remapping ([Key Remapping](keyremap.md)) keeps working.
3. The stick cannot reach anything a key cannot, which bounds a bad
   configuration to "unusable stick", never "unusable device".

| Binding | Key emitted | In lists | In the WPS |
|---|---|---|---|
| `scrollUp` | `UP` | previous item | volume up |
| `scrollDown` | `DOWN` | next item | volume down |
| `select` | `RIGHT` + release | open | skip next |
| `back` | `LEFT` + release | back / cancel | skip previous |
| `playPause` | `PLAY` + release | cancel | play / pause |
| `next` | `NEXT` + release | scroll | skip next |
| `prev` | `PREV` + release | scroll | skip previous |
| `volUp` / `volDown` | `UP` / `DOWN` + repeat | scroll repeat | volume |
| `none` | — | inert; widens its neighbours |

Because the meanings come from one shared keymap, some bindings collapse onto
the same key in some screens. Per-context binding tables are the fix, and are
the next piece of work; they are not in this change.

In the quickscreen and the pitchscreen, up and down are one press per
gesture: a flick fires once on release and a hold fires once. There every
press is a choice rather than a row, and the scroll's one-press-per-step
turned a single flick into ten choices in a row.

Literal `go:<screen>` bindings from the design spec are deliberately absent
for the same reason — there is no unambiguous key equivalent for them on this
device, and inventing one would break the guarantee above.

### Repeat rate

Repeating bindings fire at `2 + 14 × deflection²` per second after a 260 ms
delay, and release adds nothing. Scroll bindings deliberately emit discrete
presses rather than repeat-flagged ones, which keeps Rockbox's own list
acceleration out of the path so the stick alone owns the rate.

---

## Custom binding tables

Set **Binding Table** to `Custom` and create `/.rockbox/stick.cfg`:

```
# sector 0 is up, and they run clockwise
sectors 6
rotation 0
centre playPause

bind 0 scrollUp
bind 1 select
bind 2 next
bind 3 scrollDown
bind 4 prev
bind 5 back

# zones: "box l t w h" or "circle cx cy r"
arm  box 0 400 480 400
work box 0 0 480 800
dead box 0 0 480 34

armMs 110
degPerDetent 14
```

Anything missing keeps its current value. Anything out of range is replaced
by the firmware default and logged rather than refusing to start, and a table
that binds nothing at all falls back to the 4-way default instead of leaving
you with a screen that swallows touches and does nothing.

---

## If it goes wrong

**Hold both volume keys together for two seconds.** The stick turns off for
the rest of the session and clicks to confirm. This is a hardware-only
combination on purpose: if the stick has made the touchscreen unusable, you
cannot use the touchscreen to escape it.

If Rockbox is started and the previous run enabled the stick but did not
survive ten seconds, the stick starts disabled and says so. Clearing
`Touch Navigation` back to `Classic` and restarting always returns the device
to stock behaviour.

Physical keys are never touched by any of this. `keymap-hibyr1.c` has no
changes in it, and every essential action — play/pause, next/prev, volume,
back, select — works with the stick disabled, misconfigured, or mid-gesture.

---

## Tuning it: the lab plugin

*Plugins → Apps → stick_lab*

Runs the real engine against the real panel while the normal UI is nowhere
near it — nothing in the lab can emit a button, so a bad configuration cannot
strand you and the exit key always works. It draws the arm and work zones,
the live sector and deflection, the dial detent count, and the per-event
cost against the 40 ms touch tick budget.

`PLAY` starts a trace to `/stick_trace.log` in the format the host test
harness replays:

```
# sectors 4
# arm_ms 110
    0 down 240 600
   40 move 240 560
  120 up 240 520
```

Copy that file off the device and replay it on a PC:

```bash
cd tools/stick_test && make && ./stick_test --trace /path/to/stick_trace.log
```

That is how "it feels wrong on the device" becomes a deterministic failing
test instead of an argument.

---

## For developers

| File | What it is |
|---|---|
| `apps/stick.c`, `apps/stick.h` | The engine. No Rockbox headers, no libc, no allocation, integer arithmetic only |
| `apps/stick_config.c` | Settings and `stick.cfg` to an engine config, with validation |
| `apps/stick_glue.c` | The only file that knows about Rockbox: button synthesis, cues, kill switch |
| `apps/plugins/stick_lab.c` | The tuning lab |
| `tools/stick_test/` | Host test harness; needs nothing but a C compiler |

The engine adds **no timer of its own**. While a finger is down the button
driver already reposts the touch event every 40 ms (`REPEAT_INTERVAL_TOUCH`),
and `get_action()` runs on the UI thread, so the stick advances on events the
system already generates. There are no added wakeups while idle.

Run the tests with `cd tools/stick_test && make test`. Every test is named
after the clause of the spec it defends, so a failure names the guarantee it
broke.
