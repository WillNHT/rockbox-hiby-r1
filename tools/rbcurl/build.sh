#!/bin/sh
# Build curl with mbedTLS for Settings > Wi-Fi > Update Rockbox: the R1's
# busybox wget cannot finish a TLS handshake with GitHub (alert 80), and
# HiBy OS has no curl or openssl of its own.
#
#   tools/rbcurl/build.sh r1   OUTDIR   (mipsel, for the HiBy R1)
#   tools/rbcurl/build.sh host OUTDIR   (this machine, to try the script)
#
# The result is OUTDIR/curl, linked against nothing but the player's C
# library, and OUTDIR/cacert.pem, the build machine's CA bundle. Both go
# to /.rockbox/lib/; apps/hiby_wifi.c runs curl from there.
set -e

TARGET=${1:?usage: build.sh r1|host OUTDIR}
OUT=${2:?usage: build.sh r1|host OUTDIR}
MBEDTLS_VER=${MBEDTLS_VER:-3.6.2}
CURL_VER=${CURL_VER:-8.11.1}
DL=${RBC_DL:-$HOME/.cache/rbcurl}
JOBS=${JOBS:-$(nproc)}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$DL" "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$OUT/work-$TARGET
PREFIX=$WORK/prefix
mkdir -p "$WORK" "$PREFIX/lib" "$PREFIX/include"

fetch() {
    [ -f "$DL/$2" ] || wget -q -O "$DL/$2" "$1/$2" || rm -f "$DL/$2"
}
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER mbedtls-$MBEDTLS_VER.tar.bz2
fetch https://github.com/curl/curl/releases/download/curl-$(echo $CURL_VER | tr . _) curl-$CURL_VER.tar.xz

case "$TARGET" in
r1)
    CROSS=mipsel-rockbox-linux-gnu-
    HOST=mipsel-linux-gnu
    # As tools/configure builds the R1 firmware. No stack protector or
    # fortify: both pull in glibc symbols newer than the player's.
    CFLAGS="-O2 -march=mips32r2 -mhard-float -mno-mips16 -fno-stack-protector -U_FORTIFY_SOURCE"
    # Pin glibc symbols to versions the player has; see compat.c.
    COMPAT="$WORK/compat.o"
    for s in clock_gettime explicit_bzero; do
        COMPAT="$COMPAT -Wl,--wrap=$s"
    done
    ;;
host)
    CROSS=
    HOST=
    CFLAGS="-O2"
    COMPAT=
    ;;
*)
    echo "unknown target $TARGET" >&2
    exit 1
    ;;
esac
CC=${CROSS}gcc
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"

[ -z "$CROSS" ] || $CC $CFLAGS -c -o "$WORK/compat.o" "$HERE/compat.c"

# ------------------------------------------------------------- mbedTLS
if [ ! -f "$PREFIX/lib/libmbedtls.a" ]; then
    rm -rf "$WORK/mbedtls-$MBEDTLS_VER"
    tar -xjf "$DL/mbedtls-$MBEDTLS_VER.tar.bz2" -C "$WORK"
    make -C "$WORK/mbedtls-$MBEDTLS_VER/library" -j"$JOBS" \
        CC="$CC" AR="${CROSS}ar" CFLAGS="$CFLAGS" GEN_FILES= \
        libmbedtls.a libmbedx509.a libmbedcrypto.a >"$WORK/mbedtls-make.log" 2>&1
    cp "$WORK/mbedtls-$MBEDTLS_VER"/library/*.a "$PREFIX/lib/"
    cp -r "$WORK/mbedtls-$MBEDTLS_VER"/include/mbedtls "$WORK/mbedtls-$MBEDTLS_VER"/include/psa \
        "$PREFIX/include/"
fi

# ---------------------------------------------------------------- curl
# HTTP(S) and nothing else: the updater asks GitHub for a redirect and
# downloads a zip.
rm -rf "$WORK/curl-$CURL_VER"
tar -xJf "$DL/curl-$CURL_VER.tar.xz" -C "$WORK"
(
    cd "$WORK/curl-$CURL_VER"
    CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="-Wl,--gc-sections" LIBS="$COMPAT" \
    ./configure ${HOST:+--host=$HOST} --prefix="$PREFIX" \
        --disable-shared --enable-static --with-mbedtls="$PREFIX" \
        --without-ca-bundle --without-ca-path --without-ca-fallback \
        --without-libpsl --without-zlib --without-brotli --without-zstd \
        --without-libidn2 --without-nghttp2 --without-libssh2 \
        --disable-threaded-resolver --disable-docs --disable-manual \
        --disable-ftp --disable-file --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
        --disable-pop3 --disable-imap --disable-smb --disable-smtp \
        --disable-gopher --disable-mqtt --disable-websockets --disable-ntlm \
        --disable-unix-sockets --disable-alt-svc --disable-hsts \
        >"$WORK/curl-configure.log" 2>&1 || { tail -n 40 "$WORK/curl-configure.log"; exit 1; }
    make -j"$JOBS" >"$WORK/curl-make.log" 2>&1 || { tail -n 40 "$WORK/curl-make.log"; exit 1; }
)
cp "$WORK/curl-$CURL_VER/src/curl" "$OUT/curl"
${CROSS}strip "$OUT/curl"
cp /etc/ssl/certs/ca-certificates.crt "$OUT/cacert.pem"

# A symbol newer than the player's glibc and curl does not start at all.
if [ -n "$CROSS" ]; then
    NEW=$(${CROSS}objdump -T "$OUT/curl" | grep -o 'GLIBC_[0-9.]*' | sort -u -V |
          awk -F_ '{ split($2, v, "."); if (v[1] > 2 || v[2] > 3 || (v[2] == 3 && v[3] > 2)) print }')
    if [ -n "$NEW" ]; then
        echo "curl needs glibc symbols the R1 lacks:" >&2
        ${CROSS}objdump -T "$OUT/curl" | grep -F "$NEW" >&2
        exit 1
    fi
fi
ls -l "$OUT/curl" "$OUT/cacert.pem"
