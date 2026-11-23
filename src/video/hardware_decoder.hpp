// EliVier Explorer - Video decoded by the console's hardware, behind the same seam as software.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console's decoder (libSceVideodec2, src/platform/ps5/hw_video.cpp) takes one access unit at
// a time in Annex B form and writes the pictures in display order, without timestamps, into
// buffers the caller lends it: two planes, linear, luma then Cb/Cr pairs (docs/decision-log.md,
// D58 and D60). Everything around that is here, free of the SDK so that it is tested on a PC
// with a fake backend: which streams may go to the hardware, the conversion to Annex B (FFmpeg's
// bitstream filters, rebuilt after a seek because a flushed filter does not repeat the parameter
// sets), the pairing of pictures with timestamps, and waiting for a keyframe after a seek.

#pragma once

#include "video/demuxer.hpp"
#include "video/video_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hui::video
{

enum class HardwareCodec
{
    avc,
    hevc,
};

// What a backend is asked to decode.
struct HardwareStream
{
    HardwareCodec codec = HardwareCodec::avc;
    int bit_depth = 8;
    int width = 0;
    int height = 0;
};

// One picture the hardware wrote: luma from `luma`, Cb/Cr pairs from luma + pitch_bytes *
// coded_height. `owner` holds the backend's buffer until the picture has been converted.
struct HardwarePicture
{
    const std::uint8_t *luma = nullptr;
    int pitch_bytes = 0;
    int coded_height = 0;
    bool wide = false; // 16-bit words (the value in the low bits) instead of bytes
    std::shared_ptr<const void> owner;
};

class HardwareBackend
{
  public:
    virtual ~HardwareBackend() = default;
    virtual const std::string &name() const = 0;
    // One access unit (Annex B). Pictures that came out are appended, in display order. False
    // when the decoder refused it.
    virtual bool decode(const std::uint8_t *data, std::size_t size,
                        std::vector<HardwarePicture> &out) = 0;
    // Everything still inside, at the end of the stream.
    virtual bool flush(std::vector<HardwarePicture> &out) = 0;
    // After a seek: forget everything in flight.
    virtual void reset() = 0;
};

// Lends a backend for a stream, or null when it cannot (codec, size, or the decoder is in use).
// Set by the platform once its decoders are up; unset (null) on a PC and in the software-only
// build. Not thread-safe: set it before any film is opened.
using HardwareProvider = std::function<std::unique_ptr<HardwareBackend>(const HardwareStream &)>;
void set_hardware_provider(HardwareProvider provider);

// What the hardware can be given at all, from the stream's parameters: H.264 (8-bit 4:2:0, up to
// 1920 x 1088) and HEVC (Main or Main 10, 4:2:0, up to 3840 x 2176). False for everything else.
bool hardware_stream_for(const AVCodecParameters &parameters, HardwareStream &stream);

// A decoder on the hardware for `stream`, or null (with the reason in `error`) when there is no
// provider, the stream is not one the hardware takes, or the provider declined.
std::unique_ptr<VideoDecoder> open_hardware_decoder(const Demuxer &demuxer, int stream,
                                                    std::string *error);

} // namespace hui::video
