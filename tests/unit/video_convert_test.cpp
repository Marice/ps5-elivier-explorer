// EliVier Explorer - Picture conversion: planar YCbCr to RGBA8, checked against the colour maths.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/convert.hpp"

#include "video/colour.hpp"

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
