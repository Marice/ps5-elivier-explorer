// EliVier Explorer - The display's HDR10 mode, for HDR films in full screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/hdr_output.hpp"

#include "explorer/trace.hpp"
#include "platform/ps5/video_out_hook.hpp"

namespace hui::ps5
{

HdrOut::~HdrOut()
{
    // Nothing here: the viewer leaves HDR when it closes, and no VideoOut call is made while the
    // process ends (D69).
}

bool HdrOut::enter()
{
    if (active_)
        return true;
    if (!presenter_.prepare(width_, height_))
    {
        explorer::trace("hdr: no target to pack the frame in; staying SDR");
        return false;
    }
    if (!video_out_set_format(kVideoOutFormatHdr10))
        return false;
    explorer::trace("hdr: on, frame packed from %s", presenter_.format_name());
    active_ = true;
    return true;
}

void HdrOut::leave()
{
    if (!active_)
        return;
    const VideoOutState state = video_out_state();
    video_out_set_format(state.pixel_format != 0 ? state.pixel_format : kVideoOutFormatSdr);
    active_ = false;
    explorer::trace("hdr: off");
}

} // namespace hui::ps5
