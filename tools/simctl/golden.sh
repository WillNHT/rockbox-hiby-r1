#!/bin/bash
# Golden images: every theme driven through the same script, each shot
# compared with the one committed under tools/simctl/golden/<theme>/.
#
#   golden.sh [theme-slug...]
#
# RBSIM_DIR is a built and installed simulator, as for simctl.sh. Output
# goes to $OUT (default /tmp/golden): the shots, a diff image for every
# shot that changed, and new/<theme>/ holding the shots of this run laid
# out like the goldens. To accept a visual change, copy new/ over
# tools/simctl/golden/ in the same PR, so review sees the pictures change.
#
# The wall clock starts at the same instant every run (faketime), so the
# status bar clock is the same. Anything else that moves on its own - the
# elapsed time, a sheen - is painted out by a rectangle in masks.txt
# before comparing.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
SIM=${RBSIM_DIR:?RBSIM_DIR must point at an installed simulator}
DISK=$SIM/simdisk
OUT=${OUT:-/tmp/golden}
CONVERT=$(command -v convert-im6.q16 || command -v convert)
COMPARE=$(command -v compare-im6.q16 || command -v compare)

# The themes as they sit on a card, as the release lays them out.
mkdir -p "$DISK/.rockbox/wps" "$DISK/.rockbox/fonts" "$DISK/.rockbox/themes" "$DISK/.rockbox/icons"
cp -r "$ROOT"/themes/wps/. "$DISK/.rockbox/wps/"
cp -r "$ROOT"/themes/fonts/. "$DISK/.rockbox/fonts/"
cp -r "$ROOT"/themes/icons/. "$DISK/.rockbox/icons/"
cp "$ROOT"/themes/*.cfg "$DISK/.rockbox/themes/"

cat "$HERE/tests/library.txt" "$HERE/golden/script.txt" > /tmp/golden.txt

rm -rf "$OUT"
mkdir -p "$OUT"
fail=0

for cfg in "$ROOT"/themes/*.cfg; do
  name=$(basename "$cfg" .cfg)
  slug=$(echo "$name" | tr -c 'A-Za-z0-9\n' '-' | sed 's/--*/-/g')
  if [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$slug"; then
    continue
  fi

  # A fresh config each time: the theme on its own, no transitions, so
  # a dump never lands mid-animation of a screen change.
  { grep -v '^#' "$cfg"; echo "wps transition: off"; } > "$DISK/.rockbox/config.cfg"
  # and nothing a previous run left behind: the browse folder, the
  # playlist to resume, bookmarks.
  rm -f "$DISK"/.rockbox/*.dat "$DISK"/.rockbox/nvram.bin \
        "$DISK"/.rockbox/.resume.cfg "$DISK"/.rockbox/.playlist_control* \
        "$DISK"/.rockbox/*.bmark
  find "$DISK/Music" -name '*.bmark' -delete

  RBSIM_CLOCK=1 TZ=UTC FAKETIME="@2026-01-01 12:00:00" FAKETIME_DONT_FAKE_MONOTONIC=1 \
    LD_PRELOAD=${FAKETIME_LIB:-} \
    timeout 120 bash "$HERE/simctl.sh" /tmp/golden.txt "$OUT/new/$slug" >/dev/null ||
    { echo "golden: $slug: simulator run failed or hung"; fail=1; pkill -9 rockboxui || true; continue; }

  for shot in "$OUT/new/$slug"/*.png; do
    s=$(basename "$shot" .png)
    gold=$HERE/golden/$slug/$s.png
    if [ ! -f "$gold" ]; then
      echo "golden: $slug/$s: no golden yet"
      fail=1
      continue
    fi
    # Paint the moving parts out of both before comparing.
    draw=$(awk -v t="$slug" -v s="$s" \
      '$1==t && ($2==s || $2=="*") { printf "-draw \"rectangle %s %s\" ", $3, $4 }' \
      "$HERE/golden/masks.txt")
    eval "$CONVERT \"$shot\" -fill magenta $draw \"$OUT/a.png\""
    eval "$CONVERT \"$gold\" -fill magenta $draw \"$OUT/b.png\""
    px=$($COMPARE -metric AE "$OUT/a.png" "$OUT/b.png" "$OUT/diff-$slug-$s.png" 2>&1 || true)
    px=${px%% *}   # newer ImageMagick appends " (fraction)"
    if [ "$px" != 0 ]; then
      echo "golden: $slug/$s: $px pixel(s) differ"
      fail=1
    else
      rm -f "$OUT/diff-$slug-$s.png"
    fi
  done
done

rm -f "$OUT/a.png" "$OUT/b.png"
[ $fail = 0 ] && echo "golden: all shots match"
exit $fail
