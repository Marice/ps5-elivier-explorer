// EliVier Explorer - The console's hardware video decoders, made once at start-up.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/hw_video.hpp"

#include "explorer/trace.hpp"
#include "platform/ps5/system.hpp"
#include "video/hardware_decoder.hpp"

#ifdef HUI_VIDEODEC2

#include "platform/ps5/videodec2.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C"
{
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int type, int64_t *physical_start);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_start, size_t alignment);
int sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection, int flags,
                                    const char *name);
int sceSysmoduleLoadModule(uint16_t id);
}

namespace hui::ps5
{

namespace
{

namespace vd = videodec2;

constexpr std::size_t kMaxSlots = 6;
constexpr std::uint64_t kAccessUnitBytes = 8u * 1024u * 1024u; // the largest access unit taken
// A decode call this slow means the decoder is stuck or failing: the film goes on in software
// next time (a call cannot be cancelled, EVO Player found the same).
constexpr std::int64_t kStuckMicroseconds = 1000 * 1000;
// Units given without a picture coming out before the decoder is taken to need a flush after
// every unit (as EVO Player does it), which the single-picture probe hinted at (D60).
constexpr int kUnitsBeforeFlushing = 8;

std::uint64_t round_up(std::uint64_t value, std::uint64_t to)
{
    return (value + to - 1) / to * to;
}

// Direct memory, never given back (D58: what is given back does not always come back).
void *direct(std::uint64_t bytes, std::uint64_t alignment)
{
    alignment = std::max<std::uint64_t>(alignment, vd::kMemoryAlignment);
    bytes = round_up(std::max<std::uint64_t>(bytes, 1), alignment);
    int64_t start = -1;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, alignment,
                                      vd::kMemoryType, &start) != 0)
        return nullptr;
    void *address = nullptr;
    if (sceKernelMapDirectMemory(&address, bytes, vd::kMemoryProtection, 0, start, alignment) != 0)
        return nullptr;
    return address;
}

struct Slot
{
    std::uint8_t *buffer = nullptr;
    std::uint64_t bytes = 0;
    std::atomic<bool> busy{false}; // a picture in it is still held by the film
};

struct Resident
{
    const char *name = "";
    video::HardwareCodec codec = video::HardwareCodec::avc;
    int max_width = 0;
    int max_height = 0;
    int max_depth = 8;
    void *decoder = nullptr;
    void *access_unit = nullptr;
    std::array<Slot, kMaxSlots> slots{};
    std::size_t slot_count = 0;
    std::atomic<bool> lent{false};
};

Resident g_avc;
Resident g_hevc;
void *g_queue = nullptr;

vd::DecoderConfigInfo config_for(std::uint32_t codec, std::uint32_t profile, std::uint32_t level,
                                 int width, int height)
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
    c.compute_queue = reinterpret_cast<std::uint64_t>(g_queue);
    c.cpu_affinity = 0x3F;
    c.cpu_priority = 700;
    c.optimize_progressive = 1;
    return c;
}

bool make_resident(Resident &resident, const vd::DecoderConfigInfo &config, std::size_t slots)
{
    explorer::trace("hw video: %s: memory query", resident.name);
    vd::DecoderMemoryInfo memory{};
    memory.size = sizeof(memory);
    int32_t rc = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
    if (rc != 0)
    {
        explorer::trace("hw video: %s: memory query refused 0x%08x", resident.name,
                        static_cast<unsigned>(rc));
        return false;
    }
    explorer::trace("hw video: %s: %llu MiB flexible, %llu MiB direct, %u x %llu KiB pictures",
                    resident.name, static_cast<unsigned long long>(memory.cpu_size >> 20),
                    static_cast<unsigned long long>((memory.gpu_size + memory.cpu_gpu_size) >> 20),
                    static_cast<unsigned>(slots),
                    static_cast<unsigned long long>(memory.max_frame_size >> 10));
    if (memory.cpu_size > 0 &&
        (sceKernelMapNamedFlexibleMemory(&memory.cpu, round_up(memory.cpu_size, vd::kMemoryAlignment),
                                         0x03, 0, "hui-vdec") != 0 ||
         memory.cpu == nullptr))
    {
        explorer::trace("hw video: %s: no flexible memory", resident.name);
        return false;
    }
    if ((memory.gpu_size > 0 && (memory.gpu = direct(memory.gpu_size, 0)) == nullptr) ||
        (memory.cpu_gpu_size > 0 && (memory.cpu_gpu = direct(memory.cpu_gpu_size, 0)) == nullptr))
    {
        explorer::trace("hw video: %s: no direct memory", resident.name);
        return false;
    }
    resident.access_unit = direct(kAccessUnitBytes, 0);
    if (resident.access_unit == nullptr)
        return false;
    for (std::size_t i = 0; i < slots; ++i)
    {
        Slot &slot = resident.slots[i];
        slot.bytes = round_up(memory.max_frame_size, vd::kMemoryAlignment);
        slot.buffer = static_cast<std::uint8_t *>(direct(slot.bytes, memory.frame_alignment));
        if (slot.buffer == nullptr)
            break;
        // Grey, as VLC clears its pool: a picture the decoder did not write shows as grey.
        std::memset(slot.buffer, 0x80, slot.bytes);
        resident.slot_count = i + 1;
    }
    if (resident.slot_count < 2)
        return false;
    explorer::trace("hw video: %s: create", resident.name);
    rc = sceVideodec2CreateDecoder(&config, &memory, &resident.decoder);
    if (rc != 0)
    {
        explorer::trace("hw video: %s: create refused 0x%08x", resident.name,
                        static_cast<unsigned>(rc));
        resident.decoder = nullptr;
        return false;
    }
    sceVideodec2Reset(resident.decoder);
    explorer::trace("hw video: %s: ready", resident.name);
    return true;
}

// A resident decoder lent to one film.
class Backend final : public video::HardwareBackend
{
  public:
    explicit Backend(Resident &resident)
        : resident_(resident), name_(std::string("hardware ") + resident.name)
    {
        sceVideodec2Reset(resident_.decoder);
    }

    ~Backend() override
    {
        sceVideodec2Reset(resident_.decoder);
        resident_.lent.store(false);
    }

    const std::string &name() const override
    {
        return name_;
    }

    bool decode(const std::uint8_t *data, std::size_t size,
                std::vector<video::HardwarePicture> &out) override
    {
        if (broken_ || size == 0 || size > kAccessUnitBytes)
            return false;
        Slot *slot = free_slot();
        if (slot == nullptr)
        {
            note("no free picture buffer");
            return false;
        }
        std::memcpy(resident_.access_unit, data, size);
        vd::InputData input{};
        input.size = sizeof(input);
        input.au = resident_.access_unit;
        input.au_size = size;
        input.pts = units_;
        input.dts = ~std::uint64_t{0};
        ++units_;
        vd::FrameBuffer frame = frame_for(*slot);
        vd::OutputInfo output{};
        output.size = sizeof(output);
        const std::int64_t begin = sys::monotonic_us();
        const int32_t rc = sceVideodec2Decode(resident_.decoder, &input, &frame, &output);
        if (sys::monotonic_us() - begin > kStuckMicroseconds)
        {
            broken_ = true;
            note("a decode call took over a second; giving up on the hardware for this film");
        }
        if (rc != 0)
        {
            if (refusals_++ < 5)
                explorer::trace("hw video: decode refused 0x%08x", static_cast<unsigned>(rc));
            return false;
        }
        if (take(output, out))
            return true;
        // Nothing came out. A decoder that keeps pictures until it is flushed gets a flush after
        // every unit; one that gives them out by itself is left alone (B-frames would come out
        // too early otherwise).
        if (!flushing_ && !pictures_seen_ && ++empty_units_ >= kUnitsBeforeFlushing)
        {
            flushing_ = true;
            explorer::trace("hw video: %s gives pictures only when flushed; flushing every unit",
                            resident_.name);
        }
        if (flushing_)
            flush_once(*slot, out);
        return true;
    }

    bool flush(std::vector<video::HardwarePicture> &out) override
    {
        for (int i = 0; i < 32; ++i)
        {
            Slot *slot = free_slot();
            if (slot == nullptr || !flush_once(*slot, out))
                break;
        }
        return true;
    }

    void reset() override
    {
        sceVideodec2Reset(resident_.decoder);
    }

  private:
    Slot *free_slot()
    {
        for (std::size_t i = 0; i < resident_.slot_count; ++i)
            if (!resident_.slots[i].busy.load())
                return &resident_.slots[i];
        return nullptr;
    }

    static vd::FrameBuffer frame_for(Slot &slot)
    {
        vd::FrameBuffer frame{};
        frame.size = sizeof(frame);
        frame.buffer = slot.buffer;
        frame.buffer_size = slot.bytes;
        return frame;
    }

    bool flush_once(Slot &slot, std::vector<video::HardwarePicture> &out)
    {
        vd::FrameBuffer frame = frame_for(slot);
        vd::OutputInfo output{};
        output.size = sizeof(output);
        if (sceVideodec2Flush(resident_.decoder, &frame, &output) != 0)
            return false;
        return take(output, out);
    }

    // The picture in `output`, if there is one, held in the slot it was written to.
    bool take(const vd::OutputInfo &output, std::vector<video::HardwarePicture> &out)
    {
        if (output.valid == 0 || output.buffer == nullptr)
            return false;
        const auto *start = static_cast<const std::uint8_t *>(output.buffer);
        Slot *holder = nullptr;
        for (std::size_t i = 0; i < resident_.slot_count; ++i)
        {
            Slot &slot = resident_.slots[i];
            if (start >= slot.buffer && start < slot.buffer + slot.bytes)
                holder = &slot;
        }
        if (holder == nullptr || output.error != 0)
        {
            note(holder == nullptr ? "a picture outside every buffer" : "a damaged picture");
            return false;
        }
        if (!pictures_seen_)
            explorer::trace("hw video: first picture %ux%u, pitch %u bytes for %u samples",
                            output.width, output.height, output.pitch_bytes, output.pitch);
        pictures_seen_ = true;
        holder->busy.store(true);
        std::atomic<bool> *busy = &holder->busy;
        video::HardwarePicture picture;
        picture.luma = start;
        picture.pitch_bytes = static_cast<int>(output.pitch_bytes);
        picture.coded_height = static_cast<int>(output.height);
        picture.wide = output.pitch_bytes >= 2 * output.pitch;
        picture.owner =
            std::shared_ptr<const void>(holder, [busy](const void *) { busy->store(false); });
        out.push_back(std::move(picture));
        return true;
    }

    void note(const char *text)
    {
        if (notes_++ < 5)
            explorer::trace("hw video: %s: %s", resident_.name, text);
    }

    Resident &resident_;
    std::string name_;
    std::uint64_t units_ = 0;
    bool broken_ = false;
    bool flushing_ = false;
    bool pictures_seen_ = false;
    int empty_units_ = 0;
    int refusals_ = 0;
    int notes_ = 0;
};

std::unique_ptr<video::HardwareBackend> lend(const video::HardwareStream &stream)
{
    for (Resident *resident : {&g_avc, &g_hevc})
    {
        if (resident->decoder == nullptr || resident->codec != stream.codec ||
            stream.width > resident->max_width || stream.height > resident->max_height ||
            stream.bit_depth > resident->max_depth)
            continue;
        if (resident->lent.exchange(true))
        {
            explorer::trace("hw video: %s is in use; software instead", resident->name);
            return nullptr;
        }
        return std::make_unique<Backend>(*resident);
    }
    return nullptr;
}

} // namespace

void hw_video_bring_up()
{
    explorer::trace("hw video: sysmodule 207");
    sceSysmoduleLoadModule(vd::kSysmodule);
    vd::ComputeMemoryInfo compute{};
    compute.size = sizeof(compute);
    if (sceVideodec2QueryComputeMemoryInfo(&compute) != 0)
    {
        explorer::trace("hw video: no compute memory size; software only");
        return;
    }
    compute.cpu_gpu_size = round_up(compute.cpu_gpu_size, vd::kMemoryAlignment);
    compute.cpu_gpu = direct(compute.cpu_gpu_size, 0);
    vd::ComputeConfigInfo queue{};
    queue.size = sizeof(queue);
    if (compute.cpu_gpu == nullptr ||
        sceVideodec2AllocateComputeQueue(&queue, &compute, &g_queue) != 0)
    {
        explorer::trace("hw video: no compute queue; software only");
        return;
    }
    g_avc.name = "h264";
    g_avc.codec = video::HardwareCodec::avc;
    g_avc.max_width = 1920;
    g_avc.max_height = 1088;
    g_avc.max_depth = 8;
    if (!make_resident(g_avc, config_for(vd::kCodecAvc, 100, 51, 1920, 1088), 6))
        g_avc.decoder = nullptr;
    g_hevc.name = "hevc";
    g_hevc.codec = video::HardwareCodec::hevc;
    g_hevc.max_width = 3840;
    g_hevc.max_height = 2176;
    g_hevc.max_depth = 10;
    if (!make_resident(g_hevc, config_for(vd::kCodecHevc, 2, 153, 3840, 2176), 4))
        g_hevc.decoder = nullptr;
    if (g_avc.decoder != nullptr || g_hevc.decoder != nullptr)
        video::set_hardware_provider(&lend);
    explorer::trace("hw video: h264 %s, hevc %s", g_avc.decoder ? "ready" : "software",
                    g_hevc.decoder ? "ready" : "software");
}

} // namespace hui::ps5

#else

namespace hui::ps5
{

void hw_video_bring_up()
{
}

} // namespace hui::ps5

#endif
