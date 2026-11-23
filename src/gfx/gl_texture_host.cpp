// EliVier Explorer - Textures for the viewers, made with OpenGL.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/gl_texture_host.hpp"

#include <GL/glcorearb.h>

namespace hui::gfx
{

std::uint32_t GlTextureHost::create(int width, int height, const std::uint8_t *rgba)
{
    if (width <= 0 || height <= 0 || rgba == nullptr)
        return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

bool GlTextureHost::update(std::uint32_t texture, int width, int height, const std::uint8_t *rgba)
{
    if (texture == 0 || width <= 0 || height <= 0 || rgba == nullptr)
        return false;
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return true;
}

void GlTextureHost::destroy(std::uint32_t texture)
{
    if (texture == 0)
        return;
    const GLuint id = texture;
    glDeleteTextures(1, &id);
}

} // namespace hui::gfx
