// EliVier Explorer - The last pass of a frame in HDR10 mode: PQ codes packed for the scanout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/hdr_presenter.hpp"

#include "gfx/gl_program.hpp"

namespace hui::gfx
{

namespace
{

constexpr const char *kPackVertex = R"(
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// One texel per pixel, no filtering: the codes must arrive exactly. The scanout reads the word
// 3 << 30 | R << 20 | G << 10 | B from a BGRA surface: the GL's red is its byte 2, green byte 1,
// blue byte 0, alpha byte 3 (D71; video::pack_hdr10 is the same, tested).
constexpr const char *kPackFragment = R"(
layout(location = 0) uniform sampler2D u_frame;
out vec4 o_colour;
void main()
{
    vec3 value = clamp(texelFetch(u_frame, ivec2(gl_FragCoord.xy), 0).rgb, 0.0, 1.0);
    uvec3 code = uvec3(value * 1023.0 + 0.5);
    uint word = (3u << 30) | (code.r << 20) | (code.g << 10) | code.b;
    o_colour = vec4(float((word >> 16) & 255u), float((word >> 8) & 255u), float(word & 255u),
                    float(word >> 24)) / 255.0;
}
)";

struct Format
{
    GLenum internal;
    GLenum type;
    const char *name;
};

// Ten bits a channel at least; the first the GL can render into wins.
constexpr Format kFormats[] = {
    {GL_RGBA16F, GL_HALF_FLOAT, "rgba16f"},
    {GL_RGBA16, GL_UNSIGNED_SHORT, "rgba16"},
    {GL_RGB10_A2, GL_UNSIGNED_INT_2_10_10_10_REV, "rgb10a2"},
};

} // namespace

HdrPresenter::~HdrPresenter()
{
    release();
}

void HdrPresenter::release()
{
    if (framebuffer_ != 0)
        glDeleteFramebuffers(1, &framebuffer_);
    if (colour_ != 0)
        glDeleteTextures(1, &colour_);
    if (depth_stencil_ != 0)
        glDeleteRenderbuffers(1, &depth_stencil_);
    if (program_ != 0)
        glDeleteProgram(program_);
    if (vertex_array_ != 0)
        glDeleteVertexArrays(1, &vertex_array_);
    framebuffer_ = colour_ = depth_stencil_ = program_ = vertex_array_ = 0;
    width_ = height_ = 0;
    format_name_ = "none";
}

bool HdrPresenter::prepare(int width, int height)
{
    if (framebuffer_ != 0 && width == width_ && height == height_)
        return true;
    release();
    if (width <= 0 || height <= 0)
        return false;
    program_ = build_program("hdr pack", kPackVertex, kPackFragment);
    if (program_ == 0)
        return false;
    glGenVertexArrays(1, &vertex_array_);
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glGenRenderbuffers(1, &depth_stencil_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              depth_stencil_);
    bool complete = false;
    for (const Format &format : kFormats)
    {
        if (colour_ != 0)
            glDeleteTextures(1, &colour_);
        glGenTextures(1, &colour_);
        glBindTexture(GL_TEXTURE_2D, colour_);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format.internal), width, height, 0,
                     GL_RGBA, format.type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        {
            format_name_ = format.name;
            complete = true;
            break;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete)
    {
        release();
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

GLuint HdrPresenter::begin(int width, int height)
{
    return prepare(width, height) ? framebuffer_ : 0;
}

void HdrPresenter::finish(int width, int height)
{
    if (framebuffer_ == 0)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glUseProgram(program_);
    glUniform1i(0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, colour_);
    glBindVertexArray(vertex_array_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DITHER); // the GL's default, which SDR frames are drawn with
}

} // namespace hui::gfx
