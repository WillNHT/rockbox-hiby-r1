# Setup for layering.txt: the test WPS and status bar, with no backdrop,
# so the boxes show their own colours.
HERE=$(dirname "$0")
mkdir -p "$DISK/.rockbox/wps" "$DISK/.rockbox/fonts"
cp "$HERE"/../../../themes/fonts/31-GeistMono-SemiBold.fnt "$DISK/.rockbox/fonts/"
cp "$HERE/layering/layering.wps" "$HERE/layering/layering.sbs" \
   "$DISK/.rockbox/wps/"
cat > "$DISK/.rockbox/config.cfg" <<CFG
wps: /.rockbox/wps/layering.wps
sbs: /.rockbox/wps/layering.sbs
statusbar: top
wps transition: off
backdrop: -
CFG
