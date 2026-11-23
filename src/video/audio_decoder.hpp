// EliVier Explorer - Audio decoding for video files: any codec in, stereo or 7.1 float at 48 kHz out.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libavcodec decodes and libswresample does the rest in one pass: it mixes 5.1 and 7.1 down to
// stereo at the usual weights (D56), or lays them out on eight channels for a surround output
// (D65), and resamples to the mixer's rate with FFmpeg's polyphase filter, which is far better
// than the linear resampler the MP3 player uses.

#pragma once

#include "audio/surround.hpp"
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

// The console's eight-channel order, as VLC and MPC for PS5 feed it (D65; to be confirmed on a
// receiver with the speakers probe): front left, front right, centre, LFE, the side pair (where
// a 5.1 film's surrounds go), the back pair.
constexpr int kSurroundChannels = audio::kSurroundChannels;

struct AudioChunk
{
    std::vector<float> samples; // interleaved, `channels` per frame, at kAudioOutputRate
    int channels = 2;           // 2 (L, R) or kSurroundChannels
    double pts = kNoTimestamp;  // seconds from the start of the file, of the first frame
    int frames() const
    {
        return static_cast<int>(samples.size() / static_cast<std::size_t>(channels));
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
// `channels` is what comes out: 2 (mixed down when the source has more) or kSurroundChannels
// (in the order above; a source with fewer channels leaves the rest silent).
std::unique_ptr<AudioDecoder> open_audio_decoder(const Demuxer &demuxer, int stream,
                                                 std::string *error, int channels = 2);

} // namespace hui::video
