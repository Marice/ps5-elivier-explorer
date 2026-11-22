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

    int created = 0;
    int destroyed = 0;
    int last_width = 0;
    int last_height = 0;
    std::set<std::uint32_t> live;

  private:
    std::uint32_t next_ = 0;
};

} // namespace hui::testing
