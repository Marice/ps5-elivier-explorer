// EliVier Explorer - The hardware decoder's seam, driven by a fake backend.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_fixtures.hpp"

#include "video/convert.hpp"
#include "video/demuxer.hpp"
#include "video/hardware_decoder.hpp"
#include "video/playback_engine.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C"
{
#include <libavcodec/codec_par.h>
}

namespace
{

using namespace hui::video;
using hui::testing::reader_for;

// What the fake saw, kept by the test after the decoder is gone.
struct Seen
{
    std::vector<std::vector<std::uint8_t>> units;
    int resets = 0;
    int flushes = 0;
    int lent = 0;
};

// Behaves as the console's decoder is described (D58, D60): pictures come out a few access units
// after they went in, as a decoder that reorders B-frames into display order holds them, each in
// a buffer it lends, NV12 with a padded pitch.
class FakeBackend final : public HardwareBackend
{
  public:
    FakeBackend(Seen &seen, HardwareStream stream, bool refuse) : seen_(seen), stream_(stream), refuse_(refuse)
    {
    }
    const std::string &name() const override
    {
        return name_;
    }
    bool decode(const std::uint8_t *data, std::size_t size, std::vector<HardwarePicture> &out) override
    {
        seen_.units.emplace_back(data, data + size);
        if (refuse_)
            return false;
        if (held_ == kReorderDepth)
            out.push_back(picture());
        else
            ++held_;
        return true;
    }
    bool flush(std::vector<HardwarePicture> &out) override
    {
        ++seen_.flushes;
        for (; held_ > 0; --held_)
            out.push_back(picture());
        return true;
    }
    void reset() override
    {
        ++seen_.resets;
        held_ = 0;
    }

  private:
    HardwarePicture picture()
    {
        const int pitch = (stream_.width + 63) / 64 * 64;
        const int coded = (stream_.height + 15) / 16 * 16;
        auto buffer = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(pitch) * coded * 3 / 2, 0x80);
        std::fill(buffer->begin(), buffer->begin() + static_cast<std::ptrdiff_t>(pitch) * coded,
                  static_cast<std::uint8_t>(16 + (counter_++ % 200)));
        HardwarePicture p;
        p.luma = buffer->data();
        p.pitch_bytes = pitch;
        p.coded_height = coded;
        p.owner = buffer;
        return p;
    }

    Seen &seen_;
    HardwareStream stream_;
    bool refuse_;
    static constexpr int kReorderDepth = 4; // more than the fixture's B-frames need
    int held_ = 0;
    int counter_ = 0;
    std::string name_ = "fake hardware";
};

// Installs a provider for one test and takes it away again.
class HardwareDecoderTest : public ::testing::Test
{
  protected:
    void provide(bool refuse = false, bool decline = false)
    {
        set_hardware_provider([this, refuse, decline](const HardwareStream &stream)
                                  -> std::unique_ptr<HardwareBackend> {
            if (decline)
                return nullptr;
            ++seen_.lent;
            return std::make_unique<FakeBackend>(seen_, stream, refuse);
        });
    }
    void TearDown() override
    {
        set_hardware_provider(nullptr);
    }

    Seen seen_;
};

bool starts_annex_b(const std::vector<std::uint8_t> &unit)
{
    return (unit.size() > 4 && unit[0] == 0 && unit[1] == 0 && unit[2] == 0 && unit[3] == 1) ||
           (unit.size() > 3 && unit[0] == 0 && unit[1] == 0 && unit[2] == 1);
}

// True when the access unit carries an H.264 sequence parameter set (NAL type 7).
bool has_sps(const std::vector<std::uint8_t> &unit)
{
    for (std::size_t i = 0; i + 3 < unit.size(); ++i)
        if (unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1 && (unit[i + 3] & 0x1F) == 7)
            return true;
    return false;
}

int video_stream(const Demuxer &demuxer)
{
    for (const StreamInfo &stream : demuxer.info().streams)
        if (stream.kind == StreamKind::video)
            return stream.index;
    return -1;
}

TEST(HardwareStreamFor, TakesHighProfileH264AndHevcMainTenAndNothingElse)
{
    const struct
    {
        const char *file;
        bool taken;
        HardwareCodec codec;
        int depth;
    } cases[] = {
        {"h264-aac.mp4", true, HardwareCodec::avc, 8},
        {"hevc-main10-hdr.mkv", true, HardwareCodec::hevc, 10},
        {"vp9-opus.webm", false, HardwareCodec::avc, 0},
        {"av1.mkv", false, HardwareCodec::avc, 0},
        {"h264-yuv444.mp4", false, HardwareCodec::avc, 0},
    };
    for (const auto &item : cases)
    {
        const auto bytes = hui::testing::read_video_fixture(item.file);
        std::string error;
        auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
        ASSERT_TRUE(demuxer) << item.file << ": " << error;
        HardwareStream stream;
        const bool taken = hardware_stream_for(*demuxer->parameters(video_stream(*demuxer)), stream);
        EXPECT_EQ(taken, item.taken) << item.file;
        if (taken)
        {
            EXPECT_EQ(stream.codec, item.codec) << item.file;
            EXPECT_EQ(stream.bit_depth, item.depth) << item.file;
        }
    }
}

TEST_F(HardwareDecoderTest, WithoutAProviderThereIsNoHardwareDecoder)
{
    const auto bytes = hui::testing::read_video_fixture("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer);
    EXPECT_EQ(open_hardware_decoder(*demuxer, video_stream(*demuxer), &error), nullptr);
    EXPECT_FALSE(error.empty());
}

TEST_F(HardwareDecoderTest, GivesAnnexBUnitsAndPairsPicturesWithTimestamps)
{
    provide();
    const auto bytes = hui::testing::read_video_fixture("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer);
    const int index = video_stream(*demuxer);
    auto decoder = open_hardware_decoder(*demuxer, index, &error);
    ASSERT_TRUE(decoder) << error;
    EXPECT_EQ(decoder->name(), "fake hardware");

    std::vector<double> times;
    int packets = 0;
    ConvertTables tables;
    const auto take_all = [&] {
        Frame frame;
        while (decoder->receive(frame) == ReceiveResult::frame)
        {
            EXPECT_TRUE(frame.semi_planar);
            EXPECT_EQ(frame.width, 160);
            EXPECT_EQ(frame.height, 90);
            std::vector<std::uint16_t> rgba(160 * 90 * 4);
            EXPECT_TRUE(convert_to_rgba16(frame, tables, rgba.data(), 160 * 8));
            times.push_back(frame.pts);
        }
    };
    Packet packet;
    while (demuxer->read(packet) == ReadResult::packet)
    {
        if (packet.stream != index)
            continue;
        ++packets;
        while (decoder->send(packet) == SendResult::full)
            take_all();
        take_all();
    }
    decoder->send_end();
    take_all();
    Frame frame;
    EXPECT_EQ(decoder->receive(frame), ReceiveResult::end);

    ASSERT_FALSE(seen_.units.empty());
    EXPECT_TRUE(starts_annex_b(seen_.units.front()));
    EXPECT_TRUE(has_sps(seen_.units.front())); // the parameter sets travel in the stream
    EXPECT_EQ(static_cast<int>(times.size()), packets);
    for (std::size_t i = 1; i < times.size(); ++i)
        EXPECT_LT(times[i - 1], times[i]); // display order, earliest timestamp first
}

TEST_F(HardwareDecoderTest, AfterASeekItResetsWaitsForAKeyframeAndSendsTheParameterSetsAgain)
{
    provide();
    const auto bytes = hui::testing::read_video_fixture("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer);
    const int index = video_stream(*demuxer);
    auto decoder = open_hardware_decoder(*demuxer, index, &error);
    ASSERT_TRUE(decoder) << error;

    Packet packet;
    std::vector<Packet> later;
    while (demuxer->read(packet) == ReadResult::packet)
        if (packet.stream == index)
            later.push_back(std::move(packet));
    ASSERT_GT(later.size(), 3u);
    Frame frame;
    for (int i = 0; i < 3; ++i)
    {
        decoder->send(later[static_cast<std::size_t>(i)]);
        while (decoder->receive(frame) == ReceiveResult::frame)
        {
        }
    }
    decoder->flush();
    EXPECT_EQ(seen_.resets, 1);
    const std::size_t before = seen_.units.size();
    // A packet that is not a keyframe is dropped: the hardware cannot start from it.
    std::size_t first_key = 0;
    for (std::size_t i = 1; i < later.size(); ++i)
        if (!later[i].keyframe)
        {
            EXPECT_EQ(decoder->send(later[i]), SendResult::accepted);
            break;
        }
    EXPECT_EQ(seen_.units.size(), before);
    for (std::size_t i = 0; i < later.size(); ++i)
        if (later[i].keyframe)
            first_key = i;
    EXPECT_EQ(decoder->send(later[first_key]), SendResult::accepted);
    ASSERT_EQ(seen_.units.size(), before + 1);
    EXPECT_TRUE(has_sps(seen_.units.back()));
}

TEST_F(HardwareDecoderTest, ADecoderThatKeepsRefusingIsGivenUp)
{
    provide(/*refuse=*/true);
    const auto bytes = hui::testing::read_video_fixture("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer);
    const int index = video_stream(*demuxer);
    auto decoder = open_hardware_decoder(*demuxer, index, &error);
    ASSERT_TRUE(decoder) << error;
    Packet packet;
    SendResult last = SendResult::accepted;
    while (last != SendResult::error && demuxer->read(packet) == ReadResult::packet)
        if (packet.stream == index)
            last = decoder->send(packet);
    EXPECT_EQ(last, SendResult::error);
    EXPECT_GT(decoder->damaged_packets(), 0);
}

TEST_F(HardwareDecoderTest, TheEnginePrefersTheHardwareAndFallsBackToSoftware)
{
    const auto bytes = hui::testing::read_video_fixture("h264-aac.mp4");
    std::string error;
    provide();
    auto engine = PlaybackEngine::open(reader_for(bytes), bytes.size(), EngineOptions{}, &error);
    ASSERT_TRUE(engine) << error;
    EXPECT_EQ(engine->video_decoder_name(), "fake hardware");
    for (int i = 0; i < 20000 && engine->pump(); ++i)
    {
    }
    VideoPicture picture;
    EXPECT_TRUE(engine->take_picture(0.0, picture));
    EXPECT_EQ(picture.width, 160);

    provide(false, /*decline=*/true);
    auto software = PlaybackEngine::open(reader_for(bytes), bytes.size(), EngineOptions{}, &error);
    ASSERT_TRUE(software) << error;
    EXPECT_EQ(software->video_decoder_name(), "h264");
}

} // namespace
