#!/bin/sh
# Fill a simdisk with a small music library to test against.
#
#   fixtures.sh simdisk
#
# fixtures/library.tsv is the tags and lengths of real albums - the names
# that break things are real ones: Vietnamese, fullwidth, brackets. The
# audio is not theirs. Each track is a sine tone of the right length, so
# nothing copyrighted is stored here or made here, and the library costs
# the repository one text file.
set -eu
OUT=${1:?usage: fixtures.sh simdisk}/Music
TAB=$(printf '\t')

LIST=$(dirname "$0")/fixtures/library.tsv

grep -v '^#' "$LIST" | {
while IFS=$TAB read -r artist album track title secs; do
  dir="$OUT/$artist/$album"
  mkdir -p "$dir"
  # In the background: the tones are most of a simulator run otherwise.
  ffmpeg -nostdin -loglevel error -y     -f lavfi -i "sine=frequency=$((220 + track * 40)):duration=$secs"     -ac 1 -c:a aac -b:a 24k     -metadata artist="$artist" -metadata album="$album"     -metadata title="$title" -metadata track="$track"     "$dir/$(printf '%02d' "$track") $title.m4a" &
  # One flat colour per album, picked by its name, so covers can be told
  # apart in a screenshot.
  [ -e "$dir/cover.jpg" ] ||
    ffmpeg -nostdin -loglevel error -y -f lavfi       -i "color=c=0x$(printf '%s' "$album" | md5sum | cut -c1-6):s=300x300"       -frames:v 1 "$dir/cover.jpg"
done
wait
}
# A background ffmpeg that failed took its track with it, quietly.
want=$(grep -vc '^#' "$LIST")
got=$(find "$OUT" -name '*.m4a' -size +0 | wc -l)
echo "fixtures: $got of $want track(s) in $OUT"
[ "$got" -eq "$want" ]
