#!/usr/bin/env bash
# EliVier Explorer - Builds the libSceVideodec2 import stub the app links with `make HW_DECODE=1`.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-videodec2-stub.sh
# Prints the path of the stub (.deps/video/stubs/libSceVideodec2.so). Needs the SDK (make deps).

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
output="$root/.deps/video/stubs/libSceVideodec2.so"
[[ -x $sdk/bin/prospero-clang ]] || { echo "the PS5 payload SDK is missing; run make deps" >&2; exit 2; }

mkdir -p "$(dirname "$output")"
"$sdk/bin/prospero-clang" -shared -nostdlib -nodefaultlibs -fPIC -O2 -Wall -Werror \
    -Wl,-soname,libSceVideodec2.sprx -o "$output" \
    "$root/tooling/native/stubs/libSceVideodec2.c"
# The SONAME is the system library's file name, as in the SDK's own stubs (the converter turns
# .sprx into .prx for the app's list of needed libraries). Check the name and the symbol first.
"$sdk/bin/llvm-nm" -D "$output" | grep -q " T sceVideodec2QueryComputeMemoryInfo$" || {
    echo "the stub does not export sceVideodec2QueryComputeMemoryInfo" >&2; exit 2; }
printf '%s\n' "$output"
