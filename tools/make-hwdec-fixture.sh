#!/usr/bin/env bash
# EliVier Explorer - Writes the H.264 picture the hwdec probe gives the hardware decoder.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-hwdec-fixture.sh
# Writes src/platform/ps5/hwdec_fixture.inc: one 1920x1080 H.264 High keyframe (Annex B, with its
# parameter sets) drawn from FFmpeg's test pattern, and the FNV-1a hash of its luma as FFmpeg
# decodes it. H.264 decoding is exact, so the console's decoder must give the same hash. The
# encoder is not bit-reproducible across FFmpeg versions; the committed file is the reference.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1920x1080:rate=1:duration=1" -frames:v 1 \
    -c:v libx264 -profile:v high -pix_fmt yuv420p -crf 30 -bsf:v h264_mp4toannexb -f h264 \
    "$work/picture.h264"
# The luma plane as decoded, untouched: yuv420p, of which the first 1920 x 1080 bytes are Y. (Not
# `-pix_fmt gray`: that converts limited range to full range, which no decoder does.)
ffmpeg -loglevel error -y -i "$work/picture.h264" -f rawvideo -pix_fmt yuv420p "$work/luma.raw"
python3 - "$work/picture.h264" "$work/luma.raw" "$root/src/platform/ps5/hwdec_fixture.inc" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
luma = open(sys.argv[2], "rb").read()[:1920 * 1080]
h = 0x811C9DC5
for b in luma:
    h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
with open(sys.argv[3], "w") as out:
    out.write("// EliVier Explorer - One H.264 keyframe for the hwdec probe (tools/make-hwdec-fixture.sh).\n")
    out.write("// Copyright (C) 2026 BlackBearReloaded\n// SPDX-License-Identifier: GPL-3.0-or-later\n")
    out.write("// Generated from FFmpeg's testsrc2 pattern; do not edit.\n\n")
    out.write("constexpr int kFixtureWidth = 1920;\nconstexpr int kFixtureHeight = 1080;\n")
    out.write(f"constexpr std::uint32_t kFixtureLumaHash = 0x{h:08X}u; // FNV-1a over 1920 x 1080 luma\n")
    out.write(f"alignas(64) constexpr std::uint8_t kFixtureH264[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        out.write("    " + ", ".join(f"0x{b:02X}" for b in data[i:i+16]) + ",\n")
    out.write("};\n")
print(len(data), f"0x{h:08X}")
PY
