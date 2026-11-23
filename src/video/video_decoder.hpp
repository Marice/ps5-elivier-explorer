// EliVier Explorer - A video decoder turns compressed packets into pictures the GPU can read.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One interface for every way of decoding: the software decoder (libavcodec, dav1d) and, later,
// the console's hardware decoder. The player never asks which one it has. A picture is planar
// YCbCr described by plane pointers, pitches and sample size, so a texture upload needs no
// conversion on the CPU.

#pragma once

#include "video/av_sync.hpp"
#include "video/colour.hpp"
#include "video/demuxer.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hui::video
{

struct Frame
{
    // Plane 0 is luma. Planar: planes 1 and 2 are Cb and Cr. Semi-planar (NV12, P010): plane 1
    // holds Cb and Cr interleaved and plane 2 is unused.
    const std::uint8_t *planes[3] = {nullptr, nullptr, nullptr};
    int pitches[3] = {0, 0, 0}; // bytes per row
    int plane_count = 0;
    bool semi_planar = false;

    int width = 0; // the visible picture; the buffers may be padded to more
    int height = 0;
    // 8 means one byte per sample. Above 8, every sample is a 16-bit little-endian word with the
    // value in its low bits (10-bit video: 0..1023), not shifted up as P010 does.
    int bit_depth = 8;
    // log2 of how much smaller the chroma planes are: 1,1 is 4:2:0; 1,0 is 4:2:2; 0,0 is 4:4:4.
    int chroma_shift_x = 1;
    int chroma_shift_y = 1;

    ColourInfo colour;
    double pts = kNoTimestamp; // seconds from the start of the file

    // Keeps the buffers alive for as long as the Frame (or a copy of it) is held. A decoder may
    // wait for them to come back before it can decode more, so do not hoard pictures.
    std::shared_ptr<const void> owner;
};

enum class SendResult
{
    accepted,
    full,  // take pictures out with receive(), then offer the packet again
    error, // the decoder cannot go on
};

enum class ReceiveResult
{
    frame,
    need_input, // give it more packets
    end,        // after send_end(): everything has come out
    error,
};

class VideoDecoder
{
  public:
    virtual ~VideoDecoder() = default;

    // "h264", "hevc", "libdav1d": what is doing the work, for logs and the info line.
    virtual const std::string &name() const = 0;

    // A damaged packet is not an error: it is counted and playback goes on.
    virtual SendResult send(const Packet &packet) = 0;
    // No more packets: the pictures still inside come out through receive(), then `end`.
    virtual SendResult send_end() = 0;
    virtual ReceiveResult receive(Frame &frame) = 0;
    // After a seek: forget everything in flight.
    virtual void flush() = 0;

    virtual int damaged_packets() const = 0;
};

} // namespace hui::video
