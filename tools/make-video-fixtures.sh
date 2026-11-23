#!/usr/bin/env bash
# EliVier Explorer - Makes the tiny synthetic clips the video tests read (tests/fixtures/video).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-video-fixtures.sh
#
# Needs an ffmpeg with libx264, libx265, libvpx-vp9, libaom-av1 and libopus (the build container
# has one). Everything is generated from FFmpeg's own test sources: a moving test pattern, a sine
# tone and a few invented subtitle lines. No third-party material. The clips are 160 x 90 and two
# seconds long, a few tens of kilobytes each, and are checked in so that the tests need no encoder.
# The encoders are not bit-reproducible: running this again gives files with other bytes but the
# same streams. Commit a regenerated clip only when its content has to change.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/tests/fixtures/video"
command -v ffmpeg >/dev/null || { echo "ffmpeg is required" >&2; exit 2; }
mkdir -p "$out"

video="testsrc2=size=160x90:rate=24:duration=2"
tone() { printf 'sine=frequency=%s:sample_rate=48000:duration=2' "$1"; }
quiet=(-hide_banner -loglevel error -y -fflags +bitexact -flags:v +bitexact -flags:a +bitexact)

cat >"$out/lines.srt" <<'EOF'
1
00:00:00,200 --> 00:00:00,900
Hello, this is the first line.

2
00:00:01,000 --> 00:00:01,800
And this is the second line.
EOF

cat >"$out/styled.ass" <<'EOF'
[Script Info]
ScriptType: v4.00+
PlayResX: 160
PlayResY: 90

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Sans,12,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,1,0,2,4,4,4,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.20,0:00:00.90,Default,,0,0,0,,Styled {\i1}line{\i0} one
Dialogue: 0,0:00:01.00,0:00:01.80,Default,,0,0,0,,Styled line two
EOF

# H.264 8-bit in MP4 with AAC. A keyframe every half second, so that seeking has places to land.
ffmpeg "${quiet[@]}" -f lavfi -i "$video" -f lavfi -i "$(tone 440)" \
    -c:v libx264 -preset veryfast -crf 36 -pix_fmt yuv420p -g 12 -sc_threshold 0 \
    -c:a aac -b:a 48k -shortest \
    -movflags +faststart "$out/h264-aac.mp4"

# H.264 4:4:4 (High 4:4:4 Predictive): full-resolution chroma, to prove the frame format is general.
ffmpeg "${quiet[@]}" -f lavfi -i "$video" \
    -c:v libx264 -preset veryfast -crf 30 -pix_fmt yuv444p "$out/h264-yuv444.mp4"

# HEVC Main10 tagged as HDR10 (BT.2020, PQ) in Matroska with FLAC.
ffmpeg "${quiet[@]}" -f lavfi -i "$video" -f lavfi -i "$(tone 440)" \
    -c:v libx265 -preset veryfast -crf 34 -pix_fmt yuv420p10le -x265-params log-level=error \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv \
    -c:a flac -ac 1 -shortest "$out/hevc-main10-hdr.mkv"

# VP9 with Opus in WebM.
ffmpeg "${quiet[@]}" -f lavfi -i "$video" -f lavfi -i "$(tone 440)" \
    -c:v libvpx-vp9 -b:v 0 -crf 44 -deadline realtime -cpu-used 8 -pix_fmt yuv420p \
    -c:a libopus -b:a 32k -shortest "$out/vp9-opus.webm"

# AV1 in Matroska.
ffmpeg "${quiet[@]}" -f lavfi -i "$video" \
    -c:v libaom-av1 -crf 48 -cpu-used 8 -pix_fmt yuv420p "$out/av1.mkv"

# Two audio tracks (English, Dutch) and two subtitle tracks (SRT, ASS) in one Matroska file.
ffmpeg "${quiet[@]}" -f lavfi -i "$video" -f lavfi -i "$(tone 440)" -f lavfi -i "$(tone 880)" \
    -i "$out/lines.srt" -i "$out/styled.ass" \
    -map 0:v -map 1:a -map 2:a -map 3:s -map 4:s \
    -c:v libx264 -preset veryfast -crf 36 -pix_fmt yuv420p -c:a aac -b:a 32k -c:s copy -shortest \
    -c:s:0 srt -c:s:1 ass \
    -metadata:s:a:0 language=eng -metadata:s:a:0 title="English" \
    -metadata:s:a:1 language=nld -metadata:s:a:1 title="Nederlands" \
    -metadata:s:s:0 language=eng -metadata:s:s:1 language=nld \
    "$out/multi-track.mkv"

ls -l "$out"
