// EliVier Explorer - A decoded picture to RGBA8 on the CPU, ready for a texture.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/convert.hpp"

#include "video/colour.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hui::video
{

namespace
{

constexpr int kMaxDimension = 16384;
// The HDR curve: what a PQ master is assumed to peak at, and what an SDR screen shows as white.
constexpr float kAssumedMasterNits = 1000.0f;
constexpr float kSdrWhiteNits = 203.0f;

struct Coefficients
{
    // Q16 factors for one output scale: out = (cy*(Y-y_off) + cr*(Cr-mid) + cb*(Cb-mid)) >> 16.
    std::int32_t y_off;
    std::int32_t mid;
    std::int32_t cy;
    std::int32_t crr;
    std::int32_t cbb;
    std::int32_t cgr;
    std::int32_t cgb;
    std::int32_t scale; // the largest output value
};

Coefficients make_coefficients(Matrix matrix, Range range, int depth, int scale)
{
    float kr = 0.2126f;
    float kb = 0.0722f;
    if (matrix == Matrix::bt601)
    {
        kr = 0.299f;
        kb = 0.114f;
    }
    else if (matrix == Matrix::bt2020)
    {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    const float kg = 1.0f - kr - kb;

    const float unit = static_cast<float>(1 << (depth - 8)); // the 8-bit offsets, at this depth
    float y_off = 0.0f;
    float y_span = static_cast<float>((1 << depth) - 1);
    float c_span = y_span;
    if (range == Range::limited)
    {
        y_off = 16.0f * unit;
        y_span = 219.0f * unit;
        c_span = 224.0f * unit;
    }
    const auto q16 = [](float value) {
        return static_cast<std::int32_t>(std::lround(value * 65536.0f));
    };
    const float s = static_cast<float>(scale);
    Coefficients c{};
    c.y_off = static_cast<std::int32_t>(y_off);
    c.mid = 1 << (depth - 1); // the neutral chroma code: 128 at 8 bits, 512 at 10
    c.cy = q16(s / y_span);
    c.crr = q16(s * 2.0f * (1.0f - kr) / c_span);
    c.cbb = q16(s * 2.0f * (1.0f - kb) / c_span);
    c.cgr = q16(s * 2.0f * kr * (1.0f - kr) / (kg * c_span));
    c.cgb = q16(s * 2.0f * kb * (1.0f - kb) / (kg * c_span));
    c.scale = scale;
    return c;
}

std::int32_t clamp_to(std::int32_t value, std::int32_t largest)
{
    return value < 0 ? 0 : (value > largest ? largest : value);
}

void build_pq_table(ConvertTables &tables)
{
    for (int code = 0; code < 1024; ++code)
    {
        const float mapped =
            tone_map_bt2390(static_cast<float>(code) / 1023.0f, kAssumedMasterNits, kSdrWhiteNits);
        const float nits = pq_to_linear(mapped) * 10000.0f;
        const float light = std::min(nits / kSdrWhiteNits, 1.0f);
        const float encoded = light <= 0.0031308f ? 12.92f * light
                                                  : 1.055f * std::pow(light, 1.0f / 2.4f) - 0.055f;
        const float clamped = std::min(std::max(encoded, 0.0f), 1.0f);
        tables.pq_to_sdr[static_cast<std::size_t>(code)] =
            static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
    }
    tables.pq_ready = true;
}

// One sample of a plane, whatever its depth.
inline std::int32_t sample(const std::uint8_t *row, int x, bool wide)
{
    if (!wide)
        return row[x];
    std::uint16_t word = 0;
    std::memcpy(&word, row + 2 * static_cast<std::ptrdiff_t>(x), 2);
    return word;
}

// What one output channel is made of: 8 bits as they are, or 16 bits (value times 257).
inline std::uint8_t store(std::uint8_t * /*tag*/, std::int32_t value)
{
    return static_cast<std::uint8_t>(value);
}
inline std::uint16_t store(std::uint16_t * /*tag*/, std::int32_t value)
{
    return static_cast<std::uint16_t>(value * 257);
}

template <typename Out>
bool convert_frame(const Frame &frame, ConvertTables &tables, Out *out_rgba,
                   std::size_t out_pitch_bytes)
{
    const int width = frame.width;
    const int height = frame.height;
    const int depth = frame.bit_depth;
    if (out_rgba == nullptr || frame.plane_count != 3 || frame.semi_planar || width <= 0 ||
        height <= 0 || width > kMaxDimension || height > kMaxDimension || depth < 8 || depth > 16 ||
        frame.chroma_shift_x < 0 || frame.chroma_shift_x > 2 || frame.chroma_shift_y < 0 ||
        frame.chroma_shift_y > 2)
        return false;
    for (int i = 0; i < 3; ++i)
        if (frame.planes[i] == nullptr || frame.pitches[i] <= 0)
            return false;
    const bool wide = depth > 8;
    const int bytes = wide ? 2 : 1;
    const int chroma_width = (width + (1 << frame.chroma_shift_x) - 1) >> frame.chroma_shift_x;
    if (frame.pitches[0] < width * bytes || frame.pitches[1] < chroma_width * bytes ||
        frame.pitches[2] < chroma_width * bytes ||
        out_pitch_bytes < static_cast<std::size_t>(width) * 4 * sizeof(Out))
        return false;

    const bool hdr = frame.colour.transfer == Transfer::pq;
    if (hdr && !tables.pq_ready)
        build_pq_table(tables);
    // HDR goes through a 10-bit signal and the table; everything else straight to 8 bits.
    const Coefficients k = make_coefficients(frame.colour.matrix, frame.colour.range, depth,
                                             hdr ? 1023 : 255);

    for (int y = 0; y < height; ++y)
    {
        const std::uint8_t *luma =
            frame.planes[0] + static_cast<std::ptrdiff_t>(y) * frame.pitches[0];
        const int chroma_row = y >> frame.chroma_shift_y;
        const std::uint8_t *cb_row =
            frame.planes[1] + static_cast<std::ptrdiff_t>(chroma_row) * frame.pitches[1];
        const std::uint8_t *cr_row =
            frame.planes[2] + static_cast<std::ptrdiff_t>(chroma_row) * frame.pitches[2];
        Out *out = reinterpret_cast<Out *>(reinterpret_cast<std::uint8_t *>(out_rgba) +
                                           static_cast<std::ptrdiff_t>(y) * out_pitch_bytes);
        for (int x = 0; x < width; ++x, out += 4)
        {
            const int cx = x >> frame.chroma_shift_x;
            const std::int32_t cb = sample(cb_row, cx, wide) - k.mid;
            const std::int32_t cr = sample(cr_row, cx, wide) - k.mid;
            const std::int64_t base =
                std::int64_t{k.cy} * (sample(luma, x, wide) - k.y_off) + 32768;
            const std::int64_t red = base + std::int64_t{k.crr} * cr;
            const std::int64_t green = base - std::int64_t{k.cgr} * cr - std::int64_t{k.cgb} * cb;
            const std::int64_t blue = base + std::int64_t{k.cbb} * cb;
            const std::int32_t r = clamp_to(static_cast<std::int32_t>(red >> 16), k.scale);
            const std::int32_t g = clamp_to(static_cast<std::int32_t>(green >> 16), k.scale);
            const std::int32_t b = clamp_to(static_cast<std::int32_t>(blue >> 16), k.scale);
            if (hdr)
            {
                out[0] = store(out, tables.pq_to_sdr[static_cast<std::size_t>(r)]);
                out[1] = store(out, tables.pq_to_sdr[static_cast<std::size_t>(g)]);
                out[2] = store(out, tables.pq_to_sdr[static_cast<std::size_t>(b)]);
            }
            else
            {
                out[0] = store(out, r);
                out[1] = store(out, g);
                out[2] = store(out, b);
            }
            out[3] = store(out, 255);
        }
    }
    return true;
}

} // namespace

bool convert_to_rgba(const Frame &frame, ConvertTables &tables, std::uint8_t *out_rgba,
                     std::size_t out_pitch_bytes)
{
    return convert_frame(frame, tables, out_rgba, out_pitch_bytes);
}

bool convert_to_rgba16(const Frame &frame, ConvertTables &tables, std::uint16_t *out_rgba,
                       std::size_t out_pitch_bytes)
{
    return convert_frame(frame, tables, out_rgba, out_pitch_bytes);
}

} // namespace hui::video
