#!/bin/bash
# Static FFmpeg for the cross build, what sc3_movies.cpp needs (TMB's kit flags plus avformat, swresample, swscale; the
# mpeg demuxer finds an SFD's streams through the raw adx, ac3 and mpegvideo demuxers):
#   MINGW_ROOT=<folder with bin/x86_64-w64-mingw32-gcc> scripts/build_win_ffmpeg.sh <FFmpeg source> <build dir> <install prefix>
set -e
src=$(realpath "$1"); mkdir -p "$2" "$3"; prefix=$(realpath "$3"); cd "$2"
"$src/configure" --prefix="$prefix" --arch=x86_64 --target-os=mingw32 --enable-cross-compile \
  --cross-prefix="$MINGW_ROOT/bin/x86_64-w64-mingw32-" --pkg-config=false \
  --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
  --disable-everything --disable-autodetect --disable-network --disable-avdevice --disable-avfilter \
  --enable-avcodec --enable-avutil --enable-avformat --enable-swresample --enable-swscale \
  --enable-decoder=mpeg1video --enable-decoder=mpeg2video --enable-decoder=adpcm_adx \
  --enable-parser=mpegvideo --enable-parser=adx --enable-parser=ac3 \
  --enable-demuxer=mpegps --enable-demuxer=mpegvideo --enable-demuxer=adx --enable-demuxer=ac3 \
  --enable-w32threads --disable-x86asm
make -j"$(nproc)"
make install
