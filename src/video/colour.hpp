// EliVier Explorer - Colour maths for video: YCbCr matrices, PQ, HLG and the BT.2390 tone map.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The video shader (src/gfx) does the same arithmetic per pixel. It is written here first, in
// plain C++, so that the numbers can be tested against published reference values.

#pragma once

namespace hui::video
{

enum class Matrix
{
    bt601,
    bt709,
    bt2020,
};

enum class Range
{
    limited, // 16..235 luma, 16..240 chroma, scaled to the bit depth
    full,
};

// The transfer function a stream is encoded with: what the numbers mean as light.
enum class Transfer
{
    sdr, // BT.709 or sRGB style gamma
    pq,  // HDR10, Dolby Vision's base layer
    hlg,
};

struct YuvFormat
{
    Matrix matrix = Matrix::bt709;
    Range range = Range::limited;
    int bit_depth = 8; // 8 to 16
};

// What a file says about the colour of its pictures. Files that say nothing get the SDR defaults
// of their size (the demuxer fills this in; see src/video/demuxer.cpp).
struct ColourInfo
{
    Matrix matrix = Matrix::bt709;
    Range range = Range::limited;
    Transfer transfer = Transfer::sdr;
};

// A sample as the shader reads it from a texture: the integer code value divided by the largest
// code (255 for 8 bits, 1023 for 10 bits).
struct YuvSample
{
    float y;
    float u;
    float v;
};

struct Rgb
{
    float r;
    float g;
    float b;
};

// Non-linear R'G'B' in 0..1, clamped. The transfer function is not touched: the result keeps the
// curve of the source (gamma, PQ or HLG).
Rgb yuv_to_rgb(const YuvSample &sample, const YuvFormat &format);

// SMPTE ST 2084. Signal 0..1 to linear light relative to 10,000 nits, and back.
float pq_to_linear(float signal);
float linear_to_pq(float linear);

// ARIB STD-B67 inverse OETF: signal 0..1 to scene-linear light 0..1.
float hlg_to_linear(float signal);

// ITU-R BT.2390 EETF on a PQ signal: roll the highlights of a source mastered up to
// source_peak_nits off to what a display of target_peak_nits can show, and leave everything below
// the knee alone. A target at or above the source peak changes nothing. The black level is taken
// as 0.
float tone_map_bt2390(float pq_signal, float source_peak_nits, float target_peak_nits);

} // namespace hui::video
