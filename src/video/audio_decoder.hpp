// EliVier Explorer - Audio decoding for video files: any codec in, stereo float at 48 kHz out.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libavcodec decodes and libswresample does the rest in one pass: it mixes 5.1 and 7.1 down to
// stereo (scaled so that the sum cannot clip) and resamples to the mixer's rate with FFmpeg's
// polyphase filter, which is far better than the linear resampler the MP3 player uses. The
// console's output is stereo only for now (docs/decision-log.md, D43).

#pragma once

#include "video/av_sync.hpp"
#include "video/decoder_result.hpp"
#include "video/demuxer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hui::video
{

// The rate of the mixer and of the console's audio output (src/platform/ps5/audio_out.cpp).
inline constexpr int kAudioOutputRate = 48000;

struct AudioChunk
{
    std::vector<float> stereo; // interleaved L, R at kAudioOutputRate
    double pts = kNoTimestamp; // seconds from the start of the file, of the first frame
    int frames() const
    {
        return static_cast<int>(stereo.size() / 2);
    }
};

class AudioDecoder
{
  public:
    virtual ~AudioDecoder() = default;

    virtual const std::string &name() const = 0;

    // A damaged packet is not an error: it is counted and playback goes on.
    virtual SendResult send(const Packet &packet) = 0;
    // No more packets: what is still inside comes out through receive(), then `end`.
    virtual SendResult send_end() = 0;
    // ReceiveResult::frame means `chunk` holds sound.
    virtual ReceiveResult receive(AudioChunk &chunk) = 0;
    // After a seek: forget everything in flight.
    virtual void flush() = 0;

    virtual int damaged_packets() const = 0;
};

// A soft limit for mixed-down sound (docs/decision-log.md, D56): unchanged up to 0.8, then bent
// smoothly towards 1.0 so that the rare peak of a full-level surround mix-down never clips hard.
// Continuous and with slope 1 at the knee; odd, so negative samples mirror positive ones.
float limit_sample(float sample);

// Null and `error` filled when the stream cannot be played: no decoder for the codec, more than
// eight channels, or a sample rate outside 8 to 192 kHz.
std::unique_ptr<AudioDecoder> open_audio_decoder(const Demuxer &demuxer, int stream,
                                                 std::string *error);

} // namespace hui::video
