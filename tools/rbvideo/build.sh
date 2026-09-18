#!/bin/sh
# Build librbvideo.so: FFmpeg and libwebp, trimmed to decoding, behind
# apps/video/rbvideo.h. See tools/rbvideo/README.md.
#
#   tools/rbvideo/build.sh r1   OUTDIR   (mipsel, for the HiBy R1)
#   tools/rbvideo/build.sh host OUTDIR   (this machine, for the simulator)
#
# Sources are downloaded into $RBV_DL (default ~/.cache/rbvideo) unless
# they are already there. The result is OUTDIR/librbvideo.so, one file with
# no dependencies beyond the C library, to be copied to /.rockbox/lib/.
set -e

TARGET=${1:?usage: build.sh r1|host OUTDIR}
OUT=${2:?usage: build.sh r1|host OUTDIR}
FFMPEG_VER=${FFMPEG_VER:-8.0}
WEBP_VER=${WEBP_VER:-1.5.0}
ZLIB_VER=${ZLIB_VER:-1.3.1}
DL=${RBV_DL:-$HOME/.cache/rbvideo}
JOBS=${JOBS:-$(nproc)}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

mkdir -p "$DL" "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$OUT/work-$TARGET
PREFIX=$WORK/prefix
mkdir -p "$WORK" "$PREFIX"

fetch() {
    [ -f "$DL/$2" ] || wget -q -O "$DL/$2" "$1/$2"
}
fetch https://zlib.net/fossils zlib-$ZLIB_VER.tar.gz
fetch https://ffmpeg.org/releases ffmpeg-$FFMPEG_VER.tar.xz
fetch https://storage.googleapis.com/downloads.webmproject.org/releases/webp libwebp-$WEBP_VER.tar.gz

case "$TARGET" in
r1)
    CROSS=mipsel-rockbox-linux-gnu-
    CC=${CROSS}gcc
    HOST=mipsel-linux-gnu
    # As tools/configure builds the R1 firmware.
    CFLAGS="-O2 -march=mips32r2 -mhard-float -mno-mips16 -fPIC -ffunction-sections -fdata-sections"
    FF_ARCH="--enable-cross-compile --cross-prefix=$CROSS --arch=mips --cpu=mips32r2 --target-os=linux
             --disable-mipsdsp --disable-mipsdspr2 --disable-msa --disable-mmi
             --disable-mips32r5 --disable-mips32r6 --disable-mips64r2 --disable-mips64r6
             --disable-mipsfpu"
    ;;
host)
    CROSS=
    CC=gcc
    HOST=
    CFLAGS="-O2 -fPIC -ffunction-sections -fdata-sections"
    FF_ARCH="--disable-x86asm"
    ;;
*)
    echo "unknown target $TARGET" >&2
    exit 1
    ;;
esac

# --------------------------------------------------------------- zlib
# Built here rather than taken from the toolchain: the APNG decoder needs
# it, and a toolchain's libz.a is not position-independent, so it cannot
# go inside a shared object. Everything ends up inside librbvideo.so, so
# the player needs no zlib of its own.
if [ ! -f "$PREFIX/lib/libz.a" ]; then
    rm -rf "$WORK/zlib-$ZLIB_VER"
    tar -xzf "$DL/zlib-$ZLIB_VER.tar.gz" -C "$WORK"
    (
        cd "$WORK/zlib-$ZLIB_VER"
        CC="$CC" CFLAGS="$CFLAGS" ./configure --prefix="$PREFIX" --static \
            >"$WORK/zlib-configure.log"
        make -j"$JOBS" libz.a >"$WORK/zlib-make.log" 2>&1
        make install >/dev/null
    )
fi

# ------------------------------------------------------------ libwebp
if [ ! -f "$PREFIX/lib/libwebpdemux.a" ]; then
    rm -rf "$WORK/libwebp-$WEBP_VER"
    tar -xzf "$DL/libwebp-$WEBP_VER.tar.gz" -C "$WORK"
    (
        cd "$WORK/libwebp-$WEBP_VER"
        ./configure ${HOST:+--host=$HOST} CC="$CC" CFLAGS="$CFLAGS" \
            --prefix="$PREFIX" --enable-static --disable-shared \
            --disable-libwebpmux --enable-libwebpdemux --disable-libwebpdecoder \
            --disable-png --disable-jpeg --disable-tiff --disable-gif \
            --disable-wic --disable-gl --disable-sdl --disable-threading \
            >"$WORK/webp-configure.log"
        make -j"$JOBS" >"$WORK/webp-make.log"
        make install >/dev/null
    )
fi

# ------------------------------------------------------------- FFmpeg
# Decoding only. Demuxers for the containers people have videos in, the
# decoders for what is inside them, and the audio a standalone video
# needs. HEVC and VP9 are here for completeness: they decode, slowly.
if [ ! -f "$PREFIX/lib/libavcodec.a" ]; then
    rm -rf "$WORK/ffmpeg-$FFMPEG_VER"
    tar -xJf "$DL/ffmpeg-$FFMPEG_VER.tar.xz" -C "$WORK"
    (
        cd "$WORK/ffmpeg-$FFMPEG_VER"
        # zlib is what the APNG decoder needs; without it FFmpeg quietly
        # drops that one and everything else still builds.
        configure_ffmpeg() {
        # shellcheck disable=SC2086
        ./configure $FF_ARCH $1 --prefix="$PREFIX" \
            --cc="$CC" --extra-cflags="$CFLAGS -I$PREFIX/include" \
            --extra-ldflags="-L$PREFIX/lib" \
            --enable-pic --enable-static --disable-shared \
            --disable-programs --disable-doc --disable-network \
            --disable-autodetect --disable-debug \
            --disable-everything \
            --disable-avdevice --disable-avfilter \
            --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
            --enable-protocol=file \
            --enable-demuxer=mov,matroska,flv,avi,mpegps,mpegts,mpegvideo,m4v,h264,hevc,gif,ogg,asf,image2,apng \
            --enable-decoder=h264,hevc,mpeg4,h263,h263p,flv,mpeg1video,mpeg2video,mjpeg,vp8,vp9,theora,wmv1,wmv2,wmv3,vc1,msmpeg4v1,msmpeg4v2,msmpeg4v3,gif,apng,png,mjpegb \
            --enable-decoder=aac,aac_latm,mp3,mp3float,mp2,mp2float,ac3,opus,vorbis,flac,pcm_s16le,pcm_s16be,pcm_u8,pcm_f32le,wmav1,wmav2 \
            --enable-parser=h264,hevc,mpeg4video,mpegvideo,h263,vp8,vp9,aac,aac_latm,mpegaudio,ac3,opus,vorbis,flac,png,gif,vc1,mjpeg \
            --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,vp9_superframe_split \
            >"$WORK/ffmpeg-configure.log" 2>&1
        }
        configure_ffmpeg --enable-zlib ||
        configure_ffmpeg "" ||
            { tail -30 "$WORK/ffmpeg-configure.log"; exit 1; }
        make -j"$JOBS" >"$WORK/ffmpeg-make.log" 2>&1 || { tail -30 "$WORK/ffmpeg-make.log"; exit 1; }
        make install >/dev/null
    )
fi

# --------------------------------------------------------------- shim
# Everything static inside one shared object; only the API table is
# exported. -lz only when FFmpeg was built against zlib.
link_shim() {
    $CC $CFLAGS -DRBV_WEBP -fvisibility=hidden -shared \
        -I"$PREFIX/include" -I"$ROOT/apps/video" \
        -o "$OUT/librbvideo.so" "$HERE/rbvideo.c" \
        -L"$PREFIX/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil \
        -lwebpdemux -lwebp -lsharpyuv $1 \
        -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs \
        -Wl,-soname,librbvideo.so \
        -lpthread -lm
}
# zlib goes inside the object: the player's system may not have one, and
# a missing libz.so would make the whole library unloadable.
link_shim "$PREFIX/lib/libz.a" || link_shim -lz || link_shim ""
${CROSS}strip --strip-unneeded "$OUT/librbvideo.so"
ls -la "$OUT/librbvideo.so"
