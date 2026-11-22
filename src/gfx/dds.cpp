// EliVier Explorer - DDS pictures: block-compressed (BC1 to BC5, BC7) and plain 8-bit formats.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/dds.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#include "third_party/bc7decomp/bc7decomp.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hui::gfx
{

namespace
{

enum class Format
{
    bc1,
    bc2,
    bc3,
    bc4,
    bc5,
    bc7,
    masks, // uncompressed, described by bit masks
    r8,    // one 8-bit channel, shown as grey
    rgba8, // R8G8B8A8
    bgra8, // B8G8R8A8
    bgrx8, // B8G8R8X8 (alpha ignored)
    none,
};

constexpr int kMaxSide = 16384;

std::uint32_t u32(const std::string &b, std::size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

Format format_of_dxgi(std::uint32_t dxgi)
{
    switch (dxgi)
    {
    case 71:
    case 72:
        return Format::bc1;
    case 74:
    case 75:
        return Format::bc2;
    case 77:
    case 78:
        return Format::bc3;
    case 80:
        return Format::bc4;
    case 83:
        return Format::bc5;
    case 98:
    case 99:
        return Format::bc7;
    case 28:
    case 29:
        return Format::rgba8;
    case 87:
    case 91:
        return Format::bgra8;
    case 88:
    case 93:
        return Format::bgrx8;
    case 61:
        return Format::r8;
    default:
        return Format::none;
    }
}

struct Rgba
{
    std::uint8_t r, g, b, a;
};

Rgba expand565(std::uint32_t v)
{
    const std::uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}

// The sixteen colours of a BC1 colour block. `four` forces the opaque four-colour mode (BC2, BC3).
void decode_color_block(const unsigned char *p, bool four, Rgba out[16])
{
    const std::uint32_t c0 = p[0] | p[1] << 8;
    const std::uint32_t c1 = p[2] | p[3] << 8;
    Rgba palette[4];
    palette[0] = expand565(c0);
    palette[1] = expand565(c1);
    const auto mix = [](const Rgba &a, const Rgba &b, int wa, int wb, int div)
    {
        return Rgba{static_cast<std::uint8_t>((a.r * wa + b.r * wb) / div),
                    static_cast<std::uint8_t>((a.g * wa + b.g * wb) / div),
                    static_cast<std::uint8_t>((a.b * wa + b.b * wb) / div), 255};
    };
    if (c0 > c1 || four)
    {
        palette[2] = mix(palette[0], palette[1], 2, 1, 3);
        palette[3] = mix(palette[0], palette[1], 1, 2, 3);
    }
    else
    {
        palette[2] = mix(palette[0], palette[1], 1, 1, 2);
        palette[3] = {0, 0, 0, 0}; // transparent black
    }
    std::uint32_t bits = p[4] | p[5] << 8 | p[6] << 16 | static_cast<std::uint32_t>(p[7]) << 24;
    for (int i = 0; i < 16; ++i, bits >>= 2)
        out[i] = palette[bits & 3];
}

// One BC4-style channel: two end values and 3-bit indices into eight steps.
void decode_channel_block(const unsigned char *p, std::uint8_t out[16])
{
    int v[8];
    v[0] = p[0];
    v[1] = p[1];
    if (v[0] > v[1])
        for (int i = 1; i < 7; ++i)
            v[i + 1] = ((7 - i) * v[0] + i * v[1] + 3) / 7;
    else
    {
        for (int i = 1; i < 5; ++i)
            v[i + 1] = ((5 - i) * v[0] + i * v[1] + 2) / 5;
        v[6] = 0;
        v[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= static_cast<std::uint64_t>(p[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i, bits >>= 3)
        out[i] = static_cast<std::uint8_t>(v[bits & 7]);
}

std::size_t block_bytes(Format f)
{
    return f == Format::bc1 || f == Format::bc4 ? 8 : 16;
}

bool is_block(Format f)
{
    return f == Format::bc1 || f == Format::bc2 || f == Format::bc3 || f == Format::bc4 ||
           f == Format::bc5 || f == Format::bc7;
}

// A mask's shift and width, for scaling a channel to 8 bits. Width 0 means the channel is absent.
struct Channel
{
    int shift = 0;
    int bits = 0;
    std::uint32_t mask = 0;
    std::uint8_t read(std::uint32_t pixel, std::uint8_t absent) const
    {
        if (mask == 0 || bits == 0)
            return absent;
        const std::uint32_t v = (pixel & mask) >> shift;
        if (bits >= 8)
            return static_cast<std::uint8_t>(v >> (bits - 8));
        return static_cast<std::uint8_t>(v * 255u / ((1u << bits) - 1u));
    }
};

Channel channel_of(std::uint32_t mask)
{
    Channel c;
    c.mask = mask;
    if (mask == 0)
        return c;
    while (c.shift < 31 && ((mask >> c.shift) & 1) == 0)
        ++c.shift;
    while (c.shift + c.bits < 32 && ((mask >> (c.shift + c.bits)) & 1) != 0)
        ++c.bits;
    return c;
}

} // namespace

bool is_dds(const std::string &bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0;
}

bool decode_dds(const std::string &bytes, ImageData *out, std::string *error)
{
    const auto fail = [error](const char *why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (!is_dds(bytes) || bytes.size() < 128 || u32(bytes, 4) != 124)
        return fail("This is not a readable DDS file");
    const std::uint32_t height = u32(bytes, 12);
    const std::uint32_t width = u32(bytes, 16);
    if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide)
        return fail("The picture has an impossible size");
    if (static_cast<std::uint64_t>(width) * height > kMaxDecodePixels)
        return fail("The picture is too large");

    const std::uint32_t pf_flags = u32(bytes, 80);
    const std::uint32_t four_cc = u32(bytes, 84);
    const std::uint32_t bit_count = u32(bytes, 88);
    std::size_t data_at = 128;
    Format format = Format::none;
    if ((pf_flags & 4) != 0) // a four-letter code
    {
        switch (four_cc)
        {
        case 0x31545844: // DXT1
            format = Format::bc1;
            break;
        case 0x32545844: // DXT2 (premultiplied, read as DXT3)
        case 0x33545844: // DXT3
            format = Format::bc2;
            break;
        case 0x34545844: // DXT4
        case 0x35545844: // DXT5
            format = Format::bc3;
            break;
        case 0x31495441: // ATI1
        case 0x55344342: // BC4U
            format = Format::bc4;
            break;
        case 0x32495441: // ATI2
        case 0x55354342: // BC5U
            format = Format::bc5;
            break;
        case 0x30315844: // DX10
            if (bytes.size() < 148)
                return fail("The DDS file is cut short");
            format = format_of_dxgi(u32(bytes, 128));
            data_at = 148;
            break;
        default:
            break;
        }
    }
    else if ((pf_flags & 0x40) != 0 || (pf_flags & 0x2) != 0 ||
             (pf_flags & 0x20000) != 0) // RGB, alpha, luminance
        format = bit_count == 8 && (pf_flags & 0x20000) != 0 ? Format::r8 : Format::masks;
    if (format == Format::none)
        return fail("This kind of DDS picture cannot be shown");

    const std::size_t w = width;
    const std::size_t h = height;
    out->width = out->source_width = static_cast<int>(w);
    out->height = out->source_height = static_cast<int>(h);
    out->rgba.assign(w * h * 4, 0);
    std::uint8_t *dest = out->rgba.data();
    const unsigned char *data = reinterpret_cast<const unsigned char *>(bytes.data()) + data_at;
    const std::size_t available = bytes.size() - data_at;

    if (is_block(format))
    {
        const std::size_t bw = (w + 3) / 4;
        const std::size_t bh = (h + 3) / 4;
        if (bw * bh * block_bytes(format) > available)
            return fail("The DDS file is cut short");
        for (std::size_t by = 0; by < bh; ++by)
        {
            for (std::size_t bx = 0; bx < bw; ++bx)
            {
                const unsigned char *p = data + (by * bw + bx) * block_bytes(format);
                Rgba px[16];
                switch (format)
                {
                case Format::bc1:
                    decode_color_block(p, false, px);
                    break;
                case Format::bc2:
                {
                    decode_color_block(p + 8, true, px);
                    for (int i = 0; i < 16; ++i)
                    {
                        const int nibble = (p[i / 2] >> (4 * (i & 1))) & 15;
                        px[i].a = static_cast<std::uint8_t>(nibble * 17);
                    }
                    break;
                }
                case Format::bc3:
                {
                    std::uint8_t alpha[16];
                    decode_channel_block(p, alpha);
                    decode_color_block(p + 8, true, px);
                    for (int i = 0; i < 16; ++i)
                        px[i].a = alpha[i];
                    break;
                }
                case Format::bc4:
                {
                    std::uint8_t grey[16];
                    decode_channel_block(p, grey);
                    for (int i = 0; i < 16; ++i)
                        px[i] = {grey[i], grey[i], grey[i], 255};
                    break;
                }
                case Format::bc5:
                {
                    std::uint8_t red[16], green[16];
                    decode_channel_block(p, red);
                    decode_channel_block(p + 8, green);
                    for (int i = 0; i < 16; ++i)
                    {
                        const float x = static_cast<float>(red[i]) / 127.5f - 1.0f;
                        const float y = static_cast<float>(green[i]) / 127.5f - 1.0f;
                        const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                        px[i] = {red[i], green[i], static_cast<std::uint8_t>(z * 127.5f + 127.5f),
                                 255};
                    }
                    break;
                }
                default: // bc7
                {
                    bc7decomp::color_rgba decoded[16];
                    if (!bc7decomp::unpack_bc7(p, decoded))
                        std::memset(decoded, 0,
                                    sizeof(decoded)); // a reserved block decodes to nothing
                    for (int i = 0; i < 16; ++i)
                        px[i] = {decoded[i].r, decoded[i].g, decoded[i].b, decoded[i].a};
                    break;
                }
                }
                for (std::size_t y = 0; y < 4 && by * 4 + y < h; ++y)
                {
                    for (std::size_t x = 0; x < 4 && bx * 4 + x < w; ++x)
                    {
                        std::uint8_t *q = dest + ((by * 4 + y) * w + bx * 4 + x) * 4;
                        const Rgba &c = px[y * 4 + x];
                        q[0] = c.r;
                        q[1] = c.g;
                        q[2] = c.b;
                        q[3] = c.a;
                    }
                }
            }
        }
        return true;
    }

    // Uncompressed: the bytes per pixel and where each channel is.
    std::size_t step = 4;
    Channel red, green, blue, alpha;
    switch (format)
    {
    case Format::r8:
        step = 1;
        break;
    case Format::rgba8:
    case Format::bgra8:
    case Format::bgrx8:
        break;
    default: // masks
        step = bit_count / 8;
        if (bit_count % 8 != 0 || step < 1 || step > 4)
            return fail("This kind of DDS picture cannot be shown");
        red = channel_of(u32(bytes, 92));
        green = channel_of(u32(bytes, 96));
        blue = channel_of(u32(bytes, 100));
        alpha = channel_of(u32(bytes, 104));
        if ((pf_flags & 0x1) == 0)
            alpha = Channel{};
        if (red.mask == 0 && green.mask == 0 && blue.mask == 0 && alpha.mask == 0)
            return fail("This kind of DDS picture cannot be shown");
        break;
    }
    if (w * h * step > available)
        return fail("The DDS file is cut short");
    for (std::size_t i = 0; i < w * h; ++i)
    {
        const unsigned char *p = data + i * step;
        std::uint8_t *q = dest + i * 4;
        switch (format)
        {
        case Format::r8:
            q[0] = q[1] = q[2] = p[0];
            q[3] = 255;
            break;
        case Format::rgba8:
            q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = p[3];
            break;
        case Format::bgra8:
            q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = p[3];
            break;
        case Format::bgrx8:
            q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = 255;
            break;
        default:
        {
            std::uint32_t pixel = 0;
            for (std::size_t k = 0; k < step; ++k)
                pixel |= static_cast<std::uint32_t>(p[k]) << (8 * k);
            const bool grey = green.mask == 0 && blue.mask == 0;
            q[0] = red.read(pixel, 0);
            q[1] = grey ? q[0] : green.read(pixel, 0);
            q[2] = grey ? q[0] : blue.read(pixel, 0);
            q[3] = alpha.read(pixel, 255);
            break;
        }
        }
    }
    return true;
}

} // namespace hui::gfx
