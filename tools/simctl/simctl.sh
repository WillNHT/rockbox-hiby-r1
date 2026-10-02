#!/bin/bash
# Drive the simulator headlessly through its control FIFO.
#
#   simctl.sh script.txt [outdir]
#
# The script is the little command language in uisimulator/common/sim_ctrl.c:
# down/move/up in LCD coordinates, key <name> [down|up], sleep <ms>, dump,
# quit. Pictures come out of Rockbox's own screen_dump(), so they are the
# LCD framebuffer and not a grab off the X server - which is the whole
# reason this exists.
set -e
# The WSL box has no root, so its libraries live in a private sysroot. A
# machine with the packages installed (a CI runner) has no such directory.
SR=$HOME/sysroot/root
if [ -d "$SR" ]; then
  export PATH=$HOME/bin:$SR/usr/bin:/usr/bin:/bin
  export LD_LIBRARY_PATH=$SR/usr/lib/x86_64-linux-gnu:$SR/lib/x86_64-linux-gnu:$HOME/hostlibs:$SR/usr/lib/x86_64-linux-gnu/pulseaudio
  export MAGICK_CONFIGURE_PATH=$SR/etc/ImageMagick-6
  export MAGICK_CODER_MODULE_PATH=$SR/usr/lib/x86_64-linux-gnu/ImageMagick-6.9.11/modules-Q16/coders
  export MAGICK_FILTER_MODULE_PATH=$SR/usr/lib/x86_64-linux-gnu/ImageMagick-6.9.11/modules-Q16/filters
fi
# WSLg's server can run the simulator perfectly well; it is only *capturing*
# from it that returns stale frames, and nothing here captures from it.
export DISPLAY=${DISPLAY:-:0}
export SDL_AUDIODRIVER=dummy

SCRIPT=${1:?usage: simctl.sh script.txt [outdir]}
OUT=${2:-/tmp/shots}
SIM=${RBSIM_DIR:-$HOME/rb/sim2}
DISK=$SIM/simdisk
FIFO=/tmp/rbsim.ctl
CONVERT=$(command -v convert-im6.q16 || command -v convert)

rm -f $FIFO
rm -f "$DISK"/dump*.bmp

# A script can bring its own setup - a theme, a config - as foo.sh next to
# foo.txt. It runs first, with the simdisk in $DISK.
if [ -f "${SCRIPT%.txt}.sh" ]; then
  DISK=$DISK sh "${SCRIPT%.txt}.sh"
fi
mkdir -p "$OUT"
rm -f "$OUT"/*.png

cd $SIM
RBSIM_CTL=$FIFO ./rockboxui >/tmp/sim.log 2>&1 &
SP=$!

# Wait for the control thread rather than sleeping a guessed interval.
for i in $(seq 1 100); do
  grep -q "sim_ctrl: listening" /tmp/sim.log 2>/dev/null && break
  sleep 0.1
done
if ! grep -q "sim_ctrl: listening" /tmp/sim.log; then
  echo "simctl: control channel never came up" >&2
  tail -20 /tmp/sim.log >&2
  kill $SP 2>/dev/null || true
  exit 1
fi
sleep 2   # let the boot screen settle before the first command

# The script's own sleeps run in the simulator's control thread, so the
# writer finishing says nothing about the script finishing. Every run ends
# with "quit" and we wait for the process: killing on a guessed interval
# silently truncated scripts and produced screenshots of a half-run test.
# A dump is only a request; one straight before "quit" is served after the
# screen has been torn down and comes out blank, hence the sleep.
{ cat "$SCRIPT"; echo; echo "sleep 500"; echo quit; } > $FIFO
for i in $(seq 1 600); do
  kill -0 $SP 2>/dev/null || break
  sleep 0.1
done
if kill -0 $SP 2>/dev/null; then
  echo "simctl: script did not finish in 60 s; killing" >&2
  kill $SP 2>/dev/null || true
fi
wait $SP 2>/dev/null || true

n=0
for f in "$DISK"/dump*.bmp; do
  [ -e "$f" ] || { echo "simctl: no screenshots produced" >&2; exit 1; }
  n=$((n+1))
  $CONVERT "$f" "$OUT/$(printf 'shot%02d' $n).png"
done
echo "simctl: $n screenshot(s) in $OUT"
