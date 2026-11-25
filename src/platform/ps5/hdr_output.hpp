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
#include "platform/ps5/display_egl.hpp"
#include "viewer/viewer.hpp"

namespace hui::ps5
{

class HdrOut final : public viewer::HdrOutput
{
  public:
    // `display` must outlive this: its width()/height() are read live in enter(), not captured
    // once, because the saved resolution can change (1080p, 1440p, 2160p) between runs and a
    // stale size here would let the readiness check pass at the old size while the per-frame
    // presenter (sized from the display every frame, in main.cpp) fails at the new one: the HDR10
    // scanout format would already be set, but with nothing packing the frame for it.
    HdrOut(gfx::HdrPresenter &presenter, const Display &display)
        : presenter_(presenter), display_(display)
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
    const Display &display_;
    bool active_ = false;
};

} // namespace hui::ps5
