// EliVier Explorer - Video decoding on the CPU with libavcodec, and dav1d for AV1.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video/video_decoder.hpp"

#include <memory>
#include <string>

namespace hui::video
{

struct SoftwareLimits
{
    // The app heap is 128 MiB and the console has no swap, so what the CPU decodes is bounded.
    // 1920 x 1088 is what a 1080p stream needs; bigger pictures are left to the hardware decoder.
    // (An assumption until it is measured on the console: docs/decision-log.md, D41.)
    int max_pixels = 1920 * 1088;
    // Frame threads add one picture of delay and memory each.
    int max_threads = 4;
};

// Null and `error` filled when the stream cannot be decoded here: no decoder for the codec, a
// picture above the limit, or a pixel format the GPU path does not read (gray, RGB, big-endian,
// packed).
std::unique_ptr<VideoDecoder> open_software_decoder(const Demuxer &demuxer, int stream,
                                                    const SoftwareLimits &limits,
                                                    std::string *error);

} // namespace hui::video
