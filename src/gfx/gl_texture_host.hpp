// EliVier Explorer - Textures for the viewers, made with OpenGL.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "viewer/viewer.hpp"

namespace hui::gfx
{

// One level, linear filtering, edges clamped (as load_png_texture): mip chains leave
// ps5-opengl's batched fast path.
class GlTextureHost final : public hui::viewer::TextureHost
{
  public:
    std::uint32_t create(int width, int height, const std::uint8_t *rgba) override;
    void destroy(std::uint32_t texture) override;
};

} // namespace hui::gfx
