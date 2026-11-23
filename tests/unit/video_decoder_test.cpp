// EliVier Explorer - Software video decoder tests: real pictures from every supported codec.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_fixtures.hpp"

#include "video/software_decoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

int video_stream_of(const Demuxer &demuxer)
{
    for (const StreamInfo &stream : demuxer.info().streams)
        if (stream.kind == StreamKind::video)
            return stream.index;
    return -1;
}

// A copy of a decoded picture that does not depend on the decoder's buffers.
struct Picture
{
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int chroma_shift_x = 0;
    int chroma_shift_y = 0;
    int plane_count = 0;
    bool semi_planar = false;
    ColourInfo colour;
    double pts = 0.0;
    std::vector<int> luma; // one value per pixel, in the picture's own bit depth
    int u_pitch = 0;
};

Picture copy_of(const Frame &frame)
{
    Picture picture;
    picture.width = frame.width;
    picture.height = frame.height;
    picture.bit_depth = frame.bit_depth;
    picture.chroma_shift_x = frame.chroma_shift_x;
    picture.chroma_shift_y = frame.chroma_shift_y;
    picture.plane_count = frame.plane_count;
    picture.semi_planar = frame.semi_planar;
    picture.colour = frame.colour;
    picture.pts = frame.pts;
    picture.u_pitch = frame.pitches[1];
    for (int y = 0; y < frame.height; ++y)
    {
        const std::uint8_t *row =
            frame.planes[0] + static_cast<std::ptrdiff_t>(y) * frame.pitches[0];
        for (int x = 0; x < frame.width; ++x)
        {
            if (frame.bit_depth > 8)
            {
                std::uint16_t word = 0;
                std::memcpy(&word, row + 2 * x, 2);
                picture.luma.push_back(word);
            }
            else
            {
                picture.luma.push_back(row[x]);
            }
        }
    }
    return picture;
}

struct Decoded
{
    std::vector<Picture> pictures;
    std::string decoder;
    int damaged = 0;
    std::string error;
};

// Reads the whole file and decodes the first video stream, the way the player will.
Decoded decode_file(const std::vector<std::uint8_t> &bytes, const SoftwareLimits &limits = {},
                    std::size_t max_pictures = 100000)
{
    Decoded result;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &result.error);
    if (!demuxer)
        return result;
    const int stream = video_stream_of(*demuxer);
    if (stream < 0)
    {
        result.error = "no video stream";
        return result;
    }
    auto decoder = open_software_decoder(*demuxer, stream, limits, &result.error);
    if (!decoder)
        return result;
    result.decoder = decoder->name();

    // Takes every finished picture out of the decoder.
    const auto drain = [&]() {
        Frame frame;
        ReceiveResult got = decoder->receive(frame);
        while (got == ReceiveResult::frame)
        {
            if (result.pictures.size() < max_pictures)
                result.pictures.push_back(copy_of(frame));
            got = decoder->receive(frame);
        }
        return got;
    };

    Packet packet;
    for (int guard = 0; guard < 100000; ++guard)
    {
        const ReadResult read = demuxer->read(packet);
        if (read != ReadResult::packet)
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
                result.error = "send failed";
                result.damaged = decoder->damaged_packets();
                return result;
            }
            drain(); // full: take the pictures out and offer the packet again
        }
        drain();
    }
    decoder->send_end();
    drain();
    result.damaged = decoder->damaged_packets();
    return result;
}

double mean_abs_difference(const Picture &a, const Picture &b, int shift_a, int shift_b)
{
    // Compare in 8 bits so that a 10-bit and an 8-bit clip can be set side by side.
    double total = 0.0;
    const std::size_t count = std::min(a.luma.size(), b.luma.size());
    for (std::size_t i = 0; i < count; ++i)
        total += std::fabs(static_cast<double>(a.luma[i] >> shift_a) -
                           static_cast<double>(b.luma[i] >> shift_b));
    return count == 0 ? 1e9 : total / static_cast<double>(count);
}

double mean(const Picture &picture)
{
    double total = 0.0;
    for (const int value : picture.luma)
        total += value;
    return total / static_cast<double>(picture.luma.size());
}

TEST(SoftwareDecoder, DecodesAllFramesOfAnH264Clip)
{
    const Decoded decoded = decode_file(read_all("h264-aac.mp4"));
    ASSERT_TRUE(decoded.error.empty()) << decoded.error;
    EXPECT_EQ(decoded.decoder, "h264");
    ASSERT_EQ(decoded.pictures.size(), 48u);

    const Picture &first = decoded.pictures.front();
    EXPECT_EQ(first.width, 160);
    EXPECT_EQ(first.height, 90);
    EXPECT_EQ(first.bit_depth, 8);
    EXPECT_EQ(first.plane_count, 3);
    EXPECT_FALSE(first.semi_planar);
    EXPECT_EQ(first.chroma_shift_x, 1);
    EXPECT_EQ(first.chroma_shift_y, 1);
    EXPECT_GE(first.u_pitch, 80);
    EXPECT_NEAR(first.pts, 0.0, 0.05);
    EXPECT_NEAR(decoded.pictures.back().pts, 2.0 - 1.0 / 24.0, 0.05);

    // Pictures come out in presentation order, a frame apart.
    for (std::size_t i = 1; i < decoded.pictures.size(); ++i)
        EXPECT_NEAR(decoded.pictures[i].pts - decoded.pictures[i - 1].pts, 1.0 / 24.0, 0.01) << i;
}

TEST(SoftwareDecoder, HevcMain10KeepsTenBitsAndTheHdrFlags)
{
    const Decoded decoded = decode_file(read_all("hevc-main10-hdr.mkv"));
    ASSERT_TRUE(decoded.error.empty()) << decoded.error;
    EXPECT_EQ(decoded.decoder, "hevc");
    ASSERT_EQ(decoded.pictures.size(), 48u);

    const Picture &picture = decoded.pictures[10];
    EXPECT_EQ(picture.bit_depth, 10);
    EXPECT_EQ(picture.colour.transfer, Transfer::pq);
    EXPECT_EQ(picture.colour.matrix, Matrix::bt2020);
    // Low-aligned in 16-bit words: nothing above bit 9, and more than 8 bits are really in use.
    const int largest = *std::max_element(picture.luma.begin(), picture.luma.end());
    EXPECT_LT(largest, 1024);
    EXPECT_GT(largest, 300);
}

TEST(SoftwareDecoder, DecodesVp9AndAv1WithDav1d)
{
    const Decoded vp9 = decode_file(read_all("vp9-opus.webm"));
    ASSERT_TRUE(vp9.error.empty()) << vp9.error;
    EXPECT_GE(vp9.pictures.size(), 47u);
    EXPECT_EQ(vp9.decoder, "vp9");

    const Decoded av1 = decode_file(read_all("av1.mkv"));
    ASSERT_TRUE(av1.error.empty()) << av1.error;
    EXPECT_EQ(av1.decoder, "libdav1d"); // FFmpeg's own AV1 decoder only wraps hardware
    EXPECT_GE(av1.pictures.size(), 47u);
}

TEST(SoftwareDecoder, FourFourFourKeepsFullResolutionChroma)
{
    const Decoded decoded = decode_file(read_all("h264-yuv444.mp4"));
    ASSERT_TRUE(decoded.error.empty()) << decoded.error;
    ASSERT_FALSE(decoded.pictures.empty());
    EXPECT_EQ(decoded.pictures[0].chroma_shift_x, 0);
    EXPECT_EQ(decoded.pictures[0].chroma_shift_y, 0);
    EXPECT_EQ(decoded.pictures[0].plane_count, 3);
    EXPECT_GE(decoded.pictures[0].u_pitch, 160);
}

TEST(SoftwareDecoder, EveryCodecShowsTheSamePictureAndTheSequenceMoves)
{
    // All clips were made from the same moving test pattern, so frame 10 of each must look alike
    // whatever the codec. A wrong plane, stride or bit shift would make them differ wildly.
    const Decoded h264 = decode_file(read_all("h264-aac.mp4"));
    const Decoded hevc = decode_file(read_all("hevc-main10-hdr.mkv"));
    const Decoded vp9 = decode_file(read_all("vp9-opus.webm"));
    const Decoded av1 = decode_file(read_all("av1.mkv"));
    ASSERT_GT(h264.pictures.size(), 10u);
    ASSERT_GT(hevc.pictures.size(), 10u);
    ASSERT_GT(vp9.pictures.size(), 10u);
    ASSERT_GT(av1.pictures.size(), 10u);

    const Picture &reference = h264.pictures[10];
    // Measured when this was written: VP9 6.7, AV1 6.4 and HEVC 6.2 (lossy coding at a very low
    // bit rate), while another frame of the same clip is 18.4 away. 10 sits between the two.
    EXPECT_LT(mean_abs_difference(reference, vp9.pictures[10], 0, 0), 10.0);
    EXPECT_LT(mean_abs_difference(reference, av1.pictures[10], 0, 0), 10.0);
    EXPECT_LT(mean_abs_difference(reference, hevc.pictures[10], 0, 2), 10.0);
    EXPECT_GT(mean_abs_difference(reference, h264.pictures[40], 0, 0), 10.0); // it does move
    EXPECT_GT(mean(reference), 20.0);
    EXPECT_LT(mean(reference), 235.0);
}

TEST(SoftwareDecoder, RefusesPicturesAboveTheLimit)
{
    SoftwareLimits limits;
    limits.max_pixels = 100;
    const Decoded decoded = decode_file(read_all("h264-aac.mp4"), limits);
    EXPECT_TRUE(decoded.pictures.empty());
    EXPECT_NE(decoded.error.find("too large"), std::string::npos) << decoded.error;
}

TEST(SoftwareDecoder, FlushAfterASeekDecodesFromTheNewPlace)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto demuxer = Demuxer::open(reader_for(bytes), bytes.size(), &error);
    ASSERT_TRUE(demuxer) << error;
    const int stream = video_stream_of(*demuxer);
    auto decoder = open_software_decoder(*demuxer, stream, SoftwareLimits{}, &error);
    ASSERT_TRUE(decoder) << error;

    ASSERT_TRUE(demuxer->seek(1.0));
    decoder->flush();
    Packet packet;
    Frame frame;
    double first_pts = -1.0;
    for (int guard = 0; guard < 1000 && first_pts < 0.0; ++guard)
    {
        ASSERT_EQ(demuxer->read(packet), ReadResult::packet);
        if (packet.stream != stream)
            continue;
        ASSERT_EQ(decoder->send(packet), SendResult::accepted);
        if (decoder->receive(frame) == ReceiveResult::frame)
            first_pts = frame.pts;
    }
    EXPECT_NEAR(first_pts, 1.0, 0.05);
}

TEST(SoftwareDecoder, SurvivesDamagedStreams)
{
    // Flip bytes inside the clips (most of the file is picture data) and decode what is left.
    // Pictures may be wrong or missing; nothing may crash, hang or leak (AddressSanitizer).
    const char *names[] = {"h264-aac.mp4", "hevc-main10-hdr.mkv", "vp9-opus.webm", "av1.mkv",
                           "h264-yuv444.mp4"};
    std::uint32_t state = 777;
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
            const std::size_t first_data = original.size() / 4; // leave the headers mostly alone
            for (int flips = 0; flips < 4 + round; ++flips)
                damaged[first_data + next() % (damaged.size() - first_data)] ^=
                    static_cast<std::uint8_t>(1u << (next() % 8));
            (void)decode_file(damaged, SoftwareLimits{}, 200);
        }
    }
    SUCCEED();
}

} // namespace
