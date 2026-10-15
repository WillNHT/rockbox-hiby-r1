# Setup for snappy-animated.txt: the theme as it is installed on a device.
ROOT=$(dirname "$0")/../../..
mkdir -p "$DISK/.rockbox/wps" "$DISK/.rockbox/fonts"
cp -r "$ROOT"/themes/wps/. "$DISK/.rockbox/wps/"
cp "$ROOT"/themes/fonts/*.fnt "$DISK/.rockbox/fonts/"
{ grep -v '^#' "$ROOT/themes/Snappy Animated.cfg"; echo "wps transition: off"; } \
    > "$DISK/.rockbox/config.cfg"
