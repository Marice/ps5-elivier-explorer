// EliVier Explorer - DDS pictures: block-compressed (BC1 to BC5, BC7) and plain 8-bit formats.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoded on the CPU into RGBA, first mip level of the first face or slice only. Both header
// styles are read: the classic one (DXT1, DXT3, DXT5, ATI1, ATI2, pixel masks) and the DX10 one
// (BC1 to BC5 and BC7 as UNORM or sRGB, R8G8B8A8, B8G8R8A8, B8G8R8X8, R8). BC5 gets its blue back
// from the two stored channels, so a normal map looks like one; BC4 is shown as grey.

#pragma once

#include "gfx/decode_image.hpp"

namespace hui::gfx
{

bool is_dds(const std::string &bytes);

// Fills `out` (width, height, rgba; the source size equals them). False with `error` when the file
// is damaged, truncated, too large, or in a format this does not read.
bool decode_dds(const std::string &bytes, ImageData *out, std::string *error);

} // namespace hui::gfx
