// EliVier Explorer - Pictures from file bytes: JPEG, PNG, BMP, GIF and TGA, as RGBA pixels.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No GL, so it runs on the worker thread and in the host tests. A picture of more than 16
// megapixels is box-averaged down first (a texture that size is no better to look at on a TV and
// costs 64 MiB); one of more than 64 megapixels is refused before it is decoded.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hui::gfx
{

constexpr std::uint64_t kMaxDecodePixels = 64ull * 1000 * 1000;  // refused above this
constexpr std::uint64_t kMaxTexturePixels = 16ull * 1000 * 1000; // scaled down above this

struct ImageData
{
    int width = 0; // what is in `rgba`
    int height = 0;
    int source_width = 0; // what the file has
    int source_height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, rows top to bottom
};

// True when the bytes start like a picture this can read.
bool looks_like_image(const std::string &head);

// Returns false and fills `error` when it is not a picture, is damaged, or is too large.
bool decode_image(const std::string &bytes, ImageData *out, std::string *error);

// Box-averages by a whole factor (>= 1) in both directions. Exposed for the tests.
void box_downscale(ImageData *image, int factor);

} // namespace hui::gfx
