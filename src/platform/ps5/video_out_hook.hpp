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

// A copy of what the hooks saw so far.
VideoOutState video_out_state();

// Writes the output's status (resolution, dynamic range, refresh) to the trace. False when no
// output is open or the call fails.
bool trace_video_out_status(const char *when);

} // namespace hui::ps5
