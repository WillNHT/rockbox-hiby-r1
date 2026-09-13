#!/usr/bin/env python3
"""Generate themes/wps/SnappyAnimated.wps.

The repetitive parts of this skin are repetitive because the skin engine
has no way to move a viewport: a thing that travels is N viewports, one
per stop, and %an picks which one is enabled this frame. Writing those by
hand is how coordinates drift, so they are generated and the geometry is
stated once, here.
"""
import io

ACCENT = "D9E021"
INK    = "F4F2EE"
GROUND = "0C0D0E"

# The album art, and the frame the chase runs around.
AX, AY, AW, AH = 30, 160, 420, 400

# The chase: a bright segment travelling clockwise around the art.
CHASE_MS = 70
SEG   = 84          # length of a segment along an edge
THICK = 4

def chase_stops():
    """Clockwise from the top-left corner. Each stop is (x, y, w, h)."""
    stops = []
    # top edge, left to right
    for i in range(5):
        stops.append((AX + i * SEG, AY - THICK, SEG, THICK))
    # right edge, top to bottom (skip the corner the top edge just did)
    for i in range(1, 4):
        stops.append((AX + AW, AY + i * SEG, THICK, SEG))
    # bottom edge, right to left
    for i in range(4, -1, -1):
        stops.append((AX + i * SEG, AY + AH, SEG, THICK))
    # left edge, bottom to top
    for i in range(3, 0, -1):
        stops.append((AX - THICK, AY + i * SEG, THICK, SEG))
    return stops

CHASE = chase_stops()

# The pulse: an accent rule under the title that breathes in and out.
PULSE_MS = 110
PULSE_X, PULSE_Y, PULSE_H = 30, 744, 3
PULSE_W = [40, 90, 150, 220, 300, 220, 150, 90]

out = []
w = out.append

w("""#
#   ____ _  _ ____ ___  ___  _   _   ____ _  _ _ _  _
#   [__  |\\ | |__| |__] |__]  \\_/    |__| |\\ | | |\\/|
#   ___] | \\| |  | |    |      |     |  | | \\| | |  |
#
#  Snappy Animated - Snappy V2, in motion.
#
#  A fork of SnappyV2 that exists to be looked at while it runs. Every
#  layout decision is V2's; what is added is movement, and all of it comes
#  from things this fork changed:
#
#  * %an(frames, period_ms) is new. It is a free-running frame counter -
#    the only skin token whose whole content is the passage of time rather
#    than a reading of some value - and a skin with one in it is redrawn at
#    20 fps instead of waiting for something to happen. Without it the only
#    moving parts a skin could have were the peak meter and the marquee.
#
#  * The skin engine cannot move a viewport, so a thing that travels is one
#    viewport per stop with %an choosing which is enabled. That is why the
#    chase below is sixteen near-identical blocks; they are generated, not
#    typed (tools note: scratchpad/gen_animated.py).
#
#  * It redraws the whole screen several times a second, which is the
#    point. That used to be the thing that made the panel strobe - a full
#    skin update is the backdrop, the art and every bitmap rebuilt - and
#    between the vsynced page flip, the two-plane dirty tracking and the
#    stick overlay no longer asking for repaints of its own, it should now
#    simply be smooth. If this theme flickers, the rendering work is not
#    finished.
#
#  Based on Snappy v1.3 2026 by Chris Soffke, CC-BY-SA, itself a remix of
#  Simon Anden's SNARTY and the SPAZZ / SNAZZ / SNAZZY / Adwaitapod line.
#
%wd
%Fl(2,24-GeistMono-SemiBold.fnt)
%Fl(3,31-GeistMono-SemiBold.fnt)
%Fl(4,44-GeistMono-SemiBold.fnt)
%Fl(5,58-GeistMono-SemiBold.fnt)
%xl(B,batt_wps.bmp,2,0,2)
%xl(O,off_on.bmp,48,0,2)
%xl(vb,vb.bmp)
%xl(bb,bb.bmp)
%xl(vb_backdrop,vb_backdrop.bmp)
%xl(vb_too_loud,vb_too_loud.bmp)
%xl(bb_backdrop,bb_backdrop.bmp)
%?C<%Vd(aa)>
%?mp<|%?C<%Vd(pm_short)|%Vd(pm_long)>|%Vd(paused)|%Vd(ff)|%Vd(rew)|>
%?mh<%Vd(locked)|%Vd(volbar)>
%?bs<%?mv(1.5)<%Vd(voldb)|%Vd(sleep)>|%Vd(voldb)>""")

# ---- the chase ----------------------------------------------------------
w("""#
# The chase
# =========
# A bright segment running clockwise around the album art, one stop per
# frame. It is only enabled while something is playing: a still screen
# with a light running round it is a screen that looks like it is doing
# something when it is not.""")
n = len(CHASE)
for i in range(n):
    w("%%?mp<|%%?if(%%an(%d,%d),=,%d)<%%Vd(ch%02d)>|||||>" % (n, CHASE_MS, i + 1, i))
w("#")
for i, (x, y, cw, ch) in enumerate(CHASE):
    w("%%Vl(ch%02d,%d,%d,%d,%d,-)" % (i, x, y, cw, ch))
    w("%%dr(0,0,-,-,%s,%s)" % (ACCENT, INK))

# ---- the pulse ----------------------------------------------------------
w("""#
# The pulse
# =========
# An accent rule under the title, breathing. Eight widths, and the two
# colours make it a gradient rather than a bar - gradient_fillrect is the
# same path the selector uses, and this is the only place a skin can ask
# for it.""")
for i in range(len(PULSE_W)):
    w("%%?if(%%an(%d,%d),=,%d)<%%Vd(pu%d)>" % (len(PULSE_W), PULSE_MS, i + 1, i))
w("#")
for i, pw in enumerate(PULSE_W):
    w("%%Vl(pu%d,%d,%d,%d,%d,-)" % (i, PULSE_X, PULSE_Y, pw, PULSE_H))
    w("%%dr(0,0,-,-,%s,%s)" % (ACCENT, GROUND))

# ---- everything V2 already had -----------------------------------------
w("""#
# Header
# ======
# Sleep timer
%Vl(sleep,30,30,110,62,2)
%Vs(invert)%ac%bs
%aczzz...
#
# Volume (percent of the -70..-10 dB scale)
%Vl(voldb,30,30,110,62,4)
%Vs(invert)%ac%pv
#
# Shuffle
%V(174,28,94,22,2)
SHF%xd(O,%ps)
#
# Repeat
%V(174,58,94,22,2)
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
%pv(0,0,-,-,vb,backdrop,vb_backdrop)
%?if(%pv, >, 0)<%pv(0,0,-,-,vb_too_loud,backdrop,vb_backdrop)>
#
# Locked
%Vl(locked,30,106,-30,44,4)
%acLOCKED%Vs(invert)
#
# Main
# ====
# The art. The static frame is gone - the chase draws the border now, and
# two borders in the same place is one too many.
%Vl(aa,30,160,420,400,-)
%Cl(2,2,416,396,c,c)
%Cd
#
# The band under the art
# ----------------------
%Vl(pm_short,30,568,240,60,2)
%pm
#
%Vl(pm_long,30,568,-30,60,2)
%pm
#
# Paused. The two bars blink together rather than sitting there: a paused
# screen is the one case where nothing else on it is moving.
%Vl(paused,30,568,240,60,2)
%?if(%an(2,600),=,1)<%dr(86,6,28,48,F4F2EE,F4F2EE)%dr(128,6,28,48,F4F2EE,F4F2EE)|%dr(86,6,28,48,3A3B3D,3A3B3D)%dr(128,6,28,48,3A3B3D,3A3B3D)>
#
%Vl(rew,30,568,240,60,4)
%al%<%< REW
#
%Vl(ff,30,568,240,60,4)
%al%>%> FF
#
# Codec, bit rate, sample rate - cycling, because there is more here than
# fits and %t is the engine's own way of saying "in turn".
%V(280,568,-30,30,2)
%ar%fc %fb
#
%V(280,598,-30,30,2)
%ar%?if(%St(party mode),!=,off)<party|%?if(%St(single mode),!=,off)<%St(single mode)|%t(4)%fk kHz;%t(4)%pp of %pe>>
#
# Track
# -----
# Album
%V(30,632,-30,30,2)
%s%al%id
#
# Artist
%V(30,664,-30,30,2)
%s%al%?if(%ig,=,Classical)<%?ic<By %ic - >%ia|%ia>
#
# Title
%V(30,698,-30,46,4)
%al%s%?it<%it|%fn>
#
# Footer
# ======
# The pulse rule sits at y=744, between the title and this.
%V(30,752,-30,30,2)
%al%pc/%pt%ar%pp/%pe""")

io.open("themes/wps/SnappyAnimated.wps", "w", newline="\n").write("\n".join(out) + "\n")
print("wrote themes/wps/SnappyAnimated.wps  (%d chase stops, %d pulse steps)"
      % (len(CHASE), len(PULSE_W)))
