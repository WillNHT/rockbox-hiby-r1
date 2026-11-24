# Setup for audiobooks.txt: three books of two chapters, Audiobooks first
# in the main menu, and nothing to resume, so the run starts on the menu.
for b in "Ann Leckie - Ancillary Justice" "Frank Herbert - Dune" \
         "Ursula Le Guin - The Dispossessed"; do
  d="$DISK/Audiobooks/$b"
  mkdir -p "$d"
  for i in 1 2; do
    [ -s "$d/0$i.m4a" ] ||
      ffmpeg -nostdin -loglevel error -y \
        -f lavfi -i "sine=frequency=$((300 + i * 50)):duration=60" \
        -ac 1 -c:a aac -b:a 24k -metadata artist="${b%% - *}" \
        -metadata album="${b#* - }" -metadata title="Chapter $i" \
        -metadata track=$i "$d/0$i.m4a"
  done
done
rm -f "$DISK"/.rockbox/.playlist_control* "$DISK/.rockbox/.resume.cfg" \
      "$DISK/.rockbox/most-recent.bmark" "$DISK/.rockbox/audiobooks.progress"
cat > "$DISK/.rockbox/config.cfg" <<CFG
root menu order: audiobooks, files, settings
CFG
