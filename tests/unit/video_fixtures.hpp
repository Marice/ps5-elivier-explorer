// EliVier Explorer - Reading the synthetic video clips in tests/fixtures/video.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "audio/decoder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace hui::testing
{

inline std::vector<std::uint8_t> read_video_fixture(const std::string &name)
{
    std::ifstream file(std::string(HUI_SOURCE_DIR) + "/tests/fixtures/video/" + name,
                       std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
}

// A ByteReader over bytes that must outlive it.
inline hui::audio::ByteReader reader_for(const std::vector<std::uint8_t> &bytes)
{
    return [&bytes](std::uint64_t offset, void *buffer, std::size_t size) -> std::int64_t {
        if (offset >= bytes.size())
            return 0;
        const std::size_t count = std::min<std::size_t>(size, bytes.size() - offset);
        std::memcpy(buffer, bytes.data() + offset, count);
        return static_cast<std::int64_t>(count);
    };
}

} // namespace hui::testing
