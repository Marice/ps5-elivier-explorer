#!/usr/bin/env bash
# EliVier Explorer - Makes the demo film for the PC preview (host/demo/zz-demo-clip.mkv).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-video-demo.sh
#
# A moving test pattern, a tone and two invented subtitle lines, made with ffmpeg's own sources:
# no third-party material. The first line starts at 0, so that the preview shows it even though the
# preview's audio clock does not run. The name sorts last in the demo folder, so that adding it
# does not move the rows the tour walks over.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/host/demo/zz-demo-clip.mkv"
command -v ffmpeg >/dev/null || { echo "ffmpeg is required" >&2; exit 2; }
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT

cat >"$temporary/lines.srt" <<'LINES'
1
00:00:00,000 --> 00:00:02,500
A short film made of a pattern and a tone.

2
00:00:03,000 --> 00:00:05,500
The subtitles are a track inside the file.
LINES

cat >"$temporary/chapters.txt" <<'EOF'
;FFMETADATA1

[CHAPTER]
TIMEBASE=1/1000
START=0
END=3000
title=First half

[CHAPTER]
TIMEBASE=1/1000
START=3000
END=6000
title=Second half
EOF

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=960x540:rate=24:duration=6" \
    -f lavfi -i "sine=frequency=330:sample_rate=48000:duration=6" \
    -i "$temporary/lines.srt" -i "$temporary/chapters.txt" \
    -map_metadata 3 -map 0:v -map 1:a -map 2:s \
    -c:v libx264 -preset veryfast -crf 30 -g 24 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -c:s srt -metadata:s:a:0 language=eng -metadata:s:s:0 language=eng \
    "$out"
ls -l "$out"
