// EliVier Explorer - What the OpenGL runtime does with the console's video output, watched.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/video_out_hook.hpp"

#include "explorer/trace.hpp"

#include <atomic>

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
int sceVideoOutSubmitChangeBufferAttribute2(int handle, int set_index, const void *attribute,
                                            const void *option);
}

namespace hui::ps5
{

namespace
{

// Plain atomics only, nothing with a destructor and no lock: the runtime closes the output while
// the process ends, after static objects may already be gone. A mutex and a trace line in the
// close hook are the likely reason one run did not end and the next start hung (D69).
std::atomic<int> g_handle{-1};
std::atomic<int> g_set_index{-1};
std::atomic<int> g_buffer_count{0};
std::atomic<bool> g_attributes_known{false};
std::atomic<std::uint64_t> g_pixel_format{0};
std::atomic<std::uint32_t> g_tiling{0};
std::atomic<std::uint32_t> g_width{0};
std::atomic<std::uint32_t> g_height{0};
std::atomic<std::uint64_t> g_option{0};
std::atomic<std::uint32_t> g_dcc_control{0};
std::atomic<std::uint64_t> g_dcc_clear_colour{0};
// Trace lines from the hooks: the first few only (the runtime's start-up), never at the end.
std::atomic<int> g_traced{0};
constexpr int kMaxTraced = 6;

bool may_trace()
{
    return g_traced.fetch_add(1) < kMaxTraced;
}

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
    VideoOutState state;
    state.handle = g_handle.load();
    state.set_index = g_set_index.load();
    state.buffer_count = g_buffer_count.load();
    state.attributes_known = g_attributes_known.load();
    state.pixel_format = g_pixel_format.load();
    state.tiling = g_tiling.load();
    state.width = g_width.load();
    state.height = g_height.load();
    state.option = g_option.load();
    state.dcc_control = g_dcc_control.load();
    state.dcc_clear_colour = g_dcc_clear_colour.load();
    return state;
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

bool video_out_set_format(std::uint64_t format)
{
    const VideoOutState state = video_out_state();
    if (state.handle < 0 || state.set_index < 0 || !state.attributes_known)
    {
        explorer::trace("video out: format 0x%016llx not set: no buffers registered yet",
                        static_cast<unsigned long long>(format));
        return false;
    }
    // Room for the attribute structure, whatever its exact size (it is filled by the library).
    alignas(16) std::uint8_t attribute[256] = {};
    __real_sceVideoOutSetBufferAttribute2(attribute, format, state.tiling, state.width,
                                          state.height, state.option, state.dcc_control,
                                          state.dcc_clear_colour);
    const int rc =
        sceVideoOutSubmitChangeBufferAttribute2(state.handle, state.set_index, attribute, nullptr);
    explorer::trace("video out: buffers retyped to 0x%016llx = 0x%08x",
                    static_cast<unsigned long long>(format), static_cast<unsigned>(rc));
    return rc == 0;
}

} // namespace hui::ps5

using namespace hui::ps5;

extern "C" int __wrap_sceVideoOutOpen(int user, int bus_type, int index, const void *parameters)
{
    const int handle = __real_sceVideoOutOpen(user, bus_type, index, parameters);
    if (handle >= 0)
        g_handle.store(handle);
    if (may_trace())
        hui::explorer::trace("video out: open(user %d, bus %d, index %d) = %d (0x%08x)", user,
                         bus_type, index, handle, static_cast<unsigned>(handle));
    return handle;
}

// Called while the process ends: it only forgets the handle (no lock, no trace, no allocation).
extern "C" int __wrap_sceVideoOutClose(int handle)
{
    int expected = handle;
    if (g_handle.compare_exchange_strong(expected, -1))
        g_set_index.store(-1);
    return __real_sceVideoOutClose(handle);
}

extern "C" int __wrap_sceVideoOutRegisterBuffers2(int handle, int set_index,
                                                  int buffer_index_start, const void *buffers,
                                                  int buffer_count, const void *attribute,
                                                  int category, void *option)
{
    const int rc = __real_sceVideoOutRegisterBuffers2(handle, set_index, buffer_index_start,
                                                      buffers, buffer_count, attribute, category,
                                                      option);
    if (rc >= 0)
    {
        g_set_index.store(rc > 0 ? rc : set_index);
        g_buffer_count.store(buffer_count);
    }
    if (may_trace())
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
    g_pixel_format.store(pixel_format);
    g_tiling.store(tiling);
    g_width.store(width);
    g_height.store(height);
    g_option.store(option);
    g_dcc_control.store(dcc_control);
    g_dcc_clear_colour.store(dcc_clear_colour);
    g_attributes_known.store(true);
    if (may_trace())
        hui::explorer::trace("video out: buffer attribute format 0x%016llx tiling %u %ux%u option "
                         "0x%llx dcc %u clear 0x%llx",
                         static_cast<unsigned long long>(pixel_format), tiling, width, height,
                         static_cast<unsigned long long>(option), dcc_control,
                         static_cast<unsigned long long>(dcc_clear_colour));
}
