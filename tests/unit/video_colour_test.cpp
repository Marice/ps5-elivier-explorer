// EliVier Explorer - Colour maths for video: YCbCr matrices, PQ, HLG and the BT.2390 tone map.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/colour.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace
{

using namespace hui::video;

constexpr float kTolerance = 1e-4f;

YuvFormat format(Matrix matrix, Range range, int bit_depth = 8)
{
    YuvFormat result;
    result.matrix = matrix;
    result.range = range;
    result.bit_depth = bit_depth;
    return result;
}

// A code value as the shader sees it: the integer sample divided by the largest code.
float code(int value, int bit_depth)
{
    return static_cast<float>(value) / static_cast<float>((1 << bit_depth) - 1);
}

TEST(VideoColour, LimitedRangeBlackAndWhiteMapToZeroAndOne)
{
    const YuvFormat bt709 = format(Matrix::bt709, Range::limited);
    const Rgb black = yuv_to_rgb({code(16, 8), code(128, 8), code(128, 8)}, bt709);
    const Rgb white = yuv_to_rgb({code(235, 8), code(128, 8), code(128, 8)}, bt709);
    EXPECT_NEAR(black.r, 0.0f, kTolerance);
    EXPECT_NEAR(black.g, 0.0f, kTolerance);
    EXPECT_NEAR(black.b, 0.0f, kTolerance);
    EXPECT_NEAR(white.r, 1.0f, kTolerance);
    EXPECT_NEAR(white.g, 1.0f, kTolerance);
    EXPECT_NEAR(white.b, 1.0f, kTolerance);
}

TEST(VideoColour, TenBitLimitedRangeUsesTheScaledOffsets)
{
    const YuvFormat bt2020 = format(Matrix::bt2020, Range::limited, 10);
    const Rgb black = yuv_to_rgb({code(64, 10), code(512, 10), code(512, 10)}, bt2020);
    const Rgb white = yuv_to_rgb({code(940, 10), code(512, 10), code(512, 10)}, bt2020);
    EXPECT_NEAR(black.g, 0.0f, kTolerance);
    EXPECT_NEAR(white.g, 1.0f, kTolerance);
}

TEST(VideoColour, FullRangeFollowsTheMatrixCoefficients)
{
    // BT.709: Kr 0.2126, Kb 0.0722, Kg 0.7152. 8-bit full range: y 128, Cb 128 (neutral), Cr 204.
    // Cr' = (204 - 128) / 255; R = y + 2(1-Kr) Cr'; G = y - 2Kr(1-Kr)/Kg Cr'; B = y.
    const Rgb pixel = yuv_to_rgb({code(128, 8), code(128, 8), code(204, 8)},
                                 format(Matrix::bt709, Range::full));
    EXPECT_NEAR(pixel.r, 0.97131f, kTolerance);
    EXPECT_NEAR(pixel.g, 0.36244f, kTolerance);
    EXPECT_NEAR(pixel.b, 0.50196f, kTolerance);
}

TEST(VideoColour, MatricesDisagreeOnSaturatedColour)
{
    const YuvSample sample{0.5f, 0.4f, 0.8f};
    const Rgb bt601 = yuv_to_rgb(sample, format(Matrix::bt601, Range::full));
    const Rgb bt709 = yuv_to_rgb(sample, format(Matrix::bt709, Range::full));
    const Rgb bt2020 = yuv_to_rgb(sample, format(Matrix::bt2020, Range::full));
    EXPECT_GT(std::fabs(bt601.r - bt709.r), 0.01f);
    EXPECT_GT(std::fabs(bt709.r - bt2020.r), 0.01f);
}

TEST(VideoColour, OutOfGamutResultIsClamped)
{
    const Rgb pixel = yuv_to_rgb({1.0f, 0.0f, 1.0f}, format(Matrix::bt709, Range::full));
    EXPECT_GE(pixel.r, 0.0f);
    EXPECT_LE(pixel.r, 1.0f);
    EXPECT_GE(pixel.b, 0.0f);
    EXPECT_LE(pixel.b, 1.0f);
}

TEST(VideoColour, PqEndsAndKnownPoint)
{
    EXPECT_NEAR(pq_to_linear(0.0f), 0.0f, kTolerance);
    EXPECT_NEAR(pq_to_linear(1.0f), 1.0f, kTolerance);
    // 100 nits is signal 0.5081 (ST 2084), linear 100 / 10000.
    EXPECT_NEAR(linear_to_pq(100.0f / 10000.0f), 0.5081f, 0.001f);
}

TEST(VideoColour, PqRoundTrips)
{
    for (int step = 0; step <= 100; ++step)
    {
        const float signal = static_cast<float>(step) / 100.0f;
        EXPECT_NEAR(linear_to_pq(pq_to_linear(signal)), signal, 1e-3f) << signal;
    }
}

TEST(VideoColour, HlgInverseOetfKnownPoints)
{
    EXPECT_NEAR(hlg_to_linear(0.0f), 0.0f, kTolerance);
    EXPECT_NEAR(hlg_to_linear(0.5f), 1.0f / 12.0f, kTolerance); // the joint of the two curve pieces
    EXPECT_NEAR(hlg_to_linear(1.0f), 1.0f, kTolerance);
}

TEST(VideoColour, ToneMapLeavesEqualPeaksAlone)
{
    for (int step = 0; step <= 20; ++step)
    {
        const float signal = static_cast<float>(step) / 20.0f;
        EXPECT_NEAR(tone_map_bt2390(signal, 1000.0f, 1000.0f), signal, kTolerance) << signal;
    }
}

TEST(VideoColour, ToneMapBelowTheKneeIsUntouchedAndThePeakLandsOnTheTarget)
{
    const float source_peak = 1000.0f;
    const float target_peak = 100.0f;
    const float source_signal = linear_to_pq(source_peak / 10000.0f);
    const float target_signal = linear_to_pq(target_peak / 10000.0f);

    const float dark = linear_to_pq(2.0f / 10000.0f); // 2 nits
    EXPECT_NEAR(tone_map_bt2390(dark, source_peak, target_peak), dark, 1e-3f);
    EXPECT_NEAR(tone_map_bt2390(source_signal, source_peak, target_peak), target_signal, 1e-3f);
}

TEST(VideoColour, ToneMapIsMonotonicAndNeverBrighterThanTheTarget)
{
    const float source_signal = linear_to_pq(4000.0f / 10000.0f);
    const float target_signal = linear_to_pq(203.0f / 10000.0f);
    float previous = 0.0f;
    for (int step = 0; step <= 200; ++step)
    {
        const float signal = source_signal * static_cast<float>(step) / 200.0f;
        const float mapped = tone_map_bt2390(signal, 4000.0f, 203.0f);
        EXPECT_GE(mapped, previous - 1e-6f) << signal;
        EXPECT_LE(mapped, target_signal + 1e-4f) << signal;
        previous = mapped;
    }
}

} // namespace

namespace
{

using namespace hui::video;

TEST(VideoColourHdr, SdrWhiteBecomesTheHdr10CodeOf203Nits)
{
    const Rgb white = sdr_to_hdr10({1.0f, 1.0f, 1.0f});
    const float expected = linear_to_pq(203.0f / 10000.0f); // about 0.58
    EXPECT_NEAR(white.r, expected, 1e-4f);
    EXPECT_NEAR(white.g, expected, 1e-4f);
    EXPECT_NEAR(white.b, expected, 1e-4f);
    const Rgb black = sdr_to_hdr10({0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(black.r, linear_to_pq(0.0f), 1e-6f);
}

TEST(VideoColourHdr, Bt709RedLandsInsideBt2020)
{
    // Pure BT.709 red is not pure BT.2020 red: some green and blue come in (0.627, 0.069, 0.016
    // of the light), and red stays the largest.
    const Rgb red = sdr_to_hdr10({1.0f, 0.0f, 0.0f});
    EXPECT_GT(red.r, red.g);
    EXPECT_GT(red.g, red.b);
    EXPECT_GT(red.g, 0.0f);
    EXPECT_NEAR(pq_to_linear(red.r) * 10000.0f, 0.6274f * 203.0f, 1.0f);
}

TEST(VideoColourHdr, PackingPutsRedHighAndBlueLowInBgraBytes)
{
    // What the console showed right (D71): word = 3 << 30 | R << 20 | G << 10 | B, written as the
    // GL's (red, green, blue, alpha) = (byte 2, byte 1, byte 0, byte 3).
    const Hdr10Bytes bytes = pack_hdr10(1023, 0, 0);
    const std::uint32_t word = (3u << 30) | (1023u << 20);
    EXPECT_EQ(bytes.gl_red, (word >> 16) & 0xFF);
    EXPECT_EQ(bytes.gl_green, (word >> 8) & 0xFF);
    EXPECT_EQ(bytes.gl_blue, word & 0xFF);
    EXPECT_EQ(bytes.gl_alpha, word >> 24);
    const Hdr10Bytes blue = pack_hdr10(0, 0, 1023);
    EXPECT_EQ(blue.gl_blue, 0xFF);
    EXPECT_EQ(blue.gl_green, 0x03);
    EXPECT_EQ(blue.gl_red, 0x00);
    EXPECT_EQ(blue.gl_alpha, 0xC0);
}

} // namespace
