// EliVier Explorer - A texture host that only counts, for tests of the viewers.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "viewer/viewer.hpp"

#include <set>

namespace hui::testing
{

class FakeTextures final : public hui::viewer::TextureHost
{
  public:
    std::uint32_t create(int width, int height, const std::uint8_t *rgba) override
    {
        if (width <= 0 || height <= 0 || rgba == nullptr)
            return 0;
        last_width = width;
        last_height = height;
        ++created;
        live.insert(++next_);
        return next_;
    }
    void destroy(std::uint32_t texture) override
    {
        ++destroyed;
        live.erase(texture);
    }

    std::uint32_t create_streaming(int width, int height) override
    {
        if (width <= 0 || height <= 0)
            return 0;
        last_width = width;
        last_height = height;
        ++created;
        ++streaming;
        live.insert(++next_);
        return next_;
    }

    bool update(std::uint32_t texture, int width, int height, const std::uint16_t *rgba) override
    {
        if (live.count(texture) == 0 || width != last_width || height != last_height ||
            rgba == nullptr)
            return false;
        ++updated;
        last_pixel0 = rgba[0];
        return true;
    }

    int created = 0;
    int streaming = 0;
    int updated = 0;
    int last_pixel0 = -1;
    int destroyed = 0;
    int last_width = 0;
    int last_height = 0;
    std::set<std::uint32_t> live;

  private:
    std::uint32_t next_ = 0;
};

} // namespace hui::testing
