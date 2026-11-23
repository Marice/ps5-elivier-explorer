// EliVier Explorer - Colour maths for video: YCbCr matrices, PQ, HLG and the BT.2390 tone map.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/colour.hpp"

#include <algorithm>
#include <cmath>

namespace hui::video
{

namespace
{

float clamp01(float value)
{
    return std::min(std::max(value, 0.0f), 1.0f);
}

struct Coefficients
{
    float kr;
    float kb;
};

Coefficients coefficients(Matrix matrix)
{
    switch (matrix)
    {
    case Matrix::bt601:
        return {0.299f, 0.114f};
    case Matrix::bt2020:
        return {0.2627f, 0.0593f};
    case Matrix::bt709:
        break;
    }
    return {0.2126f, 0.0722f};
}

} // namespace

Rgb yuv_to_rgb(const YuvSample &sample, const YuvFormat &format)
{
    const int depth = std::min(std::max(format.bit_depth, 8), 16);
    const float largest = static_cast<float>((1 << depth) - 1);
    const float half = static_cast<float>(1 << (depth - 1)) / largest;

    float y = sample.y;
    float cb = sample.u - half;
    float cr = sample.v - half;
    if (format.range == Range::limited)
    {
        // The offsets and spans are the 8-bit ones (16, 235, 128, 240) shifted to the bit depth.
        const float scale = static_cast<float>(1 << (depth - 8)) / largest;
        y = (y - 16.0f * scale) / (219.0f * scale);
        cb /= 224.0f * scale;
        cr /= 224.0f * scale;
    }

    const Coefficients k = coefficients(format.matrix);
    const float kg = 1.0f - k.kr - k.kb;
    const float r = y + 2.0f * (1.0f - k.kr) * cr;
    const float b = y + 2.0f * (1.0f - k.kb) * cb;
    const float g = (y - k.kr * r - k.kb * b) / kg;
    return {clamp01(r), clamp01(g), clamp01(b)};
}

namespace
{

constexpr float kPqM1 = 2610.0f / 16384.0f;
constexpr float kPqM2 = 2523.0f / 4096.0f * 128.0f;
constexpr float kPqC1 = 3424.0f / 4096.0f;
constexpr float kPqC2 = 2413.0f / 4096.0f * 32.0f;
constexpr float kPqC3 = 2392.0f / 4096.0f * 32.0f;

} // namespace

float pq_to_linear(float signal)
{
    const float powered = std::pow(clamp01(signal), 1.0f / kPqM2);
    const float numerator = std::max(powered - kPqC1, 0.0f);
    const float denominator = kPqC2 - kPqC3 * powered;
    return std::pow(numerator / denominator, 1.0f / kPqM1);
}

float linear_to_pq(float linear)
{
    const float powered = std::pow(clamp01(linear), kPqM1);
    return std::pow((kPqC1 + kPqC2 * powered) / (1.0f + kPqC3 * powered), kPqM2);
}

float hlg_to_linear(float signal)
{
    constexpr float a = 0.17883277f;
    constexpr float b = 0.28466892f;
    constexpr float c = 0.55991073f;
    const float value = clamp01(signal);
    if (value <= 0.5f)
    {
        return value * value / 3.0f;
    }
    return (std::exp((value - c) / a) + b) / 12.0f;
}

float tone_map_bt2390(float pq_signal, float source_peak_nits, float target_peak_nits)
{
    if (source_peak_nits <= 0.0f || target_peak_nits <= 0.0f ||
        target_peak_nits >= source_peak_nits)
    {
        return pq_signal;
    }

    const float source_pq = linear_to_pq(source_peak_nits / 10000.0f);
    const float target_pq = linear_to_pq(target_peak_nits / 10000.0f);
    const float e1 = clamp01(pq_signal / source_pq);
    const float max_luminance = target_pq / source_pq;
    const float knee = 1.5f * max_luminance - 0.5f;
    if (e1 < knee)
    {
        return e1 * source_pq;
    }

    // A Hermite spline from the knee (slope 1) to the target peak (slope 0).
    const float t = (e1 - knee) / (1.0f - knee);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float e2 = (2.0f * t3 - 3.0f * t2 + 1.0f) * knee + (t3 - 2.0f * t2 + t) * (1.0f - knee) +
                     (-2.0f * t3 + 3.0f * t2) * max_luminance;
    return e2 * source_pq;
}

} // namespace hui::video
