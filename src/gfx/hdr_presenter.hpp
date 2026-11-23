// EliVier Explorer - The last pass of a frame in HDR10 mode: PQ codes packed for the scanout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// With the console's scanout buffers retyped to HDR10 (D71), each 32-bit pixel is read as
// 3 << 30 | R << 20 | G << 10 | B, while the GL still writes RGBA8 into them. So in HDR mode the
// frame is drawn into a target of its own whose colours are PQ codes (0..1: the film as it is,
// everything over it already mapped to PQ by the viewer), and this pass writes each pixel's word
// as the four bytes the scanout reads (video::pack_hdr10 is the same arithmetic, tested). Only
// used in HDR mode, so the shared UI shader is untouched.
//
//   GLuint target = presenter.begin(width, height);   // draw the frame into it
//   presenter.finish(width, height);                   // into the default framebuffer

#pragma once

#include <GL/glcorearb.h>

namespace hui::gfx
{

class HdrPresenter
{
  public:
    HdrPresenter() = default;
    HdrPresenter(const HdrPresenter &) = delete;
    HdrPresenter &operator=(const HdrPresenter &) = delete;
    ~HdrPresenter();

    // Makes the target and the program at this size (once, or again for another size). False
    // when the GL refuses, in which case HDR mode must not be used.
    bool prepare(int width, int height);
    // The framebuffer to draw the frame into, 0 when prepare() failed.
    GLuint begin(int width, int height);
    // Packs the target into the default framebuffer.
    void finish(int width, int height);
    void release();

    // What the target was made as, for the trace ("rgba16f").
    const char *format_name() const
    {
        return format_name_;
    }

  private:
    GLuint framebuffer_ = 0;
    GLuint colour_ = 0;
    GLuint depth_stencil_ = 0;
    GLuint program_ = 0;
    GLuint vertex_array_ = 0;
    int width_ = 0;
    int height_ = 0;
    const char *format_name_ = "none";
};

} // namespace hui::gfx
