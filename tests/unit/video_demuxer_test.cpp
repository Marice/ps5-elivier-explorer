// EliVier Explorer - Demuxer tests: streams, packets, seeking and hostile input.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/demuxer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

using namespace hui::video;

const std::string kDir = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/video/";

std::vector<std::uint8_t> read_all(const std::string &name)
{
    std::ifstream file(kDir + name, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
}

hui::audio::ByteReader reader_for(const std::vector<std::uint8_t> &bytes)
{
    return [&bytes](std::uint64_t offset, void *buffer, std::size_t size) -> std::int64_t {
        if (offset >= bytes.size())
            return 0;
        const std::size_t count = std::min<std::size_t>(size, bytes.size() - offset);
        std::memcpy(buffer, bytes.data() + offset, count);
        return static_cast<std::int64_t>(count);
    };
}

std::unique_ptr<Demuxer> open_fixture(const std::vector<std::uint8_t> &bytes, std::string *error)
{
    return Demuxer::open(reader_for(bytes), bytes.size(), error);
}

const StreamInfo *first_of(const MediaInfo &info, StreamKind kind)
{
    for (const StreamInfo &stream : info.streams)
        if (stream.kind == kind)
            return &stream;
    return nullptr;
}

int count_of(const MediaInfo &info, StreamKind kind)
{
    return static_cast<int>(std::count_if(info.streams.begin(), info.streams.end(),
                                          [kind](const StreamInfo &s) { return s.kind == kind; }));
}

TEST(Demuxer, ListsTheStreamsOfAnMp4)
{
    const auto bytes = read_all("h264-aac.mp4");
    ASSERT_FALSE(bytes.empty());
    std::string error;
    auto demuxer = open_fixture(bytes, &error);
    ASSERT_TRUE(demuxer) << error;

    const MediaInfo &info = demuxer->info();
    EXPECT_NE(info.container.find("mp4"), std::string::npos) << info.container;
    EXPECT_NEAR(info.duration, 2.0, 0.05);
    ASSERT_EQ(info.streams.size(), 2u);

    const StreamInfo *video = first_of(info, StreamKind::video);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->codec, "h264");
    EXPECT_EQ(video->profile, "High");
    EXPECT_EQ(video->width, 160);
    EXPECT_EQ(video->height, 90);
    EXPECT_EQ(video->bit_depth, 8);
    EXPECT_NEAR(video->frame_rate, 24.0, 0.01);
    EXPECT_EQ(video->colour.transfer, Transfer::sdr);

    const StreamInfo *audio = first_of(info, StreamKind::audio);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->codec, "aac");
    EXPECT_EQ(audio->channels, 1);
    EXPECT_EQ(audio->sample_rate, 48000);
}

TEST(Demuxer, ReadsTheHdrFlagsOfAnHevcMain10File)
{
    const auto bytes = read_all("hevc-main10-hdr.mkv");
    std::string error;
    auto demuxer = open_fixture(bytes, &error);
    ASSERT_TRUE(demuxer) << error;

    const StreamInfo *video = first_of(demuxer->info(), StreamKind::video);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->codec, "hevc");
    EXPECT_EQ(video->profile, "Main 10");
    EXPECT_EQ(video->bit_depth, 10);
    EXPECT_EQ(video->colour.matrix, Matrix::bt2020);
    EXPECT_EQ(video->colour.transfer, Transfer::pq);
    EXPECT_EQ(video->colour.range, Range::limited);
}

TEST(Demuxer, KnowsVp9WithOpusAndAv1)
{
    {
        const auto bytes = read_all("vp9-opus.webm");
        std::string error;
        auto demuxer = open_fixture(bytes, &error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(first_of(demuxer->info(), StreamKind::video)->codec, "vp9");
        EXPECT_EQ(first_of(demuxer->info(), StreamKind::audio)->codec, "opus");
    }
    {
        const auto bytes = read_all("av1.mkv");
        std::string error;
        auto demuxer = open_fixture(bytes, &error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(first_of(demuxer->info(), StreamKind::video)->codec, "av1");
        EXPECT_EQ(count_of(demuxer->info(), StreamKind::audio), 0);
    }
}

TEST(Demuxer, ListsAudioAndSubtitleTracksWithLanguageAndTitle)
{
    const auto bytes = read_all("multi-track.mkv");
    std::string error;
    auto demuxer = open_fixture(bytes, &error);
    ASSERT_TRUE(demuxer) << error;
    const MediaInfo &info = demuxer->info();

    ASSERT_EQ(count_of(info, StreamKind::video), 1);
    ASSERT_EQ(count_of(info, StreamKind::audio), 2);
    ASSERT_EQ(count_of(info, StreamKind::subtitle), 2);

    std::vector<const StreamInfo *> audio;
    std::vector<const StreamInfo *> subtitles;
    for (const StreamInfo &stream : info.streams)
    {
        if (stream.kind == StreamKind::audio)
            audio.push_back(&stream);
        if (stream.kind == StreamKind::subtitle)
            subtitles.push_back(&stream);
    }
    EXPECT_EQ(audio[0]->language, "eng");
    EXPECT_EQ(audio[0]->title, "English");
    EXPECT_EQ(audio[1]->language, "nld");
    EXPECT_EQ(audio[1]->title, "Nederlands");
    EXPECT_EQ(subtitles[0]->codec, "subrip");
    EXPECT_EQ(subtitles[0]->language, "eng");
    EXPECT_EQ(subtitles[1]->codec, "ass");
    EXPECT_EQ(subtitles[1]->language, "nld");
}

TEST(Demuxer, ReadsEveryPacketAndThenReportsTheEnd)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto demuxer = open_fixture(bytes, &error);
    ASSERT_TRUE(demuxer) << error;

    int video_packets = 0;
    int audio_packets = 0;
    bool first_video_is_key = false;
    double min_pts = 1e9;
    double max_pts = -1e9;
    Packet packet;
    for (int guard = 0; guard < 10000; ++guard)
    {
        const ReadResult result = demuxer->read(packet);
        if (result != ReadResult::packet)
        {
            EXPECT_EQ(result, ReadResult::end);
            break;
        }
        EXPECT_TRUE(packet.data);
        const StreamInfo &stream = demuxer->info().streams[static_cast<std::size_t>(packet.stream)];
        if (stream.kind == StreamKind::video)
        {
            if (video_packets == 0)
                first_video_is_key = packet.keyframe;
            ++video_packets;
            min_pts = std::min(min_pts, packet.pts);
            max_pts = std::max(max_pts, packet.pts);
        }
        else
        {
            ++audio_packets;
        }
    }
    EXPECT_EQ(video_packets, 48);
    EXPECT_GT(audio_packets, 10);
    EXPECT_TRUE(first_video_is_key);
    EXPECT_NEAR(min_pts, 0.0, 0.05);
    EXPECT_NEAR(max_pts, 2.0 - 1.0 / 24.0, 0.05);
    EXPECT_EQ(demuxer->read(packet), ReadResult::end); // and it stays at the end
}

TEST(Demuxer, SeekLandsOnTheKeyframeAtOrBeforeTheTime)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto demuxer = open_fixture(bytes, &error);
    ASSERT_TRUE(demuxer) << error;
    ASSERT_TRUE(demuxer->seek(1.2));

    Packet packet;
    for (int guard = 0; guard < 1000; ++guard)
    {
        ASSERT_EQ(demuxer->read(packet), ReadResult::packet);
        const StreamInfo &stream = demuxer->info().streams[static_cast<std::size_t>(packet.stream)];
        if (stream.kind != StreamKind::video)
            continue;
        EXPECT_TRUE(packet.keyframe);
        EXPECT_NEAR(packet.pts, 1.0, 0.02); // the keyframes are at 0, 0.5, 1.0 and 1.5 seconds
        return;
    }
    FAIL() << "no video packet after the seek";
}

TEST(Demuxer, SeekPastTheEndIsRefusedOrEndsAtOnce)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto demuxer = open_fixture(bytes, &error);
    ASSERT_TRUE(demuxer) << error;
    if (!demuxer->seek(500.0))
        return; // refused: fine
    Packet packet;
    EXPECT_NE(demuxer->read(packet), ReadResult::error);
}

TEST(Demuxer, RefusesFilesThatAreNotMedia)
{
    std::string error;
    const std::vector<std::uint8_t> zeros(4096, 0);
    EXPECT_FALSE(open_fixture(zeros, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    std::string note = "This is only a note, not a film. ";
    note.append(2000, 'x');
    const std::vector<std::uint8_t> text(note.begin(), note.end());
    EXPECT_FALSE(open_fixture(text, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(open_fixture({}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(Demuxer, ReportsAReadErrorInsteadOfCrashing)
{
    const auto bytes = read_all("h264-aac.mp4");
    // The header (moov is first in this file) reads fine; the data after 8 KiB fails.
    const hui::audio::ByteReader failing = [&bytes](std::uint64_t offset, void *buffer,
                                                    std::size_t size) -> std::int64_t {
        if (offset >= 8192)
            return -EIO;
        const std::size_t count =
            std::min<std::size_t>({size, bytes.size() - offset, 8192 - offset});
        std::memcpy(buffer, bytes.data() + offset, count);
        return static_cast<std::int64_t>(count);
    };
    std::string error;
    auto demuxer = Demuxer::open(failing, bytes.size(), &error);
    ASSERT_TRUE(demuxer) << error;
    Packet packet;
    ReadResult result = ReadResult::packet;
    for (int guard = 0; guard < 10000 && result == ReadResult::packet; ++guard)
        result = demuxer->read(packet);
    EXPECT_NE(result, ReadResult::packet); // it stopped, with an error or at the end
}

TEST(Demuxer, SurvivesTruncatedAndCorruptedFiles)
{
    // Deterministic damage to every clip: cut it short, then flip bytes. Opening may fail and
    // reading may stop early, but nothing may crash, hang or read outside its buffer (this runs
    // under AddressSanitizer and UBSan, though FFmpeg itself is not instrumented).
    const char *names[] = {"h264-aac.mp4", "hevc-main10-hdr.mkv", "vp9-opus.webm", "av1.mkv",
                           "multi-track.mkv"};
    std::uint32_t state = 12345;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (const char *name : names)
    {
        const auto original = read_all(name);
        ASSERT_FALSE(original.empty()) << name;
        for (int round = 0; round < 40; ++round)
        {
            std::vector<std::uint8_t> damaged = original;
            if (round % 4 == 0)
                damaged.resize(next() % damaged.size());
            for (int flips = 0; flips < 1 + round % 7 && !damaged.empty(); ++flips)
                damaged[next() % damaged.size()] ^= static_cast<std::uint8_t>(1u << (next() % 8));
            std::string error;
            auto demuxer = Demuxer::open(reader_for(damaged), damaged.size(), &error);
            if (!demuxer)
                continue;
            Packet packet;
            for (int guard = 0; guard < 5000; ++guard)
                if (demuxer->read(packet) != ReadResult::packet)
                    break;
        }
    }
    SUCCEED();
}

} // namespace
