// EliVier Explorer - What the OpenGL runtime does with the console's video output, watched.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ps5-opengl runtime opens VideoOut and registers the scanout buffers itself and keeps the
// handle to itself. HDR output needs that handle and the buffers' attributes (D68): opening a
// second VideoOut panics the console. The build routes the runtime's calls through the wrappers
// in video_out_hook.cpp (--wrap, as for the splash screen), which pass every call on unchanged and
// remember what went by. Every call is written to the trace.

#pragma once

#include <cstdint>

namespace hui::ps5
{

struct VideoOutState
{
    int handle = -1;         // -1 while no output is open
    int set_index = -1;      // the buffer set the runtime registered
    int buffer_count = 0;
    // The attributes the runtime gave its buffers (sceVideoOutSetBufferAttribute2's arguments).
    bool attributes_known = false;
    std::uint64_t pixel_format = 0;
    std::uint32_t tiling = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t option = 0;
    std::uint32_t dcc_control = 0;
    std::uint64_t dcc_clear_colour = 0;
};

// The pixel formats of the scanout buffers: what the runtime uses (8-bit SDR), and HDR10 (10:10:10:2,
// BT.2020, PQ), as EVO Player, VLC and MPC for PS5 retype theirs (D68).
constexpr std::uint64_t kVideoOutFormatSdr = 0x8000000000000000ull;
constexpr std::uint64_t kVideoOutFormatHdr10 = 0x8100070400000000ull;

// A copy of what the hooks saw so far (the runtime's own calls; set_format() is not counted).
VideoOutState video_out_state();

// Retypes the runtime's registered buffers to `format`, keeping its tiling, size and other
// attributes, and submits the change (sceVideoOutSubmitChangeBufferAttribute2). False, and
// nothing changed, while the runtime has not opened the output and registered its buffers.
bool video_out_set_format(std::uint64_t format);

// Writes the output's status (resolution, dynamic range, refresh) to the trace. False when no
// output is open or the call fails.
bool trace_video_out_status(const char *when);

} // namespace hui::ps5
