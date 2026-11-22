// EliVier Explorer - Pictures from file bytes: JPEG, PNG, BMP, GIF and TGA, as RGBA pixels.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/decode_image.hpp"

#include "gfx/dds.hpp"

extern "C"
{
#include "third_party/stb/image.h"
}

#include <cmath>
#include <cstring>

namespace hui::gfx
{

bool looks_like_image(const std::string &head)
{
    const auto starts = [&head](const char *magic, std::size_t n)
    { return head.size() >= n && std::memcmp(head.data(), magic, n) == 0; };
    return starts("DDS ", 4) || starts("\x89PNG\r\n\x1a\n", 8) || starts("\xff\xd8\xff", 3) ||
           starts("GIF87a", 6) || starts("GIF89a", 6) || starts("BM", 2);
}

void box_downscale(ImageData *image, int factor)
{
    if (factor <= 1 || image->width <= 0 || image->height <= 0)
        return;
    const int w = (image->width + factor - 1) / factor;
    const int h = (image->height + factor - 1) / factor;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            std::uint32_t sum[4] = {0, 0, 0, 0};
            std::uint32_t count = 0;
            for (int dy = 0; dy < factor && y * factor + dy < image->height; ++dy)
            {
                for (int dx = 0; dx < factor && x * factor + dx < image->width; ++dx)
                {
                    const std::uint8_t *p =
                        &image->rgba[(static_cast<std::size_t>(y * factor + dy) *
                                          static_cast<std::size_t>(image->width) +
                                      static_cast<std::size_t>(x * factor + dx)) *
                                     4];
                    for (int c = 0; c < 4; ++c)
                        sum[c] += p[c];
                    ++count;
                }
            }
            std::uint8_t *q = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                    static_cast<std::size_t>(x)) *
                                   4];
            for (int c = 0; c < 4; ++c)
                q[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
        }
    }
    image->rgba = std::move(out);
    image->width = w;
    image->height = h;
}

bool decode_image(const std::string &bytes, ImageData *out, std::string *error)
{
    const auto fail = [error](const char *why)
    {
        if (error != nullptr)
            *error = why;
        return false;
    };
    if (bytes.empty())
        return fail("The file is empty");
    if (is_dds(bytes))
    {
        if (!decode_dds(bytes, out, error))
            return false;
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(out->width) * static_cast<std::uint64_t>(out->height);
        if (pixels > kMaxTexturePixels)
            box_downscale(
                out, static_cast<int>(std::ceil(std::sqrt(
                         static_cast<double>(pixels) / static_cast<double>(kMaxTexturePixels)))));
        return true;
    }
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    const int size = static_cast<int>(bytes.size());
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, size, &width, &height, &channels) || width <= 0 || height <= 0)
    {
        const char *why = stbi_failure_reason();
        if (why != nullptr && std::strstr(why, "too large") != nullptr)
            return fail("The picture is too large");
        return fail("This is not a picture that can be shown");
    }
    if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxDecodePixels)
        return fail("The picture is too large");
    unsigned char *pixels = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
    if (pixels == nullptr)
        return fail("The picture is damaged");
    out->width = out->source_width = width;
    out->height = out->source_height = height;
    out->rgba.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height) * 4);
    stbi_image_free(pixels);
    const std::uint64_t count =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (count > kMaxTexturePixels)
        box_downscale(out,
                      static_cast<int>(std::ceil(std::sqrt(
                          static_cast<double>(count) / static_cast<double>(kMaxTexturePixels)))));
    return true;
}

} // namespace hui::gfx
