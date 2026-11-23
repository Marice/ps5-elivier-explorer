// EliVier Explorer - Subtitle tracks inside a media file: cues out of Matroska and MP4 packets.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_fixtures.hpp"

#include "video/subtitle_decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{

using namespace hui::video;
using hui::testing::reader_for;

// The times of this clip are all about 21 ms late: its AAC audio starts one codec frame before
// zero, the demuxer counts from the earliest stream, and so every stream moves by the same amount
// (the picture and the subtitles stay in step). The tolerance covers that.
constexpr double kClipOffset = 0.05;

std::vector<std::uint8_t> read_all(const std::string &name)
{
    return hui::testing::read_video_fixture(name);
}

// The stream index of the nth subtitle stream.
int subtitle_stream(const Demuxer &demuxer, int nth)
{
    int seen = 0;
    for (const StreamInfo &stream : demuxer.info().streams)
        if (stream.kind == StreamKind::subtitle && seen++ == nth)
            return stream.index;
    return -1;
}

struct Result
{
    SubtitleTrack track;
    std::string decoder;
    std::string error;
    int damaged = 0;
};

// Reads the whole file once and decodes the packets of one subtitle stream; with `twice` it then
// goes back to the start and reads them again, as the player does after a seek.
Result decode_subtitles(const std::vector<std::uint8_t> &bytes, int nth, bool twice = false)
{
    Result result;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &result.error);
    if (!demuxer)
        return result;
    const int stream = subtitle_stream(*demuxer, nth);
    if (stream < 0)
    {
        result.error = "no such subtitle stream";
        return result;
    }
    auto decoder = open_subtitle_decoder(*demuxer, stream, &result.error);
    if (!decoder)
        return result;
    result.decoder = decoder->name();
    for (int pass = 0; pass < (twice ? 2 : 1); ++pass)
    {
        if (pass == 1)
        {
            if (!demuxer->seek(0.0))
                break;
            decoder->flush();
        }
        Packet packet;
        for (int guard = 0; guard < 100000; ++guard)
        {
            if (demuxer->read(packet) != ReadResult::packet)
                break;
            if (packet.stream == stream)
                decoder->decode(packet, result.track);
        }
    }
    result.damaged = decoder->damaged_packets();
    return result;
}

TEST(SubtitleDecoder, ReadsTheSrtTrackOfAMatroskaFile)
{
    const Result result = decode_subtitles(read_all("multi-track.mkv"), 0);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.decoder, "srt"); // the decoder's name; the codec is called subrip
    ASSERT_EQ(result.track.size(), 2u);
    const auto &cues = result.track.cues();
    EXPECT_EQ(cues[0].text, "Hello, this is the first line.");
    EXPECT_NEAR(cues[0].start, 0.2, kClipOffset);
    EXPECT_NEAR(cues[0].end, 0.9, kClipOffset);
    EXPECT_EQ(cues[1].text, "And this is the second line.");
    EXPECT_NEAR(cues[1].start, 1.0, kClipOffset);
    EXPECT_NEAR(cues[1].end, 1.8, kClipOffset);
}

TEST(SubtitleDecoder, ReadsTheAssTrackAndDropsItsOverrides)
{
    const Result result = decode_subtitles(read_all("multi-track.mkv"), 1);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.decoder, "ssa"); // the decoder of the ASS codec
    ASSERT_EQ(result.track.size(), 2u);
    const auto &cues = result.track.cues();
    EXPECT_EQ(cues[0].text, "Styled line one"); // {\i1} and {\i0} are gone
    EXPECT_NEAR(cues[0].start, 0.2, kClipOffset);
    EXPECT_NEAR(cues[0].end, 0.9, kClipOffset);
    EXPECT_EQ(cues[1].text, "Styled line two");
}

TEST(SubtitleDecoder, TheSameCuesAfterASeekAreNotAddedTwice)
{
    const Result result = decode_subtitles(read_all("multi-track.mkv"), 0, true);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.track.size(), 2u);
}

TEST(SubtitleDecoder, TheTrackAnswersByTime)
{
    const Result result = decode_subtitles(read_all("multi-track.mkv"), 0);
    ASSERT_EQ(result.track.size(), 2u);
    EXPECT_TRUE(result.track.active_at(0.05).empty());
    ASSERT_EQ(result.track.active_at(0.5).size(), 1u);
    EXPECT_EQ(result.track.active_at(0.5)[0]->text, "Hello, this is the first line.");
    EXPECT_TRUE(result.track.active_at(0.95).empty()); // between the two lines
    ASSERT_EQ(result.track.active_at(1.5).size(), 1u);
}

TEST(SubtitleDecoder, RefusesAStreamThatIsNotASubtitle)
{
    const auto bytes = read_all("multi-track.mkv");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer) << error;
    EXPECT_FALSE(open_subtitle_decoder(*demuxer, 0, &error)); // stream 0 is the picture
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(open_subtitle_decoder(*demuxer, 99, &error));
    EXPECT_FALSE(error.empty());
}

TEST(SubtitleDecoder, SurvivesDamagedData)
{
    // Bytes flipped inside the file: cues may be wrong or missing; nothing may crash or hang.
    const auto original = read_all("multi-track.mkv");
    std::uint32_t state = 99;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 40; ++round)
    {
        std::vector<std::uint8_t> damaged = original;
        for (int flips = 0; flips < 3 + round % 9; ++flips)
            damaged[next() % damaged.size()] ^= static_cast<std::uint8_t>(1u << (next() % 8));
        (void)decode_subtitles(damaged, round % 2);
    }
    SUCCEED();
}

} // namespace
