// EliVier Explorer - FFmpeg's colour tags turned into the player's own ColourInfo.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal to the video module: it includes FFmpeg's headers, which the public headers do not.

#pragma once

#include "video/colour.hpp"

extern "C"
{
#include <libavutil/pixfmt.h>
}

namespace hui::video
{

// The file may say nothing (every tag unspecified). Then BT.2020 primaries or a PQ or HLG curve
// mean BT.2020, and otherwise the usual rule applies: pictures below 720 lines are BT.601, larger
// ones BT.709. Range is limited unless the file says full.
ColourInfo colour_info(AVColorTransferCharacteristic transfer, AVColorSpace space,
                       AVColorPrimaries primaries, AVColorRange range, int height);

} // namespace hui::video
