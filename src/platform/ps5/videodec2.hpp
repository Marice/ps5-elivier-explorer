// EliVier Explorer - The console's hardware video decoder (libSceVideodec2), as far as it is known.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// There is no public documentation. The layouts and the call order below come from two
// independent homebrew players that decode with it: EVO Player (GPL-3.0-or-later, its
// sce_videodec2.h and docs/hardware/videodec2-abi.md, after ProsperoLight) and VLC for PS5
// (ps5/modules/ps5vdec.c, also used by MPC PS5). They agree on every size below; where they differ
// (the config's tail) the struct is zeroed, which suits both. See docs/decision-log.md, D58.
//
// Every struct starts with `size`, which must equal sizeof(the struct): the service refuses
// anything else. Only linked into a build made with HW_DECODE=1 (an import stub, D47).

#pragma once

#include <cstdint>

namespace hui::ps5::videodec2
{

constexpr std::uint16_t kSysmodule = 207;

// The codec of a decoder.
constexpr std::uint32_t kCodecAvc = 1;
constexpr std::uint32_t kCodecHevc = 974921;
constexpr std::uint32_t kCodecVp9 = 2382845;

// A decoder runs on a compute queue (resource_type 1).
constexpr std::uint32_t kResourceCompute = 1;

// Direct memory as both players allocate it: type 12, CPU and GPU read and write, 16 KiB aligned.
constexpr int kMemoryType = 12;
constexpr int kMemoryProtection = 0x33;
constexpr std::uint64_t kMemoryAlignment = 0x4000;

struct ComputeConfigInfo
{
    std::uint64_t size;
    std::uint16_t pipe_id;
    std::uint16_t queue_id;
    std::uint8_t check_memory_type;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};
static_assert(sizeof(ComputeConfigInfo) == 0x10);

struct ComputeMemoryInfo
{
    std::uint64_t size;
    std::uint64_t cpu_gpu_size; // written by the query; the caller allocates it and sets cpu_gpu
    void *cpu_gpu;
};
static_assert(sizeof(ComputeMemoryInfo) == 0x18);

struct DecoderConfigInfo
{
    std::uint64_t size;
    std::uint32_t resource_type;
    std::uint32_t codec_type;
    std::uint32_t profile;   // AVC: 100 (High); HEVC: 1 Main, 2 Main10
    std::uint32_t max_level; // AVC: level x 10 (51); HEVC: level x 30 (153 is 5.1)
    std::int32_t max_width;  // 1920 x 1088 is the smallest the query accepts
    std::int32_t max_height;
    std::int32_t max_dpb_frames;
    std::uint32_t pipeline_depth;
    std::uint64_t compute_queue;
    std::uint64_t cpu_affinity; // 0x3F in both players
    std::int32_t cpu_priority;  // 700 in both players
    std::uint32_t optimize_progressive;
    std::uint32_t check_memory_type;
    std::uint32_t reserved;
};
static_assert(sizeof(DecoderConfigInfo) == 0x48);

struct DecoderMemoryInfo
{
    std::uint64_t size;
    std::uint64_t cpu_size; // flexible memory, CPU only
    void *cpu;
    std::uint64_t gpu_size; // direct memory
    void *gpu;
    std::uint64_t cpu_gpu_size; // direct memory
    void *cpu_gpu;
    std::uint64_t max_frame_size; // one output picture
    std::uint32_t frame_alignment;
    std::uint32_t reserved;
};
static_assert(sizeof(DecoderMemoryInfo) == 0x48);

// One access unit, Annex B, with the parameter sets in-band.
struct InputData
{
    std::uint64_t size;
    void *au;
    std::uint64_t au_size;
    std::uint64_t pts;
    std::uint64_t dts; // UINT64_MAX in both players
    std::uint64_t attached;
};
static_assert(sizeof(InputData) == 0x30);

// Where the decoder may write the next picture.
struct FrameBuffer
{
    std::uint64_t size;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t accepted; // set when the picture went into this buffer
    std::uint32_t reserved;
};
static_assert(sizeof(FrameBuffer) == 0x20);

// The picture that came out, in display order (there is no timestamp on it). Two planes, linear:
// luma, then Cb and Cr pairs from buffer + pitch_bytes * height. 8-bit is NV12; 10-bit has the
// value in the low bits of each 16-bit word (pitch_bytes == 2 * pitch), not shifted up as P010.
struct OutputInfo
{
    std::uint64_t size;
    std::uint8_t valid;
    std::uint8_t error;
    std::uint8_t picture_count;
    std::uint8_t padding;
    std::uint32_t codec;
    std::uint32_t width;
    std::uint32_t pitch; // in samples
    std::uint32_t height;
    std::uint32_t reserved;
    void *buffer;
    std::uint64_t buffer_size;
    std::uint32_t frame_format;
    std::uint32_t pitch_bytes;
};
static_assert(sizeof(OutputInfo) == 0x38);

} // namespace hui::ps5::videodec2

extern "C"
{
std::int32_t
sceVideodec2QueryComputeMemoryInfo(hui::ps5::videodec2::ComputeMemoryInfo *memory);
std::int32_t sceVideodec2AllocateComputeQueue(const hui::ps5::videodec2::ComputeConfigInfo *config,
                                              const hui::ps5::videodec2::ComputeMemoryInfo *memory,
                                              void **compute_queue);
std::int32_t sceVideodec2ReleaseComputeQueue(void *compute_queue);
std::int32_t
sceVideodec2QueryDecoderMemoryInfo(const hui::ps5::videodec2::DecoderConfigInfo *config,
                                   hui::ps5::videodec2::DecoderMemoryInfo *memory);
std::int32_t sceVideodec2CreateDecoder(const hui::ps5::videodec2::DecoderConfigInfo *config,
                                       const hui::ps5::videodec2::DecoderMemoryInfo *memory,
                                       void **decoder);
std::int32_t sceVideodec2DeleteDecoder(void *decoder);
std::int32_t sceVideodec2Reset(void *decoder);
std::int32_t sceVideodec2Decode(void *decoder, const hui::ps5::videodec2::InputData *input,
                                hui::ps5::videodec2::FrameBuffer *frame,
                                hui::ps5::videodec2::OutputInfo *output);
std::int32_t sceVideodec2Flush(void *decoder, hui::ps5::videodec2::FrameBuffer *frame,
                               hui::ps5::videodec2::OutputInfo *output);
}
