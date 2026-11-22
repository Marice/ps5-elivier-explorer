// EliVier Explorer - A name too long for its cell scrolls once its row has been in focus a while.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How far to shift the text left, as a pure function of how long the row has been focused: nothing
// for the first second, then a steady scroll to the end, a pause there, back to the start, a pause,
// and again. Any focus change starts the count over (the pane keeps it).

#pragma once

namespace hui::concepts::explorer
{

constexpr float kMarqueeDelay = 1.0f;  // seconds in focus before it starts
constexpr float kMarqueeSpeed = 70.0f; // pixels a second
constexpr float kMarqueePause = 0.8f;  // seconds held at each end

// Pixels to shift the text left: 0 when it fits, or before the delay has passed; never more than
// the overflow (text_width - view_width).
float marquee_offset(float text_width, float view_width, float focused_seconds);

} // namespace hui::concepts::explorer
