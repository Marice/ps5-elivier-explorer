// EliVier Explorer - A decoder turns a compressed file into stereo float frames, a chunk at a time.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A decoder reads its file through a ByteReader (a range read), so a long song is never held in
// memory whole. One decoder per format; the AudioPlayer resamples whatever rate comes out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hui::audio
{

// Bytes at `offset`; returns how many were read (0 at the end) or a negative errno.
using ByteReader =
    std::function<std::int64_t(std::uint64_t offset, void *buffer, std::size_t size)>;

// The picture an MP3 carries in its tag (JPEG or PNG bytes), if any.
struct CoverArt
{
    std::string mime;
    std::string data;
};

class Decoder
{
  public:
    virtual ~Decoder() = default;

    // Native sample rate of what read() returns.
    virtual int rate() const = 0;
    // Length in seconds; 0 when it cannot be known.
    virtual double duration() const = 0;
    // Interleaved stereo (mono is duplicated). Returns the frames written; 0 at the end. A decoder
    // that is fed by another thread (a film's sound) may return kTryLater when it has nothing yet
    // and the end has not come: the player asks again next frame, and its clock waits meanwhile.
    static constexpr int kTryLater = -2;
    virtual int read(float *stereo, int frames) = 0;
    // Jumps to `seconds` (approximate for formats without an index). False when not possible.
    virtual bool seek(double seconds) = 0;

    virtual const std::string &title() const = 0;
    virtual const std::string &artist() const = 0;
    // "MP3, 44.1 kHz, 192 kbps, stereo"
    virtual const std::string &description() const = 0;
    // Empty `data` when the file has none.
    virtual const CoverArt &cover() const = 0;
};

// Returns null and fills `error` when the file is not a playable MP3.
std::unique_ptr<Decoder> open_mp3(ByteReader reader, std::uint64_t size, std::string *error);

} // namespace hui::audio
