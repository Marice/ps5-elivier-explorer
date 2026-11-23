// EliVier Explorer - Audio decoder tests: codecs, track choice, downmix, timing and damaged data.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_fixtures.hpp"

#include "video/audio_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

using namespace hui::video;
using hui::testing::reader_for;

std::vector<std::uint8_t> read_all(const std::string &name)
{
    return hui::testing::read_video_fixture(name);
}

// The stream index of the nth audio stream (0 for the first).
int audio_stream(const Demuxer &demuxer, int nth)
{
    int seen = 0;
    for (const StreamInfo &stream : demuxer.info().streams)
        if (stream.kind == StreamKind::audio && seen++ == nth)
            return stream.index;
    return -1;
}

struct Sound
{
    std::vector<float> stereo; // everything that came out, interleaved
    std::vector<double> chunk_pts;
    std::vector<int> chunk_frames;
    std::string decoder;
    std::string error;
    int damaged = 0;
    int frames() const
    {
        return static_cast<int>(stereo.size() / 2);
    }
};

Sound decode_audio(const std::vector<std::uint8_t> &bytes, int nth = 0)
{
    Sound sound;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &sound.error);
    if (!demuxer)
        return sound;
    const int stream = audio_stream(*demuxer, nth);
    if (stream < 0)
    {
        sound.error = "no such audio stream";
        return sound;
    }
    auto decoder = open_audio_decoder(*demuxer, stream, &sound.error);
    if (!decoder)
        return sound;
    sound.decoder = decoder->name();

    const auto drain = [&]() {
        AudioChunk chunk;
        ReceiveResult got = decoder->receive(chunk);
        while (got == ReceiveResult::frame)
        {
            sound.chunk_pts.push_back(chunk.pts);
            sound.chunk_frames.push_back(chunk.frames());
            sound.stereo.insert(sound.stereo.end(), chunk.stereo.begin(), chunk.stereo.end());
            got = decoder->receive(chunk);
        }
        return got;
    };

    Packet packet;
    for (int guard = 0; guard < 100000; ++guard)
    {
        if (demuxer->read(packet) != ReadResult::packet)
            break;
        if (packet.stream != stream)
            continue;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const SendResult sent = decoder->send(packet);
            if (sent == SendResult::accepted)
                break;
            if (sent == SendResult::error)
            {
                sound.error = "send failed";
                return sound;
            }
            drain();
        }
        drain();
    }
    decoder->send_end();
    drain();
    sound.damaged = decoder->damaged_packets();
    return sound;
}

// Pitch from rising zero crossings of the left channel, leaving out the ends (codec priming).
double frequency_of(const Sound &sound)
{
    const int margin = 4800;
    const int frames = sound.frames();
    if (frames < 2 * margin)
        return 0.0;
    int crossings = 0;
    for (int i = margin + 1; i < frames - margin; ++i)
        if (sound.stereo[static_cast<std::size_t>(2 * (i - 1))] < 0.0f &&
            sound.stereo[static_cast<std::size_t>(2 * i)] >= 0.0f)
            ++crossings;
    return crossings / (static_cast<double>(frames - 2 * margin) / kAudioOutputRate);
}

float peak_of(const Sound &sound)
{
    float peak = 0.0f;
    for (const float value : sound.stereo)
        peak = std::max(peak, std::fabs(value));
    return peak;
}

TEST(AudioDecoder, AacMonoComesOutAsStereoAt48k)
{
    const Sound sound = decode_audio(read_all("h264-aac.mp4"));
    ASSERT_TRUE(sound.error.empty()) << sound.error;
    EXPECT_EQ(sound.decoder, "aac");
    // Two seconds, give or take the codec's priming and padding.
    EXPECT_NEAR(sound.frames(), 2 * kAudioOutputRate, 0.03 * 2 * kAudioOutputRate);
    EXPECT_NEAR(frequency_of(sound), 440.0, 440.0 * 0.03);
    for (int i = 0; i < sound.frames(); i += 997) // a mono source is the same in both ears
        ASSERT_EQ(sound.stereo[static_cast<std::size_t>(2 * i)],
                  sound.stereo[static_cast<std::size_t>(2 * i + 1)]);
}

TEST(AudioDecoder, EachTrackPlaysItsOwnTone)
{
    const auto bytes = read_all("multi-track.mkv");
    const Sound english = decode_audio(bytes, 0);
    const Sound dutch = decode_audio(bytes, 1);
    ASSERT_TRUE(english.error.empty()) << english.error;
    ASSERT_TRUE(dutch.error.empty()) << dutch.error;
    EXPECT_NEAR(frequency_of(english), 440.0, 440.0 * 0.03);
    EXPECT_NEAR(frequency_of(dutch), 880.0, 880.0 * 0.03);
}

TEST(AudioDecoder, FlacIsDecodedWithItsLevelIntact)
{
    const Sound sound = decode_audio(read_all("hevc-main10-hdr.mkv"));
    ASSERT_TRUE(sound.error.empty()) << sound.error;
    EXPECT_EQ(sound.decoder, "flac");
    EXPECT_NEAR(peak_of(sound), 0.125f, 0.003f); // FFmpeg's sine source has an amplitude of 1/8
    EXPECT_NEAR(frequency_of(sound), 440.0, 440.0 * 0.01);
}

TEST(AudioDecoder, OpusInWebm)
{
    const Sound sound = decode_audio(read_all("vp9-opus.webm"));
    ASSERT_TRUE(sound.error.empty()) << sound.error;
    EXPECT_EQ(sound.decoder, "opus");
    EXPECT_NEAR(frequency_of(sound), 440.0, 440.0 * 0.03);
    EXPECT_GT(sound.frames(), kAudioOutputRate); // more than a second came out
}

TEST(AudioDecoder, TheLimitLeavesNormalSoundAloneAndBendsPeaksBelowFullScale)
{
    EXPECT_EQ(limit_sample(0.0f), 0.0f);
    EXPECT_EQ(limit_sample(0.5f), 0.5f);
    EXPECT_EQ(limit_sample(-0.8f), -0.8f);
    EXPECT_NEAR(limit_sample(0.801f), 0.801f, 1e-3f); // no step at the knee
    float last = 0.8f;
    for (int i = 1; i <= 100; ++i)
    {
        const float bent = limit_sample(0.8f + 0.05f * static_cast<float>(i));
        EXPECT_GT(bent, last); // louder stays louder
        EXPECT_LT(bent, 1.0f);
        EXPECT_EQ(limit_sample(-0.8f - 0.05f * static_cast<float>(i)), -bent);
        last = bent;
    }
}

TEST(AudioDecoder, SurroundIsMixedDownAtTheUsualWeights)
{
    const auto bytes = read_all("ac3-surround.mkv");
    {
        // The file really has six channels to mix.
        std::string error;
        auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->info().streams[0].channels, 6);
    }
    const Sound sound = decode_audio(bytes);
    ASSERT_TRUE(sound.error.empty()) << sound.error;
    EXPECT_EQ(sound.decoder, "ac3");
    // The same 0.125 tone on every channel. Front + 0.707 centre + 0.707 surround gives about
    // 0.30 on each side (LFE is left out); the old scaled matrix gave about 0.125 (D56).
    EXPECT_GT(peak_of(sound), 0.25f);
    EXPECT_LT(peak_of(sound), 0.36f);
}

TEST(AudioDecoder, ChunksFollowOneAnotherInTime)
{
    const Sound sound = decode_audio(read_all("h264-aac.mp4"));
    ASSERT_GT(sound.chunk_pts.size(), 10u);
    for (std::size_t i = 1; i < sound.chunk_pts.size(); ++i)
    {
        const double expected = sound.chunk_pts[i - 1] + sound.chunk_frames[i - 1] /
                                                             static_cast<double>(kAudioOutputRate);
        EXPECT_NEAR(sound.chunk_pts[i], expected, 0.003) << i;
    }
}

TEST(AudioDecoder, FlushAfterASeekContinuesFromTheNewTime)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer) << error;
    const int stream = audio_stream(*demuxer, 0);
    auto decoder = open_audio_decoder(*demuxer, stream, &error);
    ASSERT_TRUE(decoder) << error;

    ASSERT_TRUE(demuxer->seek(1.0));
    decoder->flush();
    Packet packet;
    AudioChunk chunk;
    double first = -1.0;
    for (int guard = 0; guard < 1000 && first < 0.0; ++guard)
    {
        ASSERT_EQ(demuxer->read(packet), ReadResult::packet);
        if (packet.stream != stream)
            continue;
        ASSERT_EQ(decoder->send(packet), SendResult::accepted);
        if (decoder->receive(chunk) == ReceiveResult::frame)
            first = chunk.pts;
    }
    // The codec needs a few frames of run-up, so the sound starts a little before the wanted time
    // (here about 0.1 s). The player drops the part before its target; the decoder does not guess.
    EXPECT_LE(first, 1.0 + 1e-6);
    EXPECT_GT(first, 0.75);
}

TEST(AudioDecoder, RefusesAStreamThatIsNotAudio)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer) << error;
    EXPECT_FALSE(open_audio_decoder(*demuxer, 0, &error)); // stream 0 is the picture
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(open_audio_decoder(*demuxer, 99, &error));
    EXPECT_FALSE(error.empty());
}

TEST(AudioDecoder, SurvivesDamagedStreams)
{
    // Bytes flipped inside the audio data: the sound may be wrong, nothing may crash or hang.
    const char *names[] = {"h264-aac.mp4", "hevc-main10-hdr.mkv", "vp9-opus.webm",
                           "multi-track.mkv", "ac3-surround.mkv"};
    std::uint32_t state = 4242;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (const char *name : names)
    {
        const auto original = read_all(name);
        ASSERT_FALSE(original.empty()) << name;
        for (int round = 0; round < 12; ++round)
        {
            std::vector<std::uint8_t> damaged = original;
            const std::size_t first_data = original.size() / 4;
            for (int flips = 0; flips < 4 + round; ++flips)
                damaged[first_data + next() % (damaged.size() - first_data)] ^=
                    static_cast<std::uint8_t>(1u << (next() % 8));
            (void)decode_audio(damaged);
        }
    }
    SUCCEED();
}

} // namespace
