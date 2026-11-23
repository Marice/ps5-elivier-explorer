// EliVier Explorer - Console probes: questions put to the PS5 before video is built on the answers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/probes.hpp"

#include "explorer/trace.hpp"
#include "platform/ps5/surround_out.hpp"
#include "platform/ps5/system.hpp"
#include "platform/ps5/video_out_hook.hpp"
#include "platform/ps5/videodec2.hpp"
#include "video/colour.hpp"
#include "video/convert.hpp"
#include "video/convert_pool.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
int sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection, int flags,
                                    const char *name);
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

// Writes the report lines that were not logged yet to the app log and the trace, so a crash keeps
// them.
class LogFollower
{
  public:
    explicit LogFollower(const Report &report) : report_(report)
    {
    }
    void flush()
    {
        for (; logged_ < report_.lines().size(); ++logged_)
        {
            sys::log("[HUI] %s", report_.lines()[logged_].c_str());
            explorer::trace("%s", report_.lines()[logged_].c_str());
        }
    }

  private:
    const Report &report_;
    std::size_t logged_ = 0;
};

LogFollower *g_follower = nullptr; // set while run_probes() runs

// What is about to be tried, written before the call: to the log and to the trace file, which
// is synced per line and read over FTP after the app has closed. A step that takes the app down
// leaves its line as the last one, and the answers found before it are written first.
__attribute__((format(printf, 1, 2))) void step(const char *format, ...)
{
    if (g_follower != nullptr)
        g_follower->flush();
    char text[200];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    sys::log("[HUI] probe step: %s", text);
    explorer::trace("probe step: %s", text);
}

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
        step("memory type %d: %zu MiB", type, size / kMiB);
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
    step("memory: sizes of direct and flexible memory");
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
    // One finished RGBA picture (no chroma plane): what the CPU conversion of the first video
    // viewer uploads (docs/decision-log.md, D51). 8 bits per channel, and 16 as a comparison.
    {"rgba8_1080p",
     {1920, 1080, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1920u * 1080u * 4u},
     {0, 0, 0, 0, 0, 0}},
    {"rgba16_1080p",
     {1920, 1080, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 1920u * 1080u * 8u},
     {0, 0, 0, 0, 0, 0}},
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
    step("upload %s %s: %d uploads", frame.name, through_buffer ? "through a buffer" : "direct",
         kWarmup + kRepeats);
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
            if (frame.chroma.width > 0)
            {
                const auto chroma_offset = static_cast<std::uintptr_t>(frame.luma.bytes);
                upload_plane(chroma, frame.chroma, reinterpret_cast<const void *>(chroma_offset));
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        else
        {
            upload_plane(luma, frame.luma, source);
            if (frame.chroma.width > 0)
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

// ---- convert ---------------------------------------------------------------------------------

struct ConvertCase
{
    const char *name;
    int width;
    int height;
    int bit_depth;
    bool hdr;
    int threads; // bands the conversion is split over (ConvertPool), the caller's included
};

constexpr ConvertCase kConvertCases[] = {
    {"sdr8_1080p", 1920, 1080, 8, false, 1},
    {"pq10_1080p", 1920, 1080, 10, true, 1},
    {"pq10_1080p_t4", 1920, 1080, 10, true, 4},
    {"sdr10_4k_t4", 3840, 2160, 10, false, 4},
    {"pq10_4k_t4", 3840, 2160, 10, true, 4},
    {"pq10_4k_t6", 3840, 2160, 10, true, 6},
};

// What the video viewer does for every picture (docs/decision-log.md, D51 and D52): YCbCr 4:2:0 to
// RGBA with 16 bits a channel on the CPU, then glTexSubImage2D. Timed apart and together, since a
// 24 fps film has 41.7 ms for the two and everything else. Run on one thread, like the engine.
void probe_convert(Report &report)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (const ConvertCase &item : kConvertCases)
    {
        step("convert %s: %d conversions", item.name, kWarmup + kRepeats);
        const int bytes = item.bit_depth > 8 ? 2 : 1;
        const int chroma_width = item.width / 2;
        std::vector<std::uint8_t> luma(static_cast<std::size_t>(item.width) * item.height * bytes);
        std::vector<std::uint8_t> cb(static_cast<std::size_t>(chroma_width) * item.height / 2 *
                                     bytes);
        std::vector<std::uint8_t> cr(cb.size());
        // A pattern, so that the work is not that of a flat picture; sample values stay valid.
        for (std::size_t i = 0; i < luma.size(); ++i)
            luma[i] = static_cast<std::uint8_t>((i * 7u) & 0xffu);
        if (bytes == 2)
            for (std::size_t i = 1; i < luma.size(); i += 2)
                luma[i] &= 0x03;
        for (std::size_t i = 0; i < cb.size(); ++i)
        {
            cb[i] = static_cast<std::uint8_t>((i * 5u) & 0xffu);
            cr[i] = static_cast<std::uint8_t>((i * 3u) & 0xffu);
        }
        if (bytes == 2)
            for (std::size_t i = 1; i < cb.size(); i += 2)
            {
                cb[i] &= 0x03;
                cr[i] &= 0x03;
            }
        video::Frame frame;
        frame.planes[0] = luma.data();
        frame.planes[1] = cb.data();
        frame.planes[2] = cr.data();
        frame.pitches[0] = item.width * bytes;
        frame.pitches[1] = chroma_width * bytes;
        frame.pitches[2] = chroma_width * bytes;
        frame.plane_count = 3;
        frame.width = item.width;
        frame.height = item.height;
        frame.bit_depth = item.bit_depth;
        frame.colour.matrix = video::Matrix::bt709;
        frame.colour.range = video::Range::limited;
        frame.colour.transfer = item.hdr ? video::Transfer::pq : video::Transfer::sdr;

        std::vector<std::uint16_t> rgba(static_cast<std::size_t>(item.width) * item.height * 4);
        video::ConvertTables tables;
        video::ConvertPool pool(item.threads);
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, item.width, item.height, 0, GL_RGBA,
                     GL_UNSIGNED_SHORT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

        probe::TimingStats convert;
        probe::TimingStats upload;
        probe::TimingStats both;
        bool ok = true;
        for (int i = 0; i < kWarmup + kRepeats && ok; ++i)
        {
            const std::int64_t begin = sys::monotonic_us();
            ok = pool.convert_rgba16(frame, tables, rgba.data(),
                                          static_cast<std::size_t>(item.width) * 8);
            const std::int64_t converted = sys::monotonic_us();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, item.width, item.height, GL_RGBA,
                            GL_UNSIGNED_SHORT, rgba.data());
            glFinish();
            const std::int64_t done = sys::monotonic_us();
            if (i >= kWarmup)
            {
                convert.add(static_cast<double>(converted - begin) / 1000.0);
                upload.add(static_cast<double>(done - converted) / 1000.0);
                both.add(static_cast<double>(done - begin) / 1000.0);
            }
        }
        glDeleteTextures(1, &texture);
        if (!ok)
        {
            report.note("convert", std::string(item.name) + " the conversion refused the frame");
            continue;
        }
        char text[240];
        std::snprintf(text, sizeof(text),
                      "convert avg=%.2f p95=%.2f ms; upload avg=%.2f p95=%.2f ms; both avg=%.2f "
                      "p95=%.2f max=%.2f ms (n=%zu)",
                      convert.average(), convert.percentile(0.95), upload.average(),
                      upload.percentile(0.95), both.average(), both.percentile(0.95),
                      both.maximum(), both.count());
        report.add("convert", item.name, text);
        report.add_hex("convert", std::string(item.name) + "_gl_error", glGetError());
    }
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
        step("upload %s: source frame of %zu KiB", frame.name, all_bytes / 1024);
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
        const GLuint chroma = frame.chroma.width > 0 ? make_texture(frame.chroma) : 0;
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
        if (chroma != 0)
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
    step("egl: extensions and configs");
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
    step("decoder-load: sceSysmoduleLoadModule(207)");
    const int sysmodule = sceSysmoduleLoadModule(207);
    report.add_hex("decoder-load", "sysmodule_207_rc", static_cast<std::uint32_t>(sysmodule));

    static const char *const kPaths[] = {"/system/common/lib/libSceVideodec2.sprx",
                                         "/system/priv/lib/libSceVideodec2.sprx",
                                         "libSceVideodec2.sprx"};
    for (const char *path : kPaths)
    {
        int result = 0;
        step("decoder-load: sceKernelLoadStartModule(%s)", path);
        const int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &result);
        const std::string key = std::string("module_") + path;
        report.add("decoder-load", key + "_handle_or_rc", static_cast<std::int64_t>(handle));
        if (handle <= 0)
            continue;
        int found = 0;
        step("decoder-load: looking up %zu functions in module %d",
             sizeof(kFunctions) / sizeof(kFunctions[0]), handle);
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

// ---- the decoder's library, bound at build time ----------------------------------------------

#ifdef HUI_VIDEODEC2
// The layouts are in platform/ps5/videodec2.hpp.
using videodec2::ComputeMemoryInfo;

// What EVO Player does first (its evo_vdec_native_probe): load sysmodule 207, ignoring the
// result, then ask how much memory a compute queue would want. The first call into the library
// is where the previous run faulted; the loader had accepted the library at start.
void probe_decoder_bind(Report &report, const char *label)
{
    const std::string prefix = std::string(label) + "_";
    step("decoder-bind %s: sceSysmoduleLoadModule(207), as EVO Player does first", label);
    const int sysmodule = sceSysmoduleLoadModule(207);
    report.add_hex("decoder-bind", prefix + "sysmodule_207_rc",
                   static_cast<std::uint32_t>(sysmodule));
    step("decoder-bind %s: sceVideodec2QueryComputeMemoryInfo (read-only, struct of %zu bytes)",
         label, sizeof(ComputeMemoryInfo));
    ComputeMemoryInfo info{};
    info.size = sizeof(info);
    const int32_t rc = sceVideodec2QueryComputeMemoryInfo(&info);
    report.add_hex("decoder-bind", prefix + "query_compute_memory_rc",
                   static_cast<std::uint32_t>(rc));
    report.add("decoder-bind", prefix + "cpu_gpu_size_bytes",
               static_cast<std::int64_t>(info.cpu_gpu_size));
    report.add("decoder-bind", prefix + "struct_size_sent", static_cast<std::int64_t>(info.size));
}
#else
void probe_decoder_bind(Report &report, const char *label)
{
    report.note("decoder-bind", std::string(label) + ": this build has no libSceVideodec2 import; "
                                                      "build with HW_DECODE=1");
}
#endif


// ---- hwdec: a whole hardware decoder, up before the HEN, one picture after --------------------

#ifdef HUI_VIDEODEC2
namespace hwdec
{
#include "platform/ps5/hwdec_fixture.inc"

namespace vd = videodec2;

// What the first half made, for the second half. Nothing is freed: both players found that
// memory given back does not always come back (D58), and the run ends soon after.
struct State
{
    bool created = false;
    void *queue = nullptr;
    void *decoder = nullptr;
    void *au = nullptr;
    std::uint64_t au_size = 0;
    void *frame = nullptr;
    std::uint64_t frame_size = 0;
};
State g_state;

std::uint64_t round_up(std::uint64_t value, std::uint64_t to)
{
    return (value + to - 1) / to * to;
}

// Direct memory as both players take it; null when the console refuses.
void *direct(Report &report, const char *what, std::uint64_t bytes, std::uint64_t alignment)
{
    alignment = std::max<std::uint64_t>(alignment, vd::kMemoryAlignment);
    bytes = round_up(std::max<std::uint64_t>(bytes, 1), alignment);
    int64_t start = -1;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                           vd::kMemoryType, &start);
    if (rc != 0)
    {
        report.add_hex("hwdec", std::string(what) + "_allocate_rc", static_cast<std::uint32_t>(rc));
        return nullptr;
    }
    void *address = nullptr;
    rc = sceKernelMapDirectMemory(&address, bytes, vd::kMemoryProtection, 0, start, alignment);
    if (rc != 0 || address == nullptr)
    {
        report.add_hex("hwdec", std::string(what) + "_map_rc", static_cast<std::uint32_t>(rc));
        sceKernelReleaseDirectMemory(start, bytes);
        return nullptr;
    }
    report.add("hwdec", std::string(what) + "_kib", static_cast<std::int64_t>(bytes / 1024));
    return address;
}

vd::DecoderConfigInfo config(std::uint32_t codec, std::uint32_t profile, std::uint32_t level,
                             int width, int height, void *queue)
{
    vd::DecoderConfigInfo c{};
    c.size = sizeof(c);
    c.resource_type = vd::kResourceCompute;
    c.codec_type = codec;
    c.profile = profile;
    c.max_level = level;
    c.max_width = width;
    c.max_height = height;
    c.max_dpb_frames = 16;
    c.pipeline_depth = 1;
    c.compute_queue = reinterpret_cast<std::uint64_t>(queue);
    c.cpu_affinity = 0x3F;
    c.cpu_priority = 700;
    c.optimize_progressive = 1;
    return c;
}

void report_memory(Report &report, const std::string &prefix, const vd::DecoderMemoryInfo &m)
{
    report.add("hwdec", prefix + "_cpu_kib", static_cast<std::int64_t>(m.cpu_size / 1024));
    report.add("hwdec", prefix + "_gpu_kib", static_cast<std::int64_t>(m.gpu_size / 1024));
    report.add("hwdec", prefix + "_cpu_gpu_kib", static_cast<std::int64_t>(m.cpu_gpu_size / 1024));
    report.add("hwdec", prefix + "_frame_kib", static_cast<std::int64_t>(m.max_frame_size / 1024));
    report.add("hwdec", prefix + "_frame_alignment", static_cast<std::int64_t>(m.frame_alignment));
}

void bring_up(Report &report)
{
    step("hwdec: sceSysmoduleLoadModule(207)");
    report.add_hex("hwdec", "sysmodule_rc",
                   static_cast<std::uint32_t>(sceSysmoduleLoadModule(vd::kSysmodule)));

    step("hwdec: sceVideodec2QueryComputeMemoryInfo");
    vd::ComputeMemoryInfo compute{};
    compute.size = sizeof(compute);
    int32_t rc = sceVideodec2QueryComputeMemoryInfo(&compute);
    report.add_hex("hwdec", "query_compute_rc", static_cast<std::uint32_t>(rc));
    if (rc != 0)
        return;
    compute.cpu_gpu_size = round_up(compute.cpu_gpu_size, vd::kMemoryAlignment);
    step("hwdec: compute memory, %llu KiB",
         static_cast<unsigned long long>(compute.cpu_gpu_size / 1024));
    compute.cpu_gpu = direct(report, "compute", compute.cpu_gpu_size, vd::kMemoryAlignment);
    if (compute.cpu_gpu == nullptr)
        return;

    step("hwdec: sceVideodec2AllocateComputeQueue");
    vd::ComputeConfigInfo queue_config{};
    queue_config.size = sizeof(queue_config);
    rc = sceVideodec2AllocateComputeQueue(&queue_config, &compute, &g_state.queue);
    report.add_hex("hwdec", "allocate_queue_rc", static_cast<std::uint32_t>(rc));
    if (rc != 0)
        return;

    // What 4K HEVC Main10 would want (asked only, nothing made): for the resident decoder later.
    step("hwdec: sceVideodec2QueryDecoderMemoryInfo, HEVC Main10 3840x2176");
    const vd::DecoderConfigInfo hevc =
        config(vd::kCodecHevc, 2, 153, 3840, 2176, g_state.queue);
    vd::DecoderMemoryInfo hevc_memory{};
    hevc_memory.size = sizeof(hevc_memory);
    rc = sceVideodec2QueryDecoderMemoryInfo(&hevc, &hevc_memory);
    report.add_hex("hwdec", "query_hevc4k_rc", static_cast<std::uint32_t>(rc));
    if (rc == 0)
        report_memory(report, "hevc4k", hevc_memory);

    step("hwdec: sceVideodec2QueryDecoderMemoryInfo, AVC High 1920x1088");
    const vd::DecoderConfigInfo avc = config(vd::kCodecAvc, 100, 51, 1920, 1088, g_state.queue);
    vd::DecoderMemoryInfo memory{};
    memory.size = sizeof(memory);
    rc = sceVideodec2QueryDecoderMemoryInfo(&avc, &memory);
    report.add_hex("hwdec", "query_avc_rc", static_cast<std::uint32_t>(rc));
    if (rc != 0)
        return;
    report_memory(report, "avc", memory);

    step("hwdec: decoder memory (CPU part from flexible memory)");
    if (memory.cpu_size > 0)
    {
        const std::uint64_t bytes = round_up(memory.cpu_size, vd::kMemoryAlignment);
        rc = sceKernelMapNamedFlexibleMemory(&memory.cpu, bytes, 0x03, 0, "hui-vdec-cpu");
        report.add_hex("hwdec", "cpu_map_rc", static_cast<std::uint32_t>(rc));
        if (rc != 0 || memory.cpu == nullptr)
            return;
    }
    if (memory.gpu_size > 0 &&
        (memory.gpu = direct(report, "gpu", memory.gpu_size, vd::kMemoryAlignment)) == nullptr)
        return;
    if (memory.cpu_gpu_size > 0 &&
        (memory.cpu_gpu = direct(report, "cpu_gpu", memory.cpu_gpu_size,
                                 vd::kMemoryAlignment)) == nullptr)
        return;
    g_state.frame_size = round_up(memory.max_frame_size, vd::kMemoryAlignment);
    g_state.frame = direct(report, "frame", g_state.frame_size, memory.frame_alignment);
    g_state.au_size = sizeof(kFixtureH264);
    g_state.au = direct(report, "au", g_state.au_size, vd::kMemoryAlignment);
    if (g_state.frame == nullptr || g_state.au == nullptr)
        return;
    // Grey, as VLC clears its pool: a picture that is not written shows as grey, not as noise.
    std::memset(g_state.frame, 0x80, g_state.frame_size);
    std::memcpy(g_state.au, kFixtureH264, sizeof(kFixtureH264));

    step("hwdec: sceVideodec2CreateDecoder (AVC)");
    rc = sceVideodec2CreateDecoder(&avc, &memory, &g_state.decoder);
    report.add_hex("hwdec", "create_rc", static_cast<std::uint32_t>(rc));
    if (rc != 0)
        return;
    step("hwdec: sceVideodec2Reset");
    rc = sceVideodec2Reset(g_state.decoder);
    report.add_hex("hwdec", "reset_rc", static_cast<std::uint32_t>(rc));
    g_state.created = rc == 0;
    step("hwdec: decoder %s", g_state.created ? "ready" : "not ready");
}

void report_output(Report &report, const std::string &prefix, const vd::OutputInfo &out,
                   const vd::FrameBuffer &frame)
{
    report.add("hwdec", prefix + "_valid", static_cast<std::int64_t>(out.valid));
    report.add("hwdec", prefix + "_error", static_cast<std::int64_t>(out.error));
    report.add("hwdec", prefix + "_accepted", static_cast<std::int64_t>(frame.accepted));
    report.add("hwdec", prefix + "_size",
               std::to_string(out.width) + "x" + std::to_string(out.height) + " pitch " +
                   std::to_string(out.pitch) + " (" + std::to_string(out.pitch_bytes) +
                   " bytes) format " + std::to_string(out.frame_format));
}

void decode(Report &report)
{
    if (!g_state.created)
    {
        report.note("hwdec", "no decoder was made before the HEN; nothing to decode");
        return;
    }
    vd::InputData input{};
    input.size = sizeof(input);
    input.au = g_state.au;
    input.au_size = g_state.au_size;
    input.pts = 0;
    input.dts = ~std::uint64_t{0};
    vd::FrameBuffer frame{};
    frame.size = sizeof(frame);
    frame.buffer = g_state.frame;
    frame.buffer_size = g_state.frame_size;
    vd::OutputInfo out{};
    out.size = sizeof(out);

    step("hwdec: sceVideodec2Decode, one keyframe of %llu bytes, after the HEN",
         static_cast<unsigned long long>(g_state.au_size));
    const std::int64_t begin = sys::monotonic_us();
    int32_t rc = sceVideodec2Decode(g_state.decoder, &input, &frame, &out);
    report.add("hwdec", "decode_ms", static_cast<double>(sys::monotonic_us() - begin) / 1000.0);
    report.add_hex("hwdec", "decode_rc", static_cast<std::uint32_t>(rc));
    report_output(report, "decode", out, frame);
    if (rc == 0 && out.valid == 0)
    {
        step("hwdec: sceVideodec2Flush (the picture is still inside)");
        out = vd::OutputInfo{};
        out.size = sizeof(out);
        rc = sceVideodec2Flush(g_state.decoder, &frame, &out);
        report.add_hex("hwdec", "flush_rc", static_cast<std::uint32_t>(rc));
        report_output(report, "flush", out, frame);
    }
    if (rc != 0 || out.valid == 0 || out.error != 0 || out.buffer == nullptr ||
        out.pitch_bytes < static_cast<std::uint32_t>(kFixtureWidth) ||
        out.height < static_cast<std::uint32_t>(kFixtureHeight))
    {
        report.note("hwdec", "no picture came out");
        return;
    }
    step("hwdec: hashing the luma");
    const auto *luma = static_cast<const std::uint8_t *>(out.buffer);
    std::uint32_t hash = 0x811C9DC5u;
    for (int y = 0; y < kFixtureHeight; ++y)
        for (int x = 0; x < kFixtureWidth; ++x)
            hash = (hash ^ luma[static_cast<std::size_t>(y) * out.pitch_bytes + x]) * 0x01000193u;
    report.add_hex("hwdec", "luma_hash", hash);
    report.add_hex("hwdec", "luma_hash_expected", kFixtureLumaHash);
    report.add("hwdec", "picture_matches_ffmpeg", hash == kFixtureLumaHash ? "yes" : "no");
    step("hwdec: picture %s FFmpeg's", hash == kFixtureLumaHash ? "matches" : "differs from");
}

} // namespace hwdec
#endif

// ---- speakers: which speaker each of the eight channels reaches --------------------------------

// The order the decoder writes (video/audio_decoder.hpp, D65), to be confirmed by ear: a tone
// of 1.5 s on one channel at a time, then 0.5 s of silence, and the trace names the channel
// while it sounds. The LFE gets a low tone, as a subwoofer plays nothing high.
void probe_speakers(Report &report)
{
    static const char *const kNames[audio::kSurroundChannels] = {
        "front left", "front right", "centre", "LFE (subwoofer)",
        "side / surround left", "side / surround right", "back left", "back right"};
    SurroundOut out;
    step("speakers: opening the eight-channel port");
    if (!out.open())
    {
        report.note("speakers", "the eight-channel port was refused");
        return;
    }
    constexpr int kSegment = 2 * 48000;
    constexpr int kTone = 3 * 48000 / 2;
    std::atomic<int> frame{0};
    std::atomic<int> announced{-1};
    out.start([&](float *grain, int frames) {
        for (int i = 0; i < frames; ++i)
        {
            const int at = frame.fetch_add(1);
            const int channel = at / kSegment;
            const int in_segment = at % kSegment;
            if (channel < audio::kSurroundChannels && in_segment == 0)
                announced.store(channel);
            const bool sounding = channel < audio::kSurroundChannels && in_segment < kTone;
            const float pitch = channel == 3 ? 60.0f : 440.0f;
            const float value =
                sounding ? 0.3f * std::sin(6.2831853f * pitch * static_cast<float>(in_segment) / 48000.0f)
                         : 0.0f;
            for (int c = 0; c < audio::kSurroundChannels; ++c)
                grain[i * audio::kSurroundChannels + c] = c == channel ? value : 0.0f;
        }
        return frames;
    });
    int told = -1;
    const std::int64_t end = sys::monotonic_us() + std::int64_t{kSegment} * audio::kSurroundChannels / 48 * 1000 + 500000;
    while (sys::monotonic_us() < end)
    {
        const int now = announced.load();
        if (now != told && now >= 0)
        {
            told = now;
            step("speakers: channel %d now: %s", now, kNames[now]);
            report.add("speakers", "channel_" + std::to_string(now), kNames[now]);
        }
        sys::sleep_us(20000);
    }
    out.close();
    step("speakers: done");
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

// ---- HDR through the video output: the runtime's buffers retyped to HDR10 --------------------

// The 10-bit PQ code of a light level (SMPTE ST 2084).
std::uint32_t pq_code(float nits)
{
    const float y = std::pow(std::max(nits, 0.0f) / 10000.0f, 0.1593017578125f);
    const float v = std::pow((0.8359375f + 18.8515625f * y) / (1.0f + 18.6875f * y), 78.84375f);
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 1023.0f));
}

// One way the four bytes the GL writes could become the 10:10:10:2 word the display reads: which
// colour sits in the low ten bits, and in which order the GL's channels land in memory.
struct Packing
{
    const char *name;
    bool red_low;  // R in bits 0-9 (B in 20-29), or the other way round
    bool gl_bgra;  // the GL's red lands in byte 2 (a BGRA surface), or in byte 0
};

constexpr Packing kPackings[] = {
    {"1: red low, RGBA bytes", true, false},
    {"2: red low, BGRA bytes", true, true},
    {"3: blue low, RGBA bytes", false, false},
    {"4: blue low, BGRA bytes", false, true},
};

// Fills a rectangle with the 10:10:10:2 word for PQ codes r, g, b, through glClear so that the
// bytes arrive exactly (no blending, no shader).
void fill_packed(const Packing &packing, int x, int y, int w, int h, std::uint32_t r,
                 std::uint32_t g, std::uint32_t b)
{
    const std::uint32_t low = packing.red_low ? r : b;
    const std::uint32_t high = packing.red_low ? b : r;
    const std::uint32_t word = (3u << 30) | (high << 20) | (g << 10) | low;
    const float byte[4] = {static_cast<float>(word & 0xFF), static_cast<float>((word >> 8) & 0xFF),
                           static_cast<float>((word >> 16) & 0xFF),
                           static_cast<float>(word >> 24)};
    const float red = packing.gl_bgra ? byte[2] : byte[0];
    const float blue = packing.gl_bgra ? byte[0] : byte[2];
    glScissor(x, y, w, h);
    glClearColor(red / 255.0f, byte[1] / 255.0f, blue / 255.0f, byte[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

// What a person looks at, per packing (10 s each): white squares top left count the packing;
// eight grey bars from 0 to 4000 nits; red, green, blue and white patches at 203 nits.
void draw_packed_bars(const Packing &packing, int index, int width, int height)
{
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_SCISSOR_TEST);
    fill_packed(packing, 0, 0, width, height, 0, 0, 0);
    const std::uint32_t white = pq_code(203.0f);
    const int square = height / 12;
    for (int i = 0; i <= index; ++i)
        fill_packed(packing, square / 2 + i * square * 3 / 2, height - square * 3 / 2, square,
                    square, white, white, white);
    const int bar = width / 8;
    for (int i = 0; i < 8; ++i)
    {
        const std::uint32_t code = pq_code(kNits[i]);
        fill_packed(packing, i * bar, height / 2, bar, height / 3, code, code, code);
    }
    const int patch = width / 4;
    fill_packed(packing, 0, height / 12, patch, height / 4, white, 0, 0);
    fill_packed(packing, patch, height / 12, patch, height / 4, 0, white, 0);
    fill_packed(packing, 2 * patch, height / 12, patch, height / 4, 0, 0, white);
    fill_packed(packing, 3 * patch, height / 12, patch, height / 4, white, white, white);
    glDisable(GL_SCISSOR_TEST);
}

void probe_hdr_videoout(Display &display, Report &report)
{
    // The runtime opens the output at the first swap: show a few plain frames first. The system's
    // splash picture lies over everything until the app hides it (the first try showed the bars
    // behind it, D70): hidden after the first frame, as the app does.
    for (int i = 0; i < 10; ++i)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        display.swap();
        if (i == 1)
            step("hdr videoout: splash hidden %s", sys::hide_splash_screen() ? "yes" : "no");
    }
    const VideoOutState state = video_out_state();
    report.add("hdr", "videoout_handle", static_cast<std::int64_t>(state.handle));
    report.add_hex("hdr", "runtime_format_high",
                   static_cast<std::uint32_t>(state.pixel_format >> 32));
    report.add("hdr", "runtime_tiling", static_cast<std::int64_t>(state.tiling));
    trace_video_out_status("before HDR");
    step("hdr videoout: retyping the buffers to HDR10");
    if (!video_out_set_format(kVideoOutFormatHdr10))
    {
        report.note("hdr", "the buffers could not be retyped; HDR stays tone-mapped");
        return;
    }
    for (int index = 0; index < 4; ++index)
    {
        const Packing &packing = kPackings[index];
        step("hdr videoout: packing %s", packing.name);
        for (int frame = 0; frame < 60 * 10; ++frame)
        {
            draw_packed_bars(packing, index, display.width(), display.height());
            if (!display.swap())
            {
                report.note("hdr", "swap failed while the bars were shown");
                break;
            }
            if (index == 0 && frame == 60)
                trace_video_out_status("in HDR");
        }
    }
    step("hdr videoout: back to the runtime's format");
    video_out_set_format(state.pixel_format != 0 ? state.pixel_format : kVideoOutFormatSdr);
    for (int i = 0; i < 30; ++i)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        display.swap();
    }
    trace_video_out_status("after HDR");
    report.note("hdr", "four packings shown for 10 s each; which one had red, green and blue "
                       "patches right and grey bars growing brighter, with the TV in HDR?");
}

void probe_hdr(probe::HdrMode mode, Display &display, Report &report)
{
    if (mode == probe::HdrMode::videoout)
    {
        report.add("hdr", "attempt", "videoout");
        probe_hdr_videoout(display, report);
        return;
    }
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
    case probe::HdrMode::videoout:
        return;
    }
    report.add("hdr", "attempt", attempt.name);

    // The display goes down and comes up with the surface that was asked for.
    step("hdr %s: closing the display", attempt.name);
    display.close();
    step("hdr %s: opening the surface", attempt.name);
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
    step("hdr %s: showing the bars", attempt.name);
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

void run_decoder_bind_probe(probe::Report &report, const char *label)
{
    LogFollower log(report);
    g_follower = &log;
    probe_decoder_bind(report, label);
    log.flush();
    g_follower = nullptr;
}

void run_hwdec_bringup(probe::Report &report)
{
    LogFollower log(report);
    g_follower = &log;
#ifdef HUI_VIDEODEC2
    hwdec::bring_up(report);
#else
    report.note("hwdec", "this build has no libSceVideodec2 import; build with HW_DECODE=1");
#endif
    log.flush();
    g_follower = nullptr;
}

void run_probes(const probe::ProbePlan &plan, Display &display, probe::Report &report)
{
    LogFollower log(report);
    g_follower = &log;
    step("run: plan %s", probe::describe(plan).c_str());
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
    if (plan.convert)
    {
        probe_convert(report);
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
    if (plan.speakers)
    {
        probe_speakers(report);
        log.flush();
    }
    if (plan.hwdec)
    {
#ifdef HUI_VIDEODEC2
        hwdec::decode(report);
#endif
        log.flush();
    }
    if (plan.decoder_bind)
    {
        probe_decoder_bind(report, "late");
        log.flush();
    }
    if (plan.hdr != probe::HdrMode::none)
    {
        probe_hdr(plan.hdr, display, report);
        log.flush();
    }
    g_follower = nullptr;
}

} // namespace hui::ps5
