// EliVier Explorer - The display's HDR10 mode, for HDR films in full screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// enter() retypes the runtime's scanout buffers to HDR10 (video_out_hook, D71) once the last pass
// that packs the frame for them (gfx::HdrPresenter) is ready; leave() puts the runtime's own
// format back. The main loop asks active() each frame to choose between the normal frame and the
// packed one. All on the game thread, which also owns the GL context.

#pragma once

#include "gfx/hdr_presenter.hpp"
#include "viewer/viewer.hpp"

namespace hui::ps5
{

class HdrOut final : public viewer::HdrOutput
{
  public:
    HdrOut(gfx::HdrPresenter &presenter, int width, int height)
        : presenter_(presenter), width_(width), height_(height)
    {
    }
    ~HdrOut() override;

    bool enter() override;
    void leave() override;
    bool active() const override
    {
        return active_;
    }

  private:
    gfx::HdrPresenter &presenter_;
    int width_;
    int height_;
    bool active_ = false;
};

} // namespace hui::ps5
