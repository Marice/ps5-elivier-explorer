// EliVier Explorer - A decoded picture to RGBA8 or RGBA16 on the CPU, ready for a texture.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first video viewer converts in software (docs/decision-log.md, D51): no shader, nothing in
// the renderer, and every number checked on the PC against src/video/colour.cpp. A GPU path can
// replace this one function later. Integer arithmetic throughout; chroma is nearest-neighbour.
//
// SDR (and HLG, which is shown as it is) is converted straight to 8 bits. HDR in PQ is
// tone-mapped to an SDR screen per channel (BT.2390 from an assumed 1000 nit master down to 203
// nits, then the sRGB curve); there is no gamut mapping from BT.2020 to BT.709, so HDR colours
// look a little over-saturated.

#pragma once

#include "video/video_decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hui::video
{

// What a conversion keeps between pictures: the table of the HDR curve, made on first use.
struct ConvertTables
{
    std::array<std::uint8_t, 1024> pq_to_sdr{};
    bool pq_ready = false;
};

// Converts `frame` to RGBA8 (alpha 255) at its own size, rows top to bottom, `out_pitch_bytes`
// apart. Reads planar YCbCr (three planes) and semi-planar (NV12, and its 16-bit form with the
// value in the low bits, as the hardware decoder writes: plane 1 holds Cb and Cr pairs). False,
// and nothing written, for anything else (missing planes, a depth outside 8 to 16, a chroma
// shift above 2, a pitch too small for the width).
// `out_rgba` must hold out_pitch_bytes * height bytes.
bool convert_to_rgba(const Frame &frame, ConvertTables &tables, std::uint8_t *out_rgba,
                     std::size_t out_pitch_bytes);

// The same picture with 16 bits per channel (every 8-bit value times 257, so 255 is 65535). The
// console uploads 16-bit textures about 15 times faster than 8-bit ones (docs/decision-log.md,
// D52), so a moving picture goes this way. The pitch is in bytes and at least width * 8.
bool convert_to_rgba16(const Frame &frame, ConvertTables &tables, std::uint16_t *out_rgba,
                       std::size_t out_pitch_bytes);

// For splitting one picture over threads (ConvertPool): prepare_conversion checks the frame
// and the output (`out_bytes` per channel, 1 or 2) and readies the tables, once, on one
// thread; then convert_rows_rgba16 may run on several threads at once for rows
// [first_row, end_row), each band its own, sharing the tables read-only.
bool prepare_conversion(const Frame &frame, ConvertTables &tables, std::size_t out_pitch_bytes,
                        std::size_t out_bytes);
void convert_rows_rgba16(const Frame &frame, const ConvertTables &tables, std::uint16_t *out_rgba,
                         std::size_t out_pitch_bytes, int first_row, int end_row);

} // namespace hui::video
