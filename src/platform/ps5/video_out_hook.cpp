// EliVier Explorer - What the OpenGL runtime does with the console's video output, watched.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/video_out_hook.hpp"

#include "explorer/trace.hpp"

#include <mutex>

// The calls as the console's library exports them (layouts from EVO Player and VLC for PS5,
// D58): the attribute structure is filled by SetBufferAttribute2 from its arguments and is
// treated here as opaque.
extern "C"
{
int __real_sceVideoOutOpen(int user, int bus_type, int index, const void *parameters);
int __real_sceVideoOutClose(int handle);
int __real_sceVideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start,
                                       const void *buffers, int buffer_count,
                                       const void *attribute, int category, void *option);
void __real_sceVideoOutSetBufferAttribute2(void *attribute, std::uint64_t pixel_format,
                                           std::uint32_t tiling, std::uint32_t width,
                                           std::uint32_t height, std::uint64_t option,
                                           std::uint32_t dcc_control,
                                           std::uint64_t dcc_clear_colour);
int sceVideoOutGetOutputStatus(int handle, void *status);
}

namespace hui::ps5
{

namespace
{

std::mutex g_mutex;
VideoOutState g_state;

// sceVideoOutGetOutputStatus's answer, 48 bytes (as VLC and EVO read it).
struct OutputStatus
{
    std::uint32_t resolution;
    std::uint32_t dynamic_range; // 1 SDR, 2 HDR
    std::uint64_t refresh;
    std::uint64_t flags; // bit 0: HDR
    std::uint64_t reserved[3];
};
static_assert(sizeof(OutputStatus) == 48);

} // namespace

VideoOutState video_out_state()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

bool trace_video_out_status(const char *when)
{
    const int handle = video_out_state().handle;
    if (handle < 0)
    {
        explorer::trace("video out %s: no output open", when);
        return false;
    }
    OutputStatus status{};
    const int rc = sceVideoOutGetOutputStatus(handle, &status);
    explorer::trace("video out %s: status rc 0x%08x resolution %u dynamic range %u refresh 0x%llx "
                    "flags 0x%llx",
                    when, static_cast<unsigned>(rc), status.resolution, status.dynamic_range,
                    static_cast<unsigned long long>(status.refresh),
                    static_cast<unsigned long long>(status.flags));
    return rc == 0;
}

} // namespace hui::ps5

using hui::ps5::g_mutex;
using hui::ps5::g_state;

extern "C" int __wrap_sceVideoOutOpen(int user, int bus_type, int index, const void *parameters)
{
    const int handle = __real_sceVideoOutOpen(user, bus_type, index, parameters);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (handle >= 0)
            g_state.handle = handle;
    }
    hui::explorer::trace("video out: open(user %d, bus %d, index %d) = %d (0x%08x)", user,
                         bus_type, index, handle, static_cast<unsigned>(handle));
    return handle;
}

extern "C" int __wrap_sceVideoOutClose(int handle)
{
    const int rc = __real_sceVideoOutClose(handle);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (handle == g_state.handle)
        {
            g_state.handle = -1;
            g_state.set_index = -1;
        }
    }
    hui::explorer::trace("video out: close(%d) = 0x%08x", handle, static_cast<unsigned>(rc));
    return rc;
}

extern "C" int __wrap_sceVideoOutRegisterBuffers2(int handle, int set_index,
                                                  int buffer_index_start, const void *buffers,
                                                  int buffer_count, const void *attribute,
                                                  int category, void *option)
{
    const int rc = __real_sceVideoOutRegisterBuffers2(handle, set_index, buffer_index_start,
                                                      buffers, buffer_count, attribute, category,
                                                      option);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (rc >= 0)
        {
            g_state.set_index = rc > 0 ? rc : set_index;
            g_state.buffer_count = buffer_count;
        }
    }
    hui::explorer::trace("video out: register buffers(handle %d, set %d, first %d, count %d, "
                         "category %d) = 0x%08x",
                         handle, set_index, buffer_index_start, buffer_count, category,
                         static_cast<unsigned>(rc));
    return rc;
}

extern "C" void __wrap_sceVideoOutSetBufferAttribute2(void *attribute, std::uint64_t pixel_format,
                                                      std::uint32_t tiling, std::uint32_t width,
                                                      std::uint32_t height, std::uint64_t option,
                                                      std::uint32_t dcc_control,
                                                      std::uint64_t dcc_clear_colour)
{
    __real_sceVideoOutSetBufferAttribute2(attribute, pixel_format, tiling, width, height, option,
                                          dcc_control, dcc_clear_colour);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state.attributes_known = true;
        g_state.pixel_format = pixel_format;
        g_state.tiling = tiling;
        g_state.width = width;
        g_state.height = height;
        g_state.option = option;
        g_state.dcc_control = dcc_control;
        g_state.dcc_clear_colour = dcc_clear_colour;
    }
    hui::explorer::trace("video out: buffer attribute format 0x%016llx tiling %u %ux%u option "
                         "0x%llx dcc %u clear 0x%llx",
                         static_cast<unsigned long long>(pixel_format), tiling, width, height,
                         static_cast<unsigned long long>(option), dcc_control,
                         static_cast<unsigned long long>(dcc_clear_colour));
}
