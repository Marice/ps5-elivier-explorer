#!/usr/bin/env bash
# EliVier Explorer - Builds the pinned video libraries (FFmpeg, dav1d) for the PS5 into the ignored cache.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/setup-video-dependencies.sh
#
# Output (printed at the end): the include directory and the static archives
# for APP_INCLUDE_PATHS and APP_STATIC_ARCHIVES. Needs the SDK from `make deps`.
#
# FFmpeg is configured from an allow-list: no network, no encoders, no muxers,
# no filters, no iconv, no hardware acceleration. The video, audio and subtitle
# decoders and the demuxers it enables are listed below; change the lists when a
# format is added (and the licence notice with it). The libraries are built with
# the same code-generation flags as the app (tooling/prospero-clang18).

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.deps/video"
sources="$cache/src"
tools="$cache/tools"
prefix="$cache/root/usr"
sdk="$root/.deps/native/ps5-payload-sdk"
build_jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || printf '2')}

ffmpeg_version=7.1.5
ffmpeg_hash=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
dav1d_version=1.5.5
dav1d_hash=ca96b1d0ba3f01f55de7231d775892b64994ba061aaeeaaa4f662a390b3abf9e
nasm_version=2.16.03
nasm_hash=1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148
meson_version=1.7.0
meson_hash=ae3f12953045f3c7c60e27f2af1ad862f14dee125b4ed9bcb8a842a5080dbf85
stamp="$prefix/.versions"
expected="ffmpeg=$ffmpeg_version dav1d=$dav1d_version nasm=$nasm_version meson=$meson_version"

for command in wget sha256sum tar xz make python3 pip3 pkg-config; do
    command -v "$command" >/dev/null || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
[[ -x $sdk/bin/prospero-clang ]] || {
    echo "the PS5 payload SDK is missing; run make deps first" >&2
    exit 2
}

if [[ -f $stamp && $(<"$stamp") == "$expected" && -f $prefix/lib/libavcodec.a &&
    -f $prefix/lib/libdav1d.a ]]; then
    echo "==> [video] FFmpeg $ffmpeg_version and dav1d $dav1d_version are up to date" >&2
else
    mkdir -p "$sources" "$tools"

    # fetch URL HASH FILE: the archive is checked against its pinned hash and a
    # file that does not match is deleted, never used.
    fetch() {
        local url=$1 hash=$2 file=$sources/$3
        if [[ -f $file ]] && ! printf '%s  %s\n' "$hash" "$file" | sha256sum --check --strict >/dev/null 2>&1; then
            rm -f -- "$file"
        fi
        if [[ ! -f $file ]]; then
            echo "==> [video] Downloading $3" >&2
            wget -q "$url" -O "$file.download"
            printf '%s  %s\n' "$hash" "$file.download" | sha256sum --check --strict >&2
            mv "$file.download" "$file"
        fi
    }
    fetch "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" "$ffmpeg_hash" "ffmpeg-$ffmpeg_version.tar.xz"
    fetch "https://download.videolan.org/pub/videolan/dav1d/$dav1d_version/dav1d-$dav1d_version.tar.xz" "$dav1d_hash" "dav1d-$dav1d_version.tar.xz"
    fetch "https://www.nasm.us/pub/nasm/releasebuilds/$nasm_version/nasm-$nasm_version.tar.xz" "$nasm_hash" "nasm-$nasm_version.tar.xz"
    fetch "https://files.pythonhosted.org/packages/py3/m/meson/meson-$meson_version-py3-none-any.whl" "$meson_hash" "meson-$meson_version-py3-none-any.whl"

    # Host tools, kept in the cache: the system has neither nasm nor meson.
    if [[ ! -x $tools/nasm/bin/nasm ]]; then
        echo "==> [video] Building nasm $nasm_version (host tool)" >&2
        rm -rf -- "$sources/nasm-$nasm_version"
        tar -xJf "$sources/nasm-$nasm_version.tar.xz" -C "$sources"
        (cd "$sources/nasm-$nasm_version" && ./configure --prefix="$tools/nasm" && make -j "$build_jobs" && make install) \
            >"$cache/nasm.log" 2>&1
    fi
    if [[ ! -d $tools/meson/mesonbuild ]]; then
        echo "==> [video] Installing meson $meson_version (host tool)" >&2
        pip3 install --quiet --no-deps --no-index --target "$tools/meson" \
            "$sources/meson-$meson_version-py3-none-any.whl"
    fi
    export PATH="$tools/nasm/bin:$PATH"
    meson() { PYTHONPATH="$tools/meson" python3 -m mesonbuild.mesonmain "$@"; }

    # The SDK driver links; the flags below are the app's (tooling/prospero-clang18).
    target_flags="-O2 -fPIC -fno-stack-protector -fno-plt -femulated-tls"
    cc="$sdk/bin/prospero-clang"
    rm -rf -- "$prefix"
    mkdir -p "$prefix"

    echo "==> [video] Building dav1d $dav1d_version" >&2
    dav1d_dir="$cache/build-dav1d"
    rm -rf -- "$dav1d_dir" "$sources/dav1d-$dav1d_version"
    tar -xJf "$sources/dav1d-$dav1d_version.tar.xz" -C "$sources"
    cat >"$cache/ps5-cross.ini" <<EOF
[binaries]
c = '$cc'
ar = '$sdk/bin/prospero-ar'
strip = '$sdk/bin/prospero-strip'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['-fno-stack-protector', '-fno-plt', '-femulated-tls']

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'znver2'
endian = 'little'
EOF
    (
        cd "$sources/dav1d-$dav1d_version"
        meson setup "$dav1d_dir" --cross-file "$cache/ps5-cross.ini" --prefix="$prefix" --libdir=lib \
            --default-library=static --buildtype=release \
            -Denable_tools=false -Denable_tests=false -Denable_examples=false -Dbitdepths=8,16
        meson compile -C "$dav1d_dir" -j "$build_jobs"
        meson install -C "$dav1d_dir"
    ) >"$cache/dav1d.log" 2>&1

    echo "==> [video] Building FFmpeg $ffmpeg_version" >&2
    ffmpeg_dir="$cache/build-ffmpeg"
    rm -rf -- "$ffmpeg_dir" "$sources/ffmpeg-$ffmpeg_version"
    tar -xJf "$sources/ffmpeg-$ffmpeg_version.tar.xz" -C "$sources"
    mkdir -p "$ffmpeg_dir"
    pacbrew="$root/.deps/pacbrew/v0.40.2/sysroot/user/homebrew"
    video_decoders=h264,hevc,vp8,vp9,libdav1d,mpeg1video,mpeg2video,mpeg4,msmpeg4v3,vc1,wmv1,wmv2,wmv3,theora,mjpeg
    audio_decoders=aac,aac_latm,ac3,eac3,dca,truehd,mlp,flac,alac,opus,vorbis,mp1,mp1float,mp2,mp2float,mp3,mp3float,wmav1,wmav2,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_f32le,pcm_u8,pcm_bluray,pcm_dvd
    subtitle_decoders=subrip,srt,ass,ssa,webvtt,movtext,text,microdvd,subviewer,subviewer1,dvdsub,dvbsub,pgssub
    demuxers=matroska,mov,avi,mpegts,mpegps,flv,ogg,asf,wav,flac,mp3,aac,ac3,eac3,dts,dtshd,truehd,ivf,mpegvideo,h264,hevc,srt,ass,webvtt,microdvd,subviewer,subviewer1
    parsers=h264,hevc,vp8,vp9,av1,mpegvideo,mpeg4video,vc1,aac,aac_latm,ac3,dca,flac,opus,vorbis,mpegaudio,dvdsub,dvbsub
    bsfs=h264_mp4toannexb,hevc_mp4toannexb,vp9_superframe_split,vp9_superframe,aac_adtstoasc,extract_extradata
    (
        cd "$ffmpeg_dir"
        export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
        export PKG_CONFIG_PATH=
        "$sources/ffmpeg-$ffmpeg_version/configure" --prefix="$prefix" \
            --enable-cross-compile --target-os=freebsd --arch=x86_64 --cpu=znver2 \
            --cc="$cc" --cxx="$sdk/bin/prospero-clang++" --ar="$sdk/bin/prospero-ar" \
            --ranlib="$sdk/bin/prospero-ranlib" --nm="$sdk/bin/prospero-nm" --strip="$sdk/bin/prospero-strip" \
            --pkg-config=pkg-config --pkg-config-flags=--static \
            --x86asmexe="$tools/nasm/bin/nasm" \
            --extra-cflags="$target_flags -I$pacbrew/include" --extra-ldflags="-L$pacbrew/lib" \
            --enable-static --disable-shared --enable-pic \
            --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug \
            --disable-network --disable-iconv --disable-avdevice --disable-avfilter --disable-swscale \
            --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
            --enable-zlib --enable-libdav1d \
            --enable-decoder="$video_decoders,$audio_decoders,$subtitle_decoders" \
            --enable-demuxer="$demuxers" --enable-parser="$parsers" --enable-bsf="$bsfs"
        make -j "$build_jobs"
        make install
    ) >"$cache/ffmpeg.log" 2>&1

    printf '%s\n' "$expected" >"$stamp"
fi

printf 'VIDEO_INCLUDE=%s\nVIDEO_ARCHIVES=' "$prefix/include"
for archive in libavformat libavcodec libswresample libavutil libdav1d; do
    printf '%s ' "$prefix/lib/$archive.a"
done
printf '\n'
