#!/usr/bin/env python3
"""Generate the Snappy family of .wps files.

Three skins share most of their layout and all of their dial border, and
the skin engine cannot move a viewport - anything that travels is one
viewport per stop with %an choosing which is enabled. Hand-typing sixteen
near-identical blocks three times over is how coordinates drift, so the
geometry is stated once, here, and the files are generated.

    python3 themes/gen_skins.py      (run from the rockbox/ directory)

Writes themes/wps/SnappyV2.wps, SnappyVinyl.wps, SnappyAnimated.wps and
SnappyGauge.wps.
"""
import io

ACCENT = "D9E021"
INK    = "F4F2EE"
GROUND = "0C0D0E"
DIM    = "3A3B3D"

W, H = 480, 800
MARGIN = 30

# ----------------------------------------------------------------- pieces

# The header row. The battery column on the right runs from its bar at
# y=26 to the bottom of its percentage at 82, and the corner box on the
# left is sized from the same two numbers rather than from its own. It
# used to be a 31 px line inverted inside a 62 px viewport starting at 30,
# so its box stopped at 64 while its neighbours ran to 82.
ROW_Y, ROW_B = 26, 82
BOX_X, BOX_W = 30, 124          # "88:88" in the 44 px face is 115 px
# The 44 px face is 48 px tall with its digits in the lower part of the
# line, so the line sits a little below the top of the box to put the
# digits in its middle.
CLOCK_Y, CLOCK_H = ROW_Y + 7, 48
SLEEP_Y, SLEEP_H = ROW_Y + 1, 54   # two 27 px lines

HEADER = """#
# Header
# ======
# The corner box. Whatever is in it - clock, volume, sleep timer - draws
# an inverted line inside this, in the same colour, so the box is the same
# size whichever it is and lines up with the battery column.
%%V(%d,%d,%d,%d,-)
%%dr(0,0,-,-,%s,%s)
#
# Sleep timer
%%Vl(sleep,%d,%d,%d,%d,2)
%%Vs(invert)%%ac%%bs
%%aczzz...
#
# Volume (percent of the -70..-10 dB scale)
%%Vl(voldb,%d,%d,%d,%d,4)
%%Vs(invert)%%ac%%pv
#
# The clock has the box the rest of the time. The volume number is worth
# the corner only while it is changing - the bar says the rest, all the
# time, and a number that never moves is a number nobody reads.
%%Vl(clock,%d,%d,%d,%d,4)
%%Vs(invert)%%ac%%cH:%%cM
#""" % (BOX_X, ROW_Y, BOX_W, ROW_B - ROW_Y, INK, INK,
       BOX_X, SLEEP_Y, BOX_W, SLEEP_H,
       BOX_X, CLOCK_Y, BOX_W, CLOCK_H,
       BOX_X, CLOCK_Y, BOX_W, CLOCK_H) + """
# Shuffle
%V(178,28,94,22,2)
SHF%xd(O,%ps)
#
# Repeat
%V(178,58,94,22,2)
%?mm<RPT%xd(Ob)|RPT%xd(O)|RP1%xd(O)|RND%xd(O)|A-B%xd(O)>
#
# Battery bar
%V(-168,26,-30,18,-)
%bl(0,0,138,18,bb,backdrop,bb_backdrop)
#
# Battery percentage
%V(-168,48,-30,34,3)
%Vs(invert)%ac%?bp<%xd(Bb)|%xd(Ba)> %bl%%
#
# Volume bar
%Vl(volbar,30,110,-30,36,-)
%Vt(0)
%pv(0,0,-,-,vb,backdrop,vb_backdrop)
%?if(%pv, >, 0)<%pv(0,0,-,-,vb_too_loud,backdrop,vb_backdrop)>
#
# Locked
%Vl(locked,30,106,-30,44,4)
%Vt(0)
%acLOCKED%Vs(invert)"""

PRELOAD = """%Fl(2,24-GeistMono-SemiBold.fnt)
%Fl(3,31-GeistMono-SemiBold.fnt)
%Fl(4,44-GeistMono-SemiBold.fnt)
%Fl(5,58-GeistMono-SemiBold.fnt)
%xl(B,batt_wps.bmp,2,0,2)
%xl(O,off_on.bmp,48,0,2)
%xl(vb,vb.bmp)
%xl(bb,bb.bmp)
%xl(vb_backdrop,vb_backdrop.bmp)
%xl(vb_too_loud,vb_too_loud.bmp)
%xl(bb_backdrop,bb_backdrop.bmp)"""


def march(name, x, y, w, h, stops, ms, thick=3, colour=None, tail=None):
    """A bright segment travelling clockwise around a rectangle.

    Returns (enable_lines, viewport_lines). The caller decides what gates
    the enables - the dial border wraps them in %?if(%sd...), the album
    chase in %?mp<...>.
    """
    colour = colour or ACCENT
    tail = tail or INK
    per_side_x = max(1, stops * w // (2 * (w + h)))
    per_side_y = max(1, (stops - 2 * per_side_x) // 2)
    segw = w // per_side_x
    segh = h // per_side_y

    box = []
    for i in range(per_side_x):                      # top, left to right
        box.append((x + i * segw, y - thick, segw, thick))
    for i in range(per_side_y):                      # right, down
        box.append((x + w, y + i * segh, thick, segh))
    for i in range(per_side_x - 1, -1, -1):          # bottom, right to left
        box.append((x + i * segw, y + h, segw, thick))
    for i in range(per_side_y - 1, -1, -1):          # left, up
        box.append((x - thick, y + i * segh, thick, segh))

    n = len(box)
    en = ["%%?if(%%an(%d,%d),=,%d)<%%Vd(%s%02d)>" % (n, ms, i + 1, name, i)
          for i in range(n)]
    vp = []
    for i, (bx, by, bw, bh) in enumerate(box):
        vp.append("%%Vl(%s%02d,%d,%d,%d,%d,-)" % (name, i, bx, by, bw, bh))
        # %Vt(0): clear to the layer underneath rather than to the
        # viewport background. On a skin with a backdrop (%Cb, a
        # theme .bmp, %VB) that is the picture; on one without it there is no layer and skin_layer.c
        # falls back to the opaque clear this always did.
        vp.append("%Vt(0)")
        vp.append("%%dr(0,0,-,-,%s,%s)" % (colour, tail))
    return en, vp


def dial_border(x, y, w, h, ms=60, stops=12):
    """The volume bar, while the stick's dial is armed.

    The dial used to draw a ring around the thumb. A ring around a thumb
    going in circles is a second thing to watch, on top of the one that
    matters, and it sat wherever the hand happened to be. %sd lets the skin
    say it instead, in the place the user is already looking: a rim around
    the volume bar with a segment marching round it.

    Everything here is inside the %sd branch, and %an only costs the skin
    its 20 fps while it is actually being drawn - see animation_enabled in
    wps_internals.h - so a skin that is otherwise still stays still.
    """
    en, vp = march("vd", x, y, w, h, stops, ms, thick=3)
    out = ["#",
           "# The volume dial, armed",
           "# =====================",
           "# See dial_border() in themes/gen_skins.py.",
           "%?if(%sd,=,1)<%Vd(vdrim)>"]
    out += ["%%?if(%%sd,=,1)<%s>" % e for e in en]
    out += ["#",
            "%%Vl(vdrim,%d,%d,%d,%d,-)" % (x - 3, y - 3, w + 6, h + 6),
            "%%dr(0,0,-,2,%s,%s)" % (ACCENT, ACCENT),
            "%%dr(0,%d,-,2,%s,%s)" % (h + 4, ACCENT, ACCENT),
            "%%dr(0,0,2,-,%s,%s)" % (ACCENT, ACCENT),
            "%%dr(%d,0,2,-,%s,%s)" % (w + 4, ACCENT, ACCENT),
            "#"]
    out += vp
    return out


def band(y, meter_h=78, h=82):
    """The peak meter, the codec column, and the transport states that
    share their rectangle.

    No paused badge: the two bars drawn over the meter said nothing the
    stopped meter did not already say, and said it on top of it.

    No playlist position either. It is in the footer, where it belongs;
    having it alternate with the sample rate as well meant the same
    "1 of 2279" appeared twice on the panel, in two places, out of phase.
    """
    return """#
# The band
# ========
# The meter fills %d px of an %d px band. It used to be 24, because the
# engine drew a peak meter one line of the viewport's font tall and
# nothing else; %%pm takes a height now.
%%Vl(pm_short,30,%d,240,%d,2)
%%Vt(0)
%%pm(%d)
#
%%Vl(pm_long,30,%d,-30,%d,2)
%%Vt(0)
%%pm(%d)
#
%%Vl(rew,30,%d,240,46,4)
%%Vt(0)
%%al%%<%%< REW
#
%%Vl(ff,30,%d,240,46,4)
%%Vt(0)
%%al%%>%%> FF
#
%%V(280,%d,-30,30,2)
%%Vt(0)
%%ar%%fc %%fb
#
%%V(280,%d,-30,30,2)
%%Vt(0)
%%ar%%?if(%%St(party mode),!=,off)<party|%%?if(%%St(single mode),!=,off)<%%St(single mode)|%%fk kHz>>""" % (
        meter_h, h, y, h, meter_h, y, h, meter_h,
        y + 18, y + 18, y + 12, y + 42)


def hide_while_armed(lines):
    """Turn every plain viewport in `lines` into a labelled one that is
    only enabled while the dial is *not* armed, and return the enables.

    The gauge covers the panel, and being declared last it wins the full
    update - but a full update is not the only thing that paints. The peak
    meter and the elapsed time are refreshed on their own faster class,
    which does not clear a viewport and does not care what is drawn over
    it, so the skin came back through the gauge a frame later in pieces.
    A modal overlay has to actually turn the rest of the screen off, and in
    the skin language that means every viewport under it is conditional.
    """
    out, enables, n = [], [], 0
    for ln in lines.split("\n"):
        if ln.startswith("%V(") or ln.startswith("%Vl("):
            if ln.startswith("%V("):
                label = "q%d" % n
                n += 1
                ln = "%%Vl(%s,%s" % (label, ln[3:])
            else:
                label = ln[4:ln.index(",")]
            enables.append("%%?if(%%sd,=,0)<%%Vd(%s)>" % label)
        out.append(ln)
    return "\n".join(out), enables

# ------------------------------------------------------------ Snappy V2

VINYL_HEAD = """#
#  ____ _  _ ____ ___  ___  _   _   _  _ _ _  _ _   _ _
#  [__  |\\ | |__| |__] |__]  \\_/    |  | | |\\ |  \\_/  |
#  ___] | \\| |  | |    |      |      \\/  | | \\|   |   |___
#
#  Snappy Vinyl - Snappy V2 with a record where the cover was.
#
#  The record is %Cr, drawn by canvas_vinyl() (apps/canvas.c): grooves, the
#  gaps between tracks, a rim, two highlights that stay where the light is,
#  and the album cover as the label. It turns clockwise in 2 degree steps
#  at 60 degrees a second (one turn in six seconds) while something is
#  playing, and stops where it is when paused. A track with no cover gets
#  a label in the accent colour with a mark on it, so the turning still
#  shows. The diameter is the cover's edge, so the %Cl size is V2's to the
#  pixel and the two skins share one album-art slot.
#
#  A pre-rendered strip was the other way to do this - 180 frames of a
#  416 px disc is tens of megabytes - so it is computed per frame instead.
#
#  Generated by themes/gen_skins.py. Coordinates are absolute for 480x800.
#
%wd"""

# centre and radius of the record inside the 420 px art viewport, and its
# motion
VINYL_R = 208
VINYL_STEP, VINYL_SPEED, VINYL_LABEL = 2, 60, 36


def snappy_v2(vinyl=False):
    o = []
    if vinyl:
        o.append(VINYL_HEAD)
    else:
        o.append("""#
#  ____ _  _ ____ ___  ___  _   _   _  _ ____
#  [__  |\\ | |__| |__] |__]  \\_/    |  |  __]
#  ___] | \\| |  | |    |      |      \\/  |___
#
#  Snappy V2 - Chris Soffke's Snappy (CC-BY-SA), relaid for a device whose
#  transport lives on the physical keys and whose panel belongs to the
#  stick.
#
#  * The prev / play-pause / next badges and their touch band are gone.
#    Under the stick those regions never fire, so they were decoration
#    holding 88 px of the middle of the screen.
#  * The progress bar and its seek region are gone for the same reason: a
#    seek you cannot touch is a picture of a seek. Elapsed / total and the
#    playlist position stay as text - information, not a control.
#  * The album art takes the freed space: the full width between the
#    theme's 30 px margins.
#  * The peak meter, codec, bit rate and sample rate move into one band
#    beneath it. The meter fills 78 px of that band; the room came from
#    the 22 px that were empty under the footer.
#  * While the stick's volume dial is armed, the volume bar wears a
#    marching rim (%sd). The dial used to draw a ring around the thumb;
#    this says the same thing where the user is already looking.
#
#  Generated by themes/gen_skins.py. Coordinates are absolute for 480x800.
#
#  Based on Snappy v1.3 2026 by Chris Soffke, CC-BY-SA, itself a remix of
#  Simon Anden's SNARTY and the SPAZZ / SNAZZ / SNAZZY / Adwaitapod line.
#
%wd""")
    o.append(PRELOAD)
    if vinyl:
        # The record is there with or without a cover.
        o.append("%Vd(aa)")
    else:
        o.append("%?C<%Vd(aa)|%Vd(noart)%Vd(noartlabel)>")
    o.append("%?mp<|%?C<%Vd(pm_short)|%Vd(pm_long)>||%Vd(ff)|%Vd(rew)|>")
    o.append("%?mh<%Vd(locked)|%Vd(volbar)>")
    o.append("%?bs<%?mv(1.5)<%Vd(voldb)|%Vd(sleep)>|"
             "%?mv(1.5)<%Vd(voldb)|%Vd(clock)>>")
    o += dial_border(30, 110, 420, 36)
    o.append(HEADER)
    if vinyl:
        o.append("""#
# Main
# ====
# The record. %%Cl claims the same album-art size as Snappy V2 and is not
# drawn with %%Cd: %%Cr takes the cover from it as the label.
%%Vl(aa,30,150,420,420,-)
%%Cl(2,2,416,416,c,c)
%%Cr(%d,%d,%d,%d,%d,%d,%s)""" % (210, 210, VINYL_R, VINYL_STEP, VINYL_SPEED,
                            VINYL_LABEL, ACCENT))
    else:
        o.append("""#
# Main
# ====
# The art, full width. The frame is four rules rather than a bitmap so it
# costs nothing and inherits the theme's foreground colour. %Cl is
# viewport-relative, so the inset is 2,2 and not 32,162.
%Vl(aa,30,150,420,420,-)
%Cl(2,2,416,416,c,c)
%dr(0,0,420,2)
%dr(0,418,-,-)
%dr(0,2,2,416)
%dr(418,2,-,416)
%Cd
#
# No cover is not an empty screen. %C is false for a track with no
# artwork, and the frame above is inside the viewport that goes with it,
# so without this the whole middle of the panel is nothing at all.
%Vl(noart,30,150,420,420,-)
%dr(0,0,420,2)
%dr(0,418,-,-)
%dr(0,2,2,416)
%dr(418,2,-,416)
#
%Vl(noartlabel,40,340,400,40,2)
%ac%s%?id<%id|%?ia<%ia|no cover>>""")
    o.append(band(574, 70, 74))
    o.append("""#
# Track
# -----
%V(30,656,-30,30,2)
%s%al%id
#
%V(30,688,-30,30,2)
%s%al%?if(%ig,=,Classical)<%?ic<By %ic - >%ia|%ia>
#
%V(30,720,-30,46,4)
%al%s%?it<%it|%fn>
#
# Footer
# ======
%V(30,768,-30,30,2)
%Vt(0)
%al%pc/%pt%ar%pp/%pe""")
    return "\n".join(o) + "\n"


# ------------------------------------------------------- Snappy Animated

# The cover floats, with its own reflection under it, over a blurred copy
# of itself.
#
# Deliberately the same rectangle and the same %Cl size as Snappy V2, and
# that is not tidiness. playback_claim_aa_slot() hands out one slot per
# *distinct* size and there are only SKINNABLE_SCREENS_COUNT of them, so a
# skin asking for a size nothing else asks for claims one of its own; when
# they run out the claim fails, %C goes false, and every art-dependent
# viewport in the skin silently disappears - which is a now playing screen
# that is a black hole from the header to the peak meter. Matching V2 to
# the pixel means these skins share one slot between them.
AX, AY, AS = 30, 150, 420
AH = 420              # square: a cover is square and the inset is 2 px
ART_W, ART_H = 416, 416
# Butted against the cover's lower edge: two pixels lower and the reflection
# floated with a dark seam between it and the cover it is reflecting.
MIRROR_Y, MIRROR_H = AY + AH - 2, 40
BAND_Y = 616
# The sheen sweeps the cover, and stops there. It used to run to y=708,
# over the peak meter, the codec line, the artist and the title - and
# those are static viewports, drawn on a full update and never again, so
# every pass of the sheen took a 26 px bite out of the chrome and nothing
# put it back. That is the whole of "duration, visualiser, bit rate and
# playlist are not showing": they were showing, and then they were eaten.
#
# Granular: the band moves 6 px a frame at the engine's 20 fps, so what the
# eye sees is a glide rather than the 20-40 px hops it used to make. The
# band has soft edges - SHEEN_PROFILE is its opacity from the leading edge
# to the trailing one, one strip each - so there is no hard line to jump.
# After the sweep the counter runs on through SHEEN_REST empty frames: a
# pause, so the sheen reads as a glint and not as a scanner.
SHEEN_MS, SHEEN_STEP = 50, 6
SHEEN_PROFILE = [(6, 2), (6, 4), (6, 7), (8, 10), (6, 7), (6, 4), (6, 2)]
SHEEN_H = sum(h for h, _ in SHEEN_PROFILE)
SHEEN_Y0 = AY + 2
SHEEN_STOPS = (AH - 4 - SHEEN_H) // SHEEN_STEP + 1
SHEEN_REST = 30


def animated(gauge=False):
    name = "Snappy Gauge" if gauge else "Snappy Animated"
    o = []
    o.append("""#
#   ____ _  _ ____ ___  ___  _   _   %s
#   [__  |\\ | |__| |__] |__]  \\_/
#   ___] | \\| |  | |    |      |
#
#  %s - Snappy V2, in motion.
#
#  Everything that moves here comes from something this fork added:
#
#  * %%an(frames, period_ms) is a free-running frame counter - the only
#    skin token whose whole content is the passage of time. A skin drawing
#    one is redrawn at 20 fps instead of waiting for the track to do
#    something. It costs nothing while it is not being drawn, so the dial
#    border below is free until the dial is armed.
#  * %%Cb draws the cover scaled to cover the panel, blurred and washed
#    with a gradient from its own colour to the theme's ground - into
#    the LCD *backdrop*, the one surface a later viewport's clear
#    restores rather than wipes. The sharp cover, its shadow and its
#    reflection are composed into it too.
#  * %%Vt(0) makes a viewport clear to that backdrop instead of to a colour,
#    and %%dr takes an opacity - that is how chrome sits on the picture.
#  * %%Cm mirrors the bottom of the cover under itself, fading out.
#  * %%dr with two colours goes through gradient_fillrect.
#
#  The skin engine cannot move a viewport, so anything that travels is one
#  viewport per stop with %%an choosing which is enabled. Generated by
#  themes/gen_skins.py; do not hand-edit.
#
%%wd""" % (name.upper().replace("SNAPPY ", ""), name))
    o.append(PRELOAD)
    o.append("%Vd(bg)")
    o.append("%?C<%Vd(aa)|%Vd(noart)%Vd(noartlabel)>")
    o.append("%?C<%Vd(mirror)>")
    o.append("%?mp<|%?C<%Vd(pm_short)|%Vd(pm_long)>||%Vd(ff)|%Vd(rew)|>")
    # The volume bar is a viewport of its own again. It used to be drawn
    # inside the backdrop viewport when there was a cover, because a
    # viewport clears its background and a black box across the top of a
    # blurred cover is exactly what that looks like - there was no
    # transparent viewport in the skin language to put it in. %Vt(0) is
    # that viewport, so the special case is gone and the bar is one thing
    # in one place whether or not the track has artwork.
    o.append("%?mh<%Vd(locked)|%Vd(volbar)>")
    o.append("%?bs<%?mv(1.5)<%Vd(voldb)|%Vd(sleep)>|"
             "%?mv(1.5)<%Vd(voldb)|%Vd(clock)>>")

    if gauge:
        o.append("%?if(%sd,=,1)<%Vd(gauge)>%?if(%sd,=,1)<%Vd(gaugepv)>"
                 "%?if(%sd,=,1)<%Vd(gaugecap)>%?if(%sd,=,1)<%Vd(gaugebar)>")
        # Remembered, not appended: see the note where it is filled in.
        unarmed_slot = len(o)
        o.append("")
    # Every enable for something that moves goes here, in the default
    # viewport. Placed next to its own viewports it lands inside whichever
    # %Vl was declared last - "locked", hidden while unlocked - and a tag
    # inside a hidden viewport is never evaluated, so the sheen was
    # never switched on at all.
    #
    # There used to be two more: a yellow segment chasing round the cover's
    # edge and a breathing rule under the title. Both are gone (#23) - the
    # cover is the picture, and a light running round it only competed.
    frames = SHEEN_STOPS + SHEEN_REST
    for i in range(SHEEN_STOPS):
        o.append("%%?if(%%an(%d,%d),=,%d)<%%Vd(sh%02d)>"
                 % (frames, SHEEN_MS, i + 1, i))

    if not gauge:
        # Its enables come first in what it returns, then its viewports -
        # so it goes last among the enables.
        o += dial_border(30, 110, 420, 36)

    o.append(HEADER)

    # Moving parts are collected here and emitted after the backdrop
    # viewport: viewports draw in file order, and the backdrop viewport's
    # full-update redraw (the cover, on the panel) would otherwise land on
    # top of them.
    moving = []
    moving.append("""#
# The sheen
# =========
# A soft band gliding down the cover, then a pause. The backdrop cannot
# move - it is rendered once into the backdrop buffer and re-rendering it
# every frame would be a 480x800 scale and blur at 20 fps - so the movement
# goes over the top of it instead, which costs a few blended rectangles.
#
# %%Vt(0) and the seventh parameter of %%dr are what make it a sheen
# rather than a bar. The clear restores the backdrop buffer - the cover -
# and each strip is then blended over it at a few percent. Without the
# first the band is a black box eating the cover; without the second it
# is a painted grey stripe. The strips rise and fall in opacity, so the
# band has no edge, and it moves %d px a frame, so it has no jump.""" % SHEEN_STEP)
    moving.append("#")
    for i in range(SHEEN_STOPS):
        sy = SHEEN_Y0 + i * SHEEN_STEP
        moving.append("%%Vl(sh%02d,%d,%d,%d,%d,-)"
                      % (i, AX + 2, sy, ART_W, SHEEN_H))
        moving.append("%Vt(0)")
        y = 0
        for h, a in SHEEN_PROFILE:
            moving.append("%%dr(0,%d,-,%d,%s,%s,%d)" % (y, h, INK, INK, a))
            y += h

    o.append("""#
# The backdrop
# ============
# The cover, scaled to cover the whole panel, heavily blurred, pulled
# towards grey, and washed with a gradient that starts in the cover's own
# average colour at the top and sinks into the theme's ground at the
# bottom - the Spotify look. The sharp cover sits on a soft dark shadow so
# its edge never melts into its own blurred copy. %%Cb does all of it (see
# skin_art_fx.c); apps/canvas.c has the blur and the wash.
#
# Labelled and enabled from the default viewport, because the default
# viewport cannot draw: skin_render() sets its refresh mode to zero
# whenever a skin has real viewports after it, so a %%Cb in an unlabelled
# %%V(0,0,-,-,-) is parsed, is reached, and is silently never refreshed.
#
# Nothing here is drawn to the panel. %%Cb composes the blur, the cover
# (%%Cl/%%Cd) and the reflection (%%Cm) into a buffer that becomes the LCD
# backdrop - the one surface a later viewport's clear restores rather than
# wipes, and the whole of what "transparent" means in this engine
# (apps/gui/skin_engine/skin_art_fx.c, skin_layer.c). Every %%Vt(0)
# viewport on this screen clears back to that picture, sharp cover
# included, which is why the sheen can run over the cover.
#
# skin_render() repaints the gaps between viewports from the backdrop on
# every full update while it is live; before it did, the picture showed
# only where some viewport happened to clear.
#
# Things that change on their own stay out: the volume bar is a viewport
# of its own, because composed into the backdrop it would be a stale bar
# repainted only when the cover changes.
#
# Enabled unconditionally rather than behind %%?C. With no cover there is
# no backdrop, and the viewport's own clear to the theme's ground is what
# the screen should show; the veiled viewports fall back to that too.
%%Vl(bg,0,0,-,-,-)
%%Cb(0,0,480,800,40,62)
%%Cl(%d,%d,%d,%d,c,c)
%%Cd
%%Cm(%d,%d,%d,%d,170,0)
#
# Main
# ====
# The cover and its reflection are drawn in the backdrop viewport above.
# These two stay as one-pixel stubs only because the default viewport
# still names them and a %%Vd naming a viewport that does not exist fails
# the parse and drops the user to the failsafe WPS.
%%Vl(aa,%d,%d,1,1,-)
#
%%Vl(mirror,%d,%d,1,1,-)
#
# No cover is not an empty screen. %%C is false for a track with no
# artwork, and with it the backdrop, the cover and the reflection all go -
# so without this the whole middle of the panel is nothing at all.
%%Vl(noart,%d,%d,%d,%d,-)
%%Vt(0)
%%dr(0,0,-,2)
%%dr(0,%d,-,2)
%%dr(0,2,2,%d)
%%dr(%d,2,2,%d)
#
# The label is its own viewport. Rules and text in one viewport fight over
# where the line sits, and the frame came out a fifth of its height with
# the caption stranded below it.
%%Vl(noartlabel,%d,%d,%d,40,2)
%%Vt(0)
%%ac%%s%%?id<%%id|%%?ia<%%ia|no cover>>""" % (AX + 2, AY + 2, ART_W, ART_H,
                              AX + 2, MIRROR_Y, ART_W, MIRROR_H,
                              AX, AY,
                              AX, MIRROR_Y,
                              AX, AY, AS, AH,
                              AH - 2, AH - 4, AS - 2, AH - 4,
                              AX + 10, AY + AH // 2 - 20, AS - 20))
    o += moving
    content = [band(BAND_Y, 62, 66), """#
# Track
# -----
%V(30,690,-30,28,2)
%Vt(0)
%s%al%?if(%ig,=,Classical)<%?ic<By %ic - >%ia|%ia>
#
%V(30,718,-30,46,4)
%Vt(0)
%al%s%?it<%it|%fn>"""]

    content.append("""#
# Footer
# ======
%V(30,768,-30,30,2)
%Vt(0)
%al%pc/%pt%ar%pp/%pe""")

    if gauge:
        body, enables = hide_while_armed("\n".join(content))
        # Into the *default* viewport, at the top of the file, and not here.
        #
        # Appended here they land after every declaration in the file, which
        # puts them inside the last %Vl - one of the pulse viewports, which
        # is hidden - and a tag inside a hidden viewport is never evaluated.
        # So every viewport the gauge hides stayed hidden for ever: the
        # codec column, the artist, the title and the footer were missing
        # from this skin with the dial never once armed.
        o[unarmed_slot] = "\n".join(enables)
        o.append(body)
    else:
        o += content

    if gauge:
        # The whole panel, while the dial is armed.
        o.append("""#
# The gauge
# =========
# Snappy Animated says "the volume is what your thumb means now" with a rim
# round the volume bar. This fork says it with the whole screen: while the
# dial is armed the panel becomes the volume and nothing else, because a
# gesture that has taken over the meaning of the entire surface may as well
# say so on the entire surface.
#
# Declared last on purpose. A viewport clears its background, so this one
# covers what came before it - which is exactly what a modal overlay is -
# and arming or disarming the dial asks the skin for a full update, so the
# screen underneath comes back the moment the thumb lifts.
%Vl(gauge,0,0,-,-,5)
%Vb(0C0D0E)
# %Vt(100): a plain clear copies the backdrop, and the backdrop is the
# cover. A modal wants the ground, solid.
%Vt(100)
#
# Labelled, like the panel behind them. A plain %V here is drawn on every
# full update whatever %sd says, so the reading and its caption sat across
# the middle of the cover with the dial not armed at all.
%Vl(gaugepv,0,300,-,70,5)
%Vt(100)
%ac%pv
#
%Vl(gaugecap,0,380,-,30,2)
%Vt(100)
%acVOLUME
#
%Vl(gaugebar,60,440,360,40,-)
%Vt(100)
%pv(0,0,-,-,vb,backdrop,vb_backdrop)""")
        en, vp = march("gg", 60, 440, 360, 40, 16, 55, thick=4)
        o += ["%%?if(%%sd,=,1)<%s>" % e for e in en]
        o.append("#")
        o += vp

    return "\n".join(o) + "\n"


io.open("themes/wps/SnappyV2.wps", "w", newline="\n").write(snappy_v2())
io.open("themes/wps/SnappyVinyl.wps", "w", newline="\n").write(snappy_v2(True))
io.open("themes/wps/SnappyAnimated.wps", "w", newline="\n").write(animated(False))
io.open("themes/wps/SnappyGauge.wps", "w", newline="\n").write(animated(True))
print("wrote SnappyV2.wps, SnappyVinyl.wps, SnappyAnimated.wps, SnappyGauge.wps")
