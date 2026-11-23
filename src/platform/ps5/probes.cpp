// EliVier Explorer - Console probes: questions put to the PS5 before video is built on the answers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/probes.hpp"

#include "platform/ps5/system.hpp"
#include "video/colour.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// The kernel and sysmodule calls, declared as the working EVO Player code declares them (its
// evo_vdec_native.c); only calls whose signature that code confirms are used. They live in
// libkernel and libSceSysmodule, which the app already links.
extern "C"
{
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int type, int64_t *physical_start);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t physical_start, size_t length);
int sceKernelMunmap(void *address, size_t length);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelConfiguredFlexibleMemorySize(size_t *size);
int sceSysmoduleLoadModule(uint16_t id);
// Standard module calls (the PS4 SDK's, unchanged on the PS5).
int sceKernelLoadStartModule(const char *path, size_t arguments, const void *argument_block,
                             unsigned flags, const void *options, int *result);
int sceKernelDlsym(int module, const char *symbol, void **address);

void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes, std::size_t *blocks,
                    std::size_t *failures);
}

namespace hui::ps5
{

namespace
{

using probe::Report;

constexpr std::size_t kMiB = std::size_t{1} << 20;

double seconds_between(std::int64_t from_us, std::int64_t to_us)
{
    return static_cast<double>(to_us - from_us) / 1e6;
}

// Writes the report lines that were not logged yet to the app log, so a crash keeps them.
class LogFollower
{
  public:
    explicit LogFollower(const Report &report) : report_(report)
    {
    }
    void flush()
    {
        for (; logged_ < report_.lines().size(); ++logged_)
            sys::log("[HUI] %s", report_.lines()[logged_].c_str());
    }

  private:
    const Report &report_;
    std::size_t logged_ = 0;
};

// ---- memory ----------------------------------------------------------------------------------

// Direct memory is what a hardware decoder's frame pools and a fast software decode need, and the
// 128 MiB app heap cannot hold them. Block sizes double from 16 MiB; each is allocated, mapped,
// written with a pattern, read back and released. The first failure ends that memory type's
// ladder, so a console short of memory is not pushed further.
void probe_memory_type(Report &report, int type, std::size_t max_bytes, int64_t limit)
{
    const std::string name = "t" + std::to_string(type);
    std::size_t best = 0;
    for (const std::size_t size : probe::allocation_ladder(max_bytes))
    {
        const std::string label = name + "_" + std::to_string(size / kMiB) + "mib";
        int64_t start = -1;
        int rc = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, type, &start);
        if (rc != 0)
        {
            report.add_hex("memory", label + "_allocate", static_cast<std::uint32_t>(rc));
            break;
        }
        void *address = nullptr;
        // 0x33: CPU read and write, GPU all (what a decoder's shared pool needs).
        rc = sceKernelMapDirectMemory(&address, size, 0x33, 0, start, 0x4000);
        if (rc != 0 || address == nullptr)
        {
            report.add_hex("memory", label + "_map", static_cast<std::uint32_t>(rc));
            sceKernelReleaseDirectMemory(start, size);
            break;
        }
        const std::int64_t write_from = sys::monotonic_us();
        probe::fill_pattern(address, size, 0xC0FFEEu + static_cast<std::uint32_t>(type));
        const std::int64_t write_to = sys::monotonic_us();
        const std::size_t bad =
            probe::find_mismatch(address, size, 0xC0FFEEu + static_cast<std::uint32_t>(type));
        const std::int64_t read_to = sys::monotonic_us();

        char text[160];
        std::snprintf(text, sizeof(text), "%s write=%.0f MiB/s read_and_check=%.0f MiB/s",
                      bad == size ? "ok" : "MISMATCH",
                      probe::mib_per_second(size, seconds_between(write_from, write_to)),
                      probe::mib_per_second(size, seconds_between(write_to, read_to)));
        report.add("memory", label, text);
        if (bad != size)
            report.add("memory", label + "_first_bad_byte", static_cast<std::int64_t>(bad));
        else
            best = size;
        sceKernelMunmap(address, size);
        sceKernelReleaseDirectMemory(start, size);
        if (bad != size)
            break;
    }
    report.add("memory", name + "_largest_ok_mib", static_cast<std::int64_t>(best / kMiB));
}

void probe_memory(Report &report)
{
    const std::int64_t limit = sceKernelGetDirectMemorySize();
    report.add("memory", "direct_total_mib", limit / static_cast<std::int64_t>(kMiB));
    std::size_t available = 0;
    std::size_t configured = 0;
    const int rc_available = sceKernelAvailableFlexibleMemorySize(&available);
    const int rc_configured = sceKernelConfiguredFlexibleMemorySize(&configured);
    report.add("memory", "flexible_available_mib",
               rc_available == 0 ? static_cast<std::int64_t>(available / kMiB) : -1);
    report.add("memory", "flexible_configured_mib",
               rc_configured == 0 ? static_cast<std::int64_t>(configured / kMiB) : -1);
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    report.add("memory", "app_heap_live_mib", static_cast<std::int64_t>(live / kMiB));
    report.add("memory", "app_heap_peak_mib", static_cast<std::int64_t>(peak / kMiB));

    // Type 12 is what EVO Player uses for decoder pools; type 11 for fast CPU memory. Type 3
    // (uncached) is left out: it is known to be slow and not wanted.
    probe_memory_type(report, 12, 1024 * kMiB, limit);
    probe_memory_type(report, 11, 256 * kMiB, limit);
}

// ---- upload ----------------------------------------------------------------------------------

struct PlaneShape
{
    int width;
    int height;
    GLenum internal_format;
    GLenum format;
    GLenum type;
    std::size_t bytes;
};

struct FrameShape
{
    const char *name;
    PlaneShape luma;
    PlaneShape chroma;
};

constexpr FrameShape kFrames[] = {
    {"nv12_1080p",
     {1920, 1080, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1920u * 1080u},
     {960, 540, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 960u * 540u * 2u}},
    {"nv12_4k",
     {3840, 2160, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 3840u * 2160u},
     {1920, 1080, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 1920u * 1080u * 2u}},
    // 10-bit video: every sample a 16-bit word.
    {"p010_4k",
     {3840, 2160, GL_R16, GL_RED, GL_UNSIGNED_SHORT, 3840u * 2160u * 2u},
     {1920, 1080, GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 1920u * 1080u * 4u}},
};

constexpr int kWarmup = 5;
constexpr int kRepeats = 60;
constexpr std::size_t kHeapGuardBytes = 100 * kMiB; // the app heap is 128 MiB

GLuint make_texture(const PlaneShape &plane)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, plane.internal_format, plane.width, plane.height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return texture;
}

void upload_plane(GLuint texture, const PlaneShape &plane, const void *data)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plane.width, plane.height, plane.format, plane.type,
                    data);
}

// One method of getting a frame to the GPU, timed: the cost of the calls, and the cost until the
// GPU is done with them (glFinish). A 4K60 frame has 16.7 ms for everything.
void time_method(Report &report, const FrameShape &frame, GLuint luma, GLuint chroma,
                 const unsigned char *source, bool through_buffer, GLuint buffer)
{
    probe::TimingStats calls;
    probe::TimingStats total;
    const std::size_t all_bytes = frame.luma.bytes + frame.chroma.bytes;
    for (int i = 0; i < kWarmup + kRepeats; ++i)
    {
        const std::int64_t begin = sys::monotonic_us();
        if (through_buffer)
        {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
            void *mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
                                            static_cast<GLsizeiptr>(all_bytes),
                                            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (mapped == nullptr)
            {
                report.note("upload", std::string(frame.name) + " buffer map failed");
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                return;
            }
            std::memcpy(mapped, source, all_bytes);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            upload_plane(luma, frame.luma, nullptr);
            const auto chroma_offset = static_cast<std::uintptr_t>(frame.luma.bytes);
            upload_plane(chroma, frame.chroma, reinterpret_cast<const void *>(chroma_offset));
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        else
        {
            upload_plane(luma, frame.luma, source);
            upload_plane(chroma, frame.chroma, source + frame.luma.bytes);
        }
        const std::int64_t called = sys::monotonic_us();
        glFinish();
        const std::int64_t done = sys::monotonic_us();
        if (i >= kWarmup)
        {
            calls.add(static_cast<double>(called - begin) / 1000.0);
            total.add(static_cast<double>(done - begin) / 1000.0);
        }
    }
    char text[200];
    std::snprintf(text, sizeof(text),
                  "calls avg=%.2f p95=%.2f ms; done avg=%.2f p95=%.2f max=%.2f ms (n=%zu)",
                  calls.average(), calls.percentile(0.95), total.average(), total.percentile(0.95),
                  total.maximum(), total.count());
    report.add("upload", std::string(frame.name) + (through_buffer ? "_buffer" : "_direct"), text);
}

void probe_upload(Report &report)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (const FrameShape &frame : kFrames)
    {
        std::size_t live = 0;
        std::size_t peak = 0;
        std::size_t blocks = 0;
        std::size_t failures = 0;
        hui_heap_stats(&live, &peak, &blocks, &failures);
        const std::size_t all_bytes = frame.luma.bytes + frame.chroma.bytes;
        if (failures > 0 || live + all_bytes > kHeapGuardBytes)
        {
            report.note("upload", std::string(frame.name) +
                                      " skipped: too little app heap for a source frame");
            continue;
        }
        auto *source = static_cast<unsigned char *>(std::malloc(all_bytes));
        if (source == nullptr)
        {
            report.note("upload",
                        std::string(frame.name) + " skipped: no memory for a source frame");
            continue;
        }
        probe::fill_pattern(source, all_bytes, 0x5EED);
        report.add("upload", std::string(frame.name) + "_frame_mib",
                   static_cast<std::int64_t>((all_bytes + kMiB - 1) / kMiB));
        const GLuint luma = make_texture(frame.luma);
        const GLuint chroma = make_texture(frame.chroma);
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(all_bytes), nullptr,
                     GL_STREAM_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        time_method(report, frame, luma, chroma, source, false, 0);
        time_method(report, frame, luma, chroma, source, true, buffer);
        report.add_hex("upload", std::string(frame.name) + "_gl_error", glGetError());

        glDeleteBuffers(1, &buffer);
        glDeleteTextures(1, &luma);
        glDeleteTextures(1, &chroma);
        std::free(source);
    }
}

// ---- egl -------------------------------------------------------------------------------------

// A long list of words as several lines of at most 190 characters.
void add_words(Report &report, const std::string &key, const char *words)
{
    if (words == nullptr)
    {
        report.add("egl", key, "(none)");
        return;
    }
    std::string line;
    int part = 0;
    const std::string all(words);
    std::size_t at = 0;
    while (at < all.size())
    {
        std::size_t end = all.find(' ', at);
        if (end == std::string::npos)
            end = all.size();
        const std::string word = all.substr(at, end - at);
        if (!line.empty() && line.size() + 1 + word.size() > 190)
        {
            report.add("egl", key + "_" + std::to_string(part++), line);
            line.clear();
        }
        if (!word.empty())
            line += (line.empty() ? "" : " ") + word;
        at = end + 1;
    }
    if (!line.empty())
        report.add("egl", key + "_" + std::to_string(part), line);
}

bool has_word(const char *words, const char *word)
{
    if (words == nullptr)
        return false;
    const std::size_t length = std::strlen(word);
    for (const char *at = words; (at = std::strstr(at, word)) != nullptr; at += length)
    {
        const bool starts = at == words || at[-1] == ' ';
        const bool ends = at[length] == ' ' || at[length] == '\0';
        if (starts && ends)
            return true;
    }
    return false;
}

void probe_egl(Report &report)
{
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY)
    {
        report.note("egl", "no current display");
        return;
    }
    report.add("egl", "vendor", eglQueryString(display, EGL_VENDOR) != nullptr
                                    ? eglQueryString(display, EGL_VENDOR)
                                    : "(null)");
    report.add("egl", "version", eglQueryString(display, EGL_VERSION) != nullptr
                                     ? eglQueryString(display, EGL_VERSION)
                                     : "(null)");
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    add_words(report, "extensions", extensions);
    // The ones HDR output would rest on, one answer each.
    static const char *const kWanted[] = {"EGL_KHR_gl_colorspace",
                                          "EGL_EXT_gl_colorspace_bt2020_pq",
                                          "EGL_EXT_gl_colorspace_bt2020_hlg",
                                          "EGL_EXT_gl_colorspace_bt2020_linear",
                                          "EGL_EXT_gl_colorspace_scrgb",
                                          "EGL_EXT_gl_colorspace_scrgb_linear",
                                          "EGL_EXT_gl_colorspace_display_p3",
                                          "EGL_EXT_pixel_format_float",
                                          "EGL_EXT_surface_SMPTE2086_metadata",
                                          "EGL_EXT_surface_CTA861_3_metadata"};
    for (const char *name : kWanted)
        report.add("egl", std::string("has_") + name, has_word(extensions, name) ? "yes" : "no");

    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    EGLint colour_space = 0;
    if (surface != EGL_NO_SURFACE &&
        eglQuerySurface(display, surface, EGL_GL_COLORSPACE_KHR, &colour_space))
        report.add_hex("egl", "current_surface_colour_space",
                       static_cast<std::uint32_t>(colour_space));
    else
        report.add("egl", "current_surface_colour_space", "(not reported)");

    EGLint count = 0;
    if (!eglGetConfigs(display, nullptr, 0, &count) || count <= 0)
    {
        report.note("egl", "no configs listed");
        return;
    }
    report.add("egl", "config_count", count);
    EGLConfig configs[64];
    const EGLint wanted = count < 64 ? count : 64;
    EGLint got = 0;
    if (!eglGetConfigs(display, configs, wanted, &got))
        return;
    for (EGLint i = 0; i < got; ++i)
    {
        EGLint r = 0;
        EGLint g = 0;
        EGLint b = 0;
        EGLint a = 0;
        EGLint depth = 0;
        EGLint stencil = 0;
        EGLint surface_type = 0;
        EGLint renderable = 0;
        eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(display, configs[i], EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(display, configs[i], EGL_STENCIL_SIZE, &stencil);
        eglGetConfigAttrib(display, configs[i], EGL_SURFACE_TYPE, &surface_type);
        eglGetConfigAttrib(display, configs[i], EGL_RENDERABLE_TYPE, &renderable);
        char text[120];
        std::snprintf(text, sizeof(text),
                      "rgba=%d/%d/%d/%d depth=%d stencil=%d window=%d opengl=%d", r, g, b, a,
                      depth, stencil, (surface_type & EGL_WINDOW_BIT) != 0 ? 1 : 0,
                      (renderable & EGL_OPENGL_BIT) != 0 ? 1 : 0);
        report.add("egl", "config_" + std::to_string(i), text);
    }
}

// ---- the hardware decoder's library ----------------------------------------------------------

// Only loading and looking up: nothing of the decoder is called. The calls take structs whose
// layout is known only from other people's headers, and a wrong layout could corrupt memory, so
// that waits until this probe shows the functions can be reached at all.
void probe_decoder_load(Report &report)
{
    static const char *const kFunctions[] = {
        "sceVideodec2QueryComputeMemoryInfo", "sceVideodec2AllocateComputeQueue",
        "sceVideodec2ReleaseComputeQueue",    "sceVideodec2QueryDecoderMemoryInfo",
        "sceVideodec2CreateDecoder",          "sceVideodec2DeleteDecoder",
        "sceVideodec2Reset",                  "sceVideodec2Decode",
        "sceVideodec2Flush"};

    // 207 is SCE_SYSMODULE_VIDEODEC2 in EVO Player's header; its code ignores the result.
    const int sysmodule = sceSysmoduleLoadModule(207);
    report.add_hex("decoder-load", "sysmodule_207_rc", static_cast<std::uint32_t>(sysmodule));

    static const char *const kPaths[] = {"/system/common/lib/libSceVideodec2.sprx",
                                         "/system/priv/lib/libSceVideodec2.sprx",
                                         "libSceVideodec2.sprx"};
    for (const char *path : kPaths)
    {
        int result = 0;
        const int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &result);
        const std::string key = std::string("module_") + path;
        report.add("decoder-load", key + "_handle_or_rc", static_cast<std::int64_t>(handle));
        if (handle <= 0)
            continue;
        int found = 0;
        for (const char *name : kFunctions)
        {
            void *address = nullptr;
            const int rc = sceKernelDlsym(handle, name, &address);
            if (rc == 0 && address != nullptr)
                ++found;
            else
                report.add_hex("decoder-load", key + "_" + name, static_cast<std::uint32_t>(rc));
        }
        report.add("decoder-load", key + "_functions_found",
                   std::string(std::to_string(found) + " of " +
                               std::to_string(sizeof(kFunctions) / sizeof(kFunctions[0]))));
    }
}

// ---- an HDR surface --------------------------------------------------------------------------

struct HdrAttempt
{
    const char *name;
    SurfaceRequest request;
    EGLint colour_space;
    bool scrgb; // bar values are linear light in units of 80 nits, not an encoded signal
};

// What the bars show, left to right: black, then steps up to 4000 nits.
constexpr float kNits[8] = {0.0f, 10.0f, 100.0f, 203.0f, 400.0f, 1000.0f, 2000.0f, 4000.0f};
constexpr float kHlgSignal[8] = {0.0f, 0.2f, 0.4f, 0.5f, 0.6f, 0.75f, 0.9f, 1.0f};

void draw_bars(const HdrAttempt &attempt, int width, int height, bool hlg)
{
    glEnable(GL_SCISSOR_TEST);
    const int bar = width / 8;
    for (int i = 0; i < 8; ++i)
    {
        float value = 0.0f;
        if (hlg)
            value = kHlgSignal[i];
        else if (attempt.scrgb)
            value = kNits[i] / 80.0f;
        else
            value = video::linear_to_pq(kNits[i] / 10000.0f);
        glScissor(i * bar, height / 6, bar, height - height / 6);
        glClearColor(value, value, value, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // A strip below the bars: paper white (203 nits) over its whole width, to compare with.
    const float paper = attempt.scrgb ? 203.0f / 80.0f : video::linear_to_pq(203.0f / 10000.0f);
    glScissor(0, 0, width, height / 6);
    glClearColor(hlg ? 0.75f : paper, hlg ? 0.75f : paper, hlg ? 0.75f : paper, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

void probe_hdr(probe::HdrMode mode, Display &display, Report &report)
{
    HdrAttempt attempt{};
    bool hlg = false;
    switch (mode)
    {
    case probe::HdrMode::pq10:
        attempt = {"pq10", SurfaceRequest{10, 10, 10, 2, false, EGL_GL_COLORSPACE_BT2020_PQ_EXT},
                   EGL_GL_COLORSPACE_BT2020_PQ_EXT, false};
        break;
    case probe::HdrMode::hlg10:
        attempt = {"hlg10", SurfaceRequest{10, 10, 10, 2, false, EGL_GL_COLORSPACE_BT2020_HLG_EXT},
                   EGL_GL_COLORSPACE_BT2020_HLG_EXT, false};
        hlg = true;
        break;
    case probe::HdrMode::fp16:
        attempt = {"fp16", SurfaceRequest{16, 16, 16, 16, true, EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT},
                   EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT, true};
        break;
    case probe::HdrMode::none:
        return;
    }
    report.add("hdr", "attempt", attempt.name);

    // The display goes down and comes up with the surface that was asked for.
    display.close();
    const bool opened = display.open(1920, 1080, attempt.request);
    report.add("hdr", "open", opened ? "yes" : "no");
    report.add("hdr", "egl_error", std::string(egl_error_name(display.last_error())));
    if (!opened)
    {
        report.note("hdr", "the driver refused this surface; the app falls back to 8-bit output");
        return;
    }
    report.add("hdr", "config", display.config_info());
    report.add_hex("hdr", "surface_colour_space",
                   static_cast<std::uint32_t>(display.surface_colour_space()));
    report.add("hdr", "colour_space_accepted",
               display.surface_colour_space() == attempt.colour_space ? "yes" : "no");

    // Hold the bars so that a person can look at the TV: does it switch to HDR, and are the steps
    // above paper white brighter, one after the other? The probe cannot see the TV.
    constexpr int kHoldFrames = 60 * 12;
    for (int frame = 0; frame < kHoldFrames; ++frame)
    {
        draw_bars(attempt, display.width(), display.height(), hlg);
        if (!display.swap())
        {
            report.note("hdr", "swap failed while the bars were shown");
            break;
        }
    }
    report.note("hdr",
                "bars shown for 12 s; look at the TV: HDR badge, and brighter steps to the right");
    display.close();
}

} // namespace

void run_probes(const probe::ProbePlan &plan, Display &display, probe::Report &report)
{
    LogFollower log(report);
    report.add("run", "plan", probe::describe(plan));
    report.add("run", "display",
               std::to_string(display.width()) + "x" + std::to_string(display.height()));
    log.flush();
    if (plan.memory)
    {
        probe_memory(report);
        log.flush();
    }
    if (plan.upload)
    {
        probe_upload(report);
        log.flush();
    }
    if (plan.egl)
    {
        probe_egl(report);
        log.flush();
    }
    if (plan.decoder_load)
    {
        probe_decoder_load(report);
        log.flush();
    }
    if (plan.hdr != probe::HdrMode::none)
    {
        probe_hdr(plan.hdr, display, report);
        log.flush();
    }
}

} // namespace hui::ps5
