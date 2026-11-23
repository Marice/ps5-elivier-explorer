// EliVier Explorer - Picture conversion: planar YCbCr to RGBA8, checked against the colour maths.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/convert.hpp"

#include "video/colour.hpp"
#include "video/convert_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

using namespace hui::video;

// A frame whose planes live in the test: one value for every sample of each plane, or a gradient.
struct Picture
{
    int width;
    int height;
    int bit_depth;
    int shift_x;
    int shift_y;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
    int pitch_y;
    int pitch_c;

    Frame frame(ColourInfo colour) const
    {
        Frame f;
        f.planes[0] = y.data();
        f.planes[1] = u.data();
        f.planes[2] = v.data();
        f.pitches[0] = pitch_y;
        f.pitches[1] = pitch_c;
        f.pitches[2] = pitch_c;
        f.plane_count = 3;
        f.width = width;
        f.height = height;
        f.bit_depth = bit_depth;
        f.chroma_shift_x = shift_x;
        f.chroma_shift_y = shift_y;
        f.colour = colour;
        return f;
    }
};

void put(std::vector<std::uint8_t> &plane, int pitch, int bit_depth, int x, int y, int value)
{
    if (bit_depth > 8)
    {
        const auto word = static_cast<std::uint16_t>(value);
        std::memcpy(&plane[static_cast<std::size_t>(y) * pitch + 2 * x], &word, 2);
    }
    else
    {
        plane[static_cast<std::size_t>(y) * pitch + x] = static_cast<std::uint8_t>(value);
    }
}

// A picture of width x height with one Y, Cb, Cr code for all of it (limited range black is 16).
Picture flat(int width, int height, int bit_depth, int shift_x, int shift_y, int y, int cb, int cr,
             int extra_pitch = 0)
{
    Picture p{width, height, bit_depth, shift_x, shift_y, {}, {}, {}, 0, 0};
    const int bytes = bit_depth > 8 ? 2 : 1;
    const int cw = (width + (1 << shift_x) - 1) >> shift_x;
    const int ch = (height + (1 << shift_y) - 1) >> shift_y;
    p.pitch_y = width * bytes + extra_pitch;
    p.pitch_c = cw * bytes + extra_pitch;
    p.y.assign(static_cast<std::size_t>(p.pitch_y) * height, 0xEE); // padding is garbage
    p.u.assign(static_cast<std::size_t>(p.pitch_c) * ch, 0xEE);
    p.v.assign(static_cast<std::size_t>(p.pitch_c) * ch, 0xEE);
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            put(p.y, p.pitch_y, bit_depth, col, row, y);
    for (int row = 0; row < ch; ++row)
        for (int col = 0; col < cw; ++col)
        {
            put(p.u, p.pitch_c, bit_depth, col, row, cb);
            put(p.v, p.pitch_c, bit_depth, col, row, cr);
        }
    return p;
}

struct Pixel
{
    int r, g, b, a;
};

std::vector<std::uint8_t> convert(const Picture &p, ColourInfo colour,
                                  ConvertTables *tables = nullptr)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(p.width) * p.height * 4, 0x77);
    ConvertTables local;
    EXPECT_TRUE(convert_to_rgba(p.frame(colour), tables != nullptr ? *tables : local, out.data(),
                                p.width * 4));
    return out;
}

Pixel at(const std::vector<std::uint8_t> &rgba, int width, int x, int y)
{
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

ColourInfo sdr(Matrix matrix, Range range)
{
    ColourInfo c;
    c.matrix = matrix;
    c.range = range;
    c.transfer = Transfer::sdr;
    return c;
}

TEST(VideoConvert, LimitedRangeBlackWhiteAndGrey)
{
    const ColourInfo bt709 = sdr(Matrix::bt709, Range::limited);
    const Picture black = flat(4, 2, 8, 1, 1, 16, 128, 128);
    const Picture white = flat(4, 2, 8, 1, 1, 235, 128, 128);
    const Picture grey = flat(4, 2, 8, 1, 1, 126, 128, 128);
    const Pixel b = at(convert(black, bt709), 4, 1, 1);
    const Pixel w = at(convert(white, bt709), 4, 2, 0);
    const Pixel g = at(convert(grey, bt709), 4, 0, 0);
    EXPECT_EQ(b.r + b.g + b.b, 0);
    EXPECT_EQ(w.r, 255);
    EXPECT_EQ(w.g, 255);
    EXPECT_EQ(w.b, 255);
    EXPECT_NEAR(g.r, 128, 1);
    EXPECT_EQ(g.r, g.g);
    EXPECT_EQ(g.g, g.b);
    EXPECT_EQ(b.a, 255);
    EXPECT_EQ(w.a, 255);
}

TEST(VideoConvert, TheKnownBt709LimitedRangeRed)
{
    // Y 63, Cb 102, Cr 240 is the usual code for full red in BT.709 limited range.
    const Picture red = flat(2, 2, 8, 1, 1, 63, 102, 240);
    const Pixel p = at(convert(red, sdr(Matrix::bt709, Range::limited)), 2, 0, 0);
    EXPECT_NEAR(p.r, 255, 3);
    EXPECT_NEAR(p.g, 0, 3);
    EXPECT_NEAR(p.b, 0, 3);
}

TEST(VideoConvert, AgreesWithTheColourMathsWithinOneCode)
{
    // The integer path is checked against yuv_to_rgb, which is tested against published values.
    const Matrix matrices[] = {Matrix::bt601, Matrix::bt709, Matrix::bt2020};
    const Range ranges[] = {Range::limited, Range::full};
    const int depths[] = {8, 10, 12};
    for (const Matrix matrix : matrices)
        for (const Range range : ranges)
            for (const int depth : depths)
            {
                const int largest = (1 << depth) - 1;
                int worst = 0;
                for (int yi = 0; yi <= 8; ++yi)
                    for (int ci = 0; ci <= 8; ++ci)
                        for (int ri = 0; ri <= 8; ++ri)
                        {
                            const int y = yi * largest / 8;
                            const int cb = ci * largest / 8;
                            const int cr = ri * largest / 8;
                            const Picture p = flat(2, 2, depth, 1, 1, y, cb, cr);
                            const Pixel got = at(convert(p, sdr(matrix, range)), 2, 0, 0);
                            YuvFormat format;
                            format.matrix = matrix;
                            format.range = range;
                            format.bit_depth = depth;
                            const Rgb want =
                                yuv_to_rgb({static_cast<float>(y) / largest,
                                            static_cast<float>(cb) / largest,
                                            static_cast<float>(cr) / largest},
                                           format);
                            const auto code8 = [](float v) {
                                return static_cast<int>(std::lround(v * 255.0f));
                            };
                            worst = std::max({worst, std::abs(got.r - code8(want.r)),
                                              std::abs(got.g - code8(want.g)),
                                              std::abs(got.b - code8(want.b))});
                        }
                EXPECT_LE(worst, 1) << "matrix " << static_cast<int>(matrix) << " range "
                                    << static_cast<int>(range) << " depth " << depth;
            }
}

TEST(VideoConvert, ChromaOfAFourTwoZeroPictureIsSharedByAFourPixelBlock)
{
    Picture p = flat(4, 2, 8, 1, 1, 126, 128, 128);
    put(p.u, p.pitch_c, 8, 1, 0, 90); // the chroma sample of the right-hand 2 x 2 block
    put(p.v, p.pitch_c, 8, 1, 0, 200);
    const auto rgba = convert(p, sdr(Matrix::bt709, Range::limited));
    const Pixel left = at(rgba, 4, 1, 1);
    EXPECT_EQ(left.r, left.g); // grey on the left
    for (int y = 0; y < 2; ++y)
        for (int x = 2; x < 4; ++x)
        {
            const Pixel right = at(rgba, 4, x, y);
            EXPECT_GT(right.r, right.b + 20) << x << "," << y; // the same tint in all four
            EXPECT_EQ(right.r, at(rgba, 4, 2, 0).r);
        }
}

TEST(VideoConvert, FourFourFourGivesEveryPixelItsOwnChroma)
{
    Picture p = flat(3, 1, 8, 0, 0, 126, 128, 128);
    put(p.v, p.pitch_c, 8, 2, 0, 220);
    const auto rgba = convert(p, sdr(Matrix::bt709, Range::limited));
    EXPECT_EQ(at(rgba, 3, 0, 0).r, at(rgba, 3, 1, 0).r);
    EXPECT_GT(at(rgba, 3, 2, 0).r, at(rgba, 3, 1, 0).r + 30);
}

TEST(VideoConvert, TheRowPaddingOfThePlanesIsNeverRead)
{
    const Picture p = flat(5, 3, 8, 1, 1, 235, 128, 128, 13); // odd size, padded rows
    const auto rgba = convert(p, sdr(Matrix::bt709, Range::limited));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x)
        {
            const Pixel px = at(rgba, 5, x, y);
            EXPECT_EQ(px.r, 255) << x << "," << y;
            EXPECT_EQ(px.a, 255);
        }
}

TEST(VideoConvert, TenBitSamplesAreLowAlignedWords)
{
    // 10-bit limited white is 940, black 64.
    const ColourInfo c = sdr(Matrix::bt2020, Range::limited);
    const Pixel white = at(convert(flat(2, 2, 10, 1, 1, 940, 512, 512), c), 2, 1, 1);
    const Pixel black = at(convert(flat(2, 2, 10, 1, 1, 64, 512, 512), c), 2, 0, 0);
    EXPECT_EQ(white.r, 255);
    EXPECT_EQ(black.r, 0);
}

TEST(VideoConvert, OutputIsClampedForOutOfRangeCodes)
{
    const ColourInfo c = sdr(Matrix::bt709, Range::limited);
    const Pixel hot = at(convert(flat(2, 2, 8, 1, 1, 255, 0, 255), c), 2, 0, 0);
    const Pixel cold = at(convert(flat(2, 2, 8, 1, 1, 0, 255, 0), c), 2, 0, 0);
    for (const int v : {hot.r, hot.g, hot.b, cold.r, cold.g, cold.b})
    {
        EXPECT_GE(v, 0);
        EXPECT_LE(v, 255);
    }
}

TEST(VideoConvert, HdrInPqIsToneMappedToSdr)
{
    ColourInfo hdr;
    hdr.matrix = Matrix::bt2020;
    hdr.range = Range::limited;
    hdr.transfer = Transfer::pq;
    // PQ signal for 203 nits (reference white), 1000 nits and 0 nits, as 10-bit limited-range luma.
    const auto code = [](float nits) {
        return 64 + static_cast<int>(std::lround(linear_to_pq(nits / 10000.0f) * 876.0f));
    };
    ConvertTables tables;
    const auto shown = [&](float nits) {
        return at(convert(flat(2, 2, 10, 1, 1, code(nits), 512, 512), hdr, &tables), 2, 0, 0);
    };
    const Pixel dark = shown(0.0f);
    const Pixel paper = shown(203.0f);
    const Pixel bright = shown(1000.0f);
    EXPECT_LE(dark.r, 2);
    EXPECT_GT(paper.r, 200); // reference white shows as (nearly) white on an SDR screen
    EXPECT_GE(bright.r, paper.r);
    EXPECT_LE(bright.r, 255);
    EXPECT_EQ(paper.r, paper.g); // grey stays grey
    EXPECT_EQ(paper.g, paper.b);
}

TEST(VideoConvert, TheHdrCurveNeverGetsDarkerWithMoreLight)
{
    ColourInfo hdr;
    hdr.matrix = Matrix::bt2020;
    hdr.range = Range::limited;
    hdr.transfer = Transfer::pq;
    ConvertTables tables;
    int previous = -1;
    for (int y = 64; y <= 940; y += 4)
    {
        const Pixel p = at(convert(flat(2, 2, 10, 1, 1, y, 512, 512), hdr, &tables), 2, 0, 0);
        EXPECT_GE(p.r, previous) << y;
        previous = p.r;
    }
}

TEST(VideoConvert, HlgIsShownAsItIs)
{
    ColourInfo hlg;
    hlg.matrix = Matrix::bt2020;
    hlg.range = Range::limited;
    hlg.transfer = Transfer::hlg;
    ColourInfo plain = hlg;
    plain.transfer = Transfer::sdr;
    const Picture p = flat(2, 2, 10, 1, 1, 600, 480, 540);
    EXPECT_EQ(convert(p, hlg), convert(p, plain));
}

TEST(VideoConvert, RefusesWhatItCannotRead)
{
    std::vector<std::uint8_t> out(64, 0);
    ConvertTables tables;
    Frame empty;
    EXPECT_FALSE(convert_to_rgba(empty, tables, out.data(), 16));

    Picture p = flat(4, 2, 8, 1, 1, 126, 128, 128);
    Frame f = p.frame(sdr(Matrix::bt709, Range::limited));
    f.semi_planar = true; // NV12 comes with the hardware decoder
    EXPECT_FALSE(convert_to_rgba(f, tables, out.data(), 16));
    f = p.frame(sdr(Matrix::bt709, Range::limited));
    EXPECT_FALSE(convert_to_rgba(f, tables, out.data(), 4 * 4 - 1)); // pitch too small
    f.bit_depth = 7;
    EXPECT_FALSE(convert_to_rgba(f, tables, out.data(), 16));
    f = p.frame(sdr(Matrix::bt709, Range::limited));
    f.chroma_shift_x = 5;
    EXPECT_FALSE(convert_to_rgba(f, tables, out.data(), 16));
}

} // namespace

namespace
{

TEST(VideoConvert, SixteenBitOutputIsTheEightBitOutputTimesTwoFiftySeven)
{
    for (const int depth : {8, 10})
        for (const Transfer transfer : {Transfer::sdr, Transfer::pq})
        {
            Picture p = flat(6, 4, depth, 1, 1, 90, 100, 170);
            for (int x = 0; x < 6; ++x)
                put(p.y, p.pitch_y, depth, x, 1, depth == 8 ? 30 + 30 * x : 120 + 120 * x);
            ColourInfo colour = sdr(Matrix::bt709, Range::limited);
            colour.transfer = transfer;
            ConvertTables tables8;
            ConvertTables tables16;
            std::vector<std::uint8_t> narrow(6 * 4 * 4);
            std::vector<std::uint16_t> wide(6 * 4 * 4, 0x7777);
            ASSERT_TRUE(convert_to_rgba(p.frame(colour), tables8, narrow.data(), 6 * 4));
            ASSERT_TRUE(convert_to_rgba16(p.frame(colour), tables16, wide.data(), 6 * 8));
            for (std::size_t i = 0; i < narrow.size(); ++i)
                EXPECT_EQ(wide[i], narrow[i] * 257) << "depth " << depth << " sample " << i;
        }
}

TEST(VideoConvert, SixteenBitOutputHonoursItsPitchAndRefusesAShortOne)
{
    const Picture p = flat(4, 2, 8, 1, 1, 126, 128, 128);
    const ColourInfo colour = sdr(Matrix::bt709, Range::limited);
    ConvertTables tables;
    std::vector<std::uint16_t> wide(2 * 20, 0x1234); // 4 pixels of 4 samples, 40 bytes a row
    ASSERT_TRUE(convert_to_rgba16(p.frame(colour), tables, wide.data(), 40));
    EXPECT_EQ(wide[15], 65535);       // alpha of the last pixel of row 0
    EXPECT_EQ(wide[16], 0x1234);      // padding is not touched
    EXPECT_EQ(wide[20 + 3], 65535);   // alpha of the first pixel of row 1
    EXPECT_FALSE(convert_to_rgba16(p.frame(colour), tables, wide.data(), 31));
    EXPECT_FALSE(convert_to_rgba16(p.frame(colour), tables, nullptr, 40));
}

} // namespace

namespace
{

// The same picture as NV12 (or its 16-bit form): Cb and Cr interleaved in one plane.
Frame semi_planar_of(const Picture &p, std::vector<std::uint8_t> &uv, ColourInfo colour)
{
    const int bytes = p.bit_depth > 8 ? 2 : 1;
    const int cw = (p.width + (1 << p.shift_x) - 1) >> p.shift_x;
    const int ch = (p.height + (1 << p.shift_y) - 1) >> p.shift_y;
    const int pitch = cw * 2 * bytes + 6; // padded, as a decoder's pitch is
    uv.assign(static_cast<std::size_t>(pitch) * ch, 0xEE);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
        {
            std::memcpy(&uv[static_cast<std::size_t>(y) * pitch + (2 * x) * bytes],
                        &p.u[static_cast<std::size_t>(y) * p.pitch_c + x * bytes], bytes);
            std::memcpy(&uv[static_cast<std::size_t>(y) * pitch + (2 * x + 1) * bytes],
                        &p.v[static_cast<std::size_t>(y) * p.pitch_c + x * bytes], bytes);
        }
    Frame f = p.frame(colour);
    f.planes[1] = uv.data();
    f.planes[2] = nullptr;
    f.pitches[1] = pitch;
    f.pitches[2] = 0;
    f.plane_count = 2;
    f.semi_planar = true;
    return f;
}

Picture varied(int width, int height, int depth)
{
    Picture p = flat(width, height, depth, 1, 1, 0, 0, 0);
    const int top = depth > 8 ? 1023 : 255;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            put(p.y, p.pitch_y, depth, x, y, (x * 37 + y * 11) % (top + 1));
    for (int y = 0; y < (height + 1) / 2; ++y)
        for (int x = 0; x < (width + 1) / 2; ++x)
        {
            put(p.u, p.pitch_c, depth, x, y, (x * 13 + y * 29) % (top + 1));
            put(p.v, p.pitch_c, depth, x, y, (x * 53 + y * 7) % (top + 1));
        }
    return p;
}

TEST(VideoConvert, ASemiPlanarFrameGivesTheSamePixelsAsItsPlanarTwin)
{
    for (const int depth : {8, 10})
    {
        const Picture p = varied(10, 6, depth);
        const ColourInfo colour = sdr(Matrix::bt709, Range::limited);
        std::vector<std::uint8_t> uv;
        const Frame nv12 = semi_planar_of(p, uv, colour);
        ConvertTables tables;
        std::vector<std::uint16_t> planar(10 * 6 * 4);
        std::vector<std::uint16_t> semi(10 * 6 * 4);
        ASSERT_TRUE(convert_to_rgba16(p.frame(colour), tables, planar.data(), 10 * 8));
        ASSERT_TRUE(convert_to_rgba16(nv12, tables, semi.data(), 10 * 8)) << "depth " << depth;
        EXPECT_EQ(planar, semi) << "depth " << depth;
    }
}

TEST(VideoConvert, ASemiPlanarFrameWithAShortChromaPitchIsRefused)
{
    const Picture p = varied(10, 6, 8);
    const ColourInfo colour = sdr(Matrix::bt709, Range::limited);
    std::vector<std::uint8_t> uv;
    Frame nv12 = semi_planar_of(p, uv, colour);
    nv12.pitches[1] = 9; // five chroma pairs need ten bytes
    ConvertTables tables;
    std::vector<std::uint16_t> out(10 * 6 * 4);
    EXPECT_FALSE(convert_to_rgba16(nv12, tables, out.data(), 10 * 8));
}

TEST(VideoConvert, EightBitValuesInSixteenBitWordsReadAsEightBit)
{
    // How the hardware decoder may write an 8-bit picture: each value in the low byte of a word.
    const Picture narrow = varied(8, 4, 8);
    Picture wide = flat(8, 4, 10, 1, 1, 0, 0, 0); // 16-bit planes of the same size
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
            put(wide.y, wide.pitch_y, 10, x, y, narrow.y[static_cast<std::size_t>(y) * narrow.pitch_y + x]);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
        {
            put(wide.u, wide.pitch_c, 10, x, y, narrow.u[static_cast<std::size_t>(y) * narrow.pitch_c + x]);
            put(wide.v, wide.pitch_c, 10, x, y, narrow.v[static_cast<std::size_t>(y) * narrow.pitch_c + x]);
        }
    const ColourInfo colour = sdr(Matrix::bt709, Range::limited);
    Frame words = wide.frame(colour);
    words.bit_depth = 8;
    words.sample_bytes = 2;
    ConvertTables tables;
    std::vector<std::uint16_t> expected(8 * 4 * 4);
    std::vector<std::uint16_t> got(8 * 4 * 4);
    ASSERT_TRUE(convert_to_rgba16(narrow.frame(colour), tables, expected.data(), 8 * 8));
    ASSERT_TRUE(convert_to_rgba16(words, tables, got.data(), 8 * 8));
    EXPECT_EQ(expected, got);
    words.sample_bytes = 3;
    EXPECT_FALSE(convert_to_rgba16(words, tables, got.data(), 8 * 8));
}

TEST(VideoConvert, RowBandsPutTogetherAreThePicture)
{
    const Picture p = varied(12, 9, 10);
    ColourInfo colour = sdr(Matrix::bt2020, Range::limited);
    colour.transfer = Transfer::pq;
    ConvertTables whole_tables;
    std::vector<std::uint16_t> whole(12 * 9 * 4);
    ASSERT_TRUE(convert_to_rgba16(p.frame(colour), whole_tables, whole.data(), 12 * 8));

    ConvertTables tables;
    ASSERT_TRUE(prepare_conversion(p.frame(colour), tables, 12 * 8, sizeof(std::uint16_t)));
    std::vector<std::uint16_t> bands(12 * 9 * 4, 0x5555);
    convert_rows_rgba16(p.frame(colour), tables, bands.data(), 12 * 8, 0, 4);
    convert_rows_rgba16(p.frame(colour), tables, bands.data(), 12 * 8, 4, 5);
    convert_rows_rgba16(p.frame(colour), tables, bands.data(), 12 * 8, 5, 9);
    EXPECT_EQ(whole, bands);
}

TEST(VideoConvert, APoolOfThreadsGivesTheSamePictureAsOneThread)
{
    const Picture p = varied(64, 37, 8);
    const ColourInfo colour = sdr(Matrix::bt709, Range::limited);
    ConvertTables tables;
    std::vector<std::uint16_t> alone(64 * 37 * 4);
    ASSERT_TRUE(convert_to_rgba16(p.frame(colour), tables, alone.data(), 64 * 8));
    for (const int threads : {1, 2, 4})
    {
        ConvertPool pool(threads);
        ConvertTables pool_tables;
        for (int round = 0; round < 3; ++round) // the workers are reused
        {
            std::vector<std::uint16_t> shared(64 * 37 * 4, 0x1111);
            ASSERT_TRUE(pool.convert_rgba16(p.frame(colour), pool_tables, shared.data(), 64 * 8));
            EXPECT_EQ(alone, shared) << threads << " threads, round " << round;
        }
    }
    ConvertPool pool(3);
    ConvertTables pool_tables;
    std::vector<std::uint16_t> out(64 * 37 * 4);
    EXPECT_FALSE(pool.convert_rgba16(p.frame(colour), pool_tables, out.data(), 10)); // short pitch
}

} // namespace

namespace
{

TEST(VideoConvert, WithHdrOutputAPqPictureKeepsItsSignal)
{
    // 10-bit limited range BT.2020 greys: black, a middle code and white come out as the PQ codes
    // they are (0, about half, 1023), not tone-mapped down to an SDR screen.
    ColourInfo pq;
    pq.matrix = Matrix::bt2020;
    pq.range = Range::limited;
    pq.transfer = Transfer::pq;
    const int ys[3] = {64, 502, 940};
    const int expected[3] = {0, 32767, 65535};
    for (int i = 0; i < 3; ++i)
    {
        const Picture p = flat(4, 2, 10, 1, 1, ys[i], 512, 512);
        ConvertTables tables;
        tables.pq_passthrough = true;
        std::vector<std::uint16_t> out(4 * 2 * 4);
        ASSERT_TRUE(convert_to_rgba16(p.frame(pq), tables, out.data(), 4 * 8));
        EXPECT_NEAR(out[0], expected[i], 80) << "Y " << ys[i];
        EXPECT_EQ(out[0], out[1]);
        EXPECT_EQ(out[3], 65535);
        ConvertTables mapped; // without HDR output the same white is tone-mapped below full
        std::vector<std::uint16_t> sdr(4 * 2 * 4);
        ASSERT_TRUE(convert_to_rgba16(p.frame(pq), mapped, sdr.data(), 4 * 8));
        if (i == 1)
            EXPECT_NE(sdr[0], out[0]);
    }
}

} // namespace
