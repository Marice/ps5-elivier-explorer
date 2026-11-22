// EliVier Explorer - A name too long for its cell scrolls once its row has been in focus a while.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/marquee.hpp"

#include <cmath>

namespace hui::concepts::explorer
{

float marquee_offset(float text_width, float view_width, float focused_seconds)
{
    const float overflow = text_width - view_width;
    if (overflow <= 0.0f || focused_seconds < kMarqueeDelay)
        return 0.0f;
    float t = focused_seconds - kMarqueeDelay;
    const float travel = overflow / kMarqueeSpeed;
    if (t < travel)
        return t * kMarqueeSpeed; // the first run starts at once: the delay was the wait
    t -= travel;
    // Then the rest of the loop: a pause at the end, the jump back, a pause at the start, the run.
    const float period = 2.0f * kMarqueePause + travel;
    t = std::fmod(t, period);
    if (t < kMarqueePause)
        return overflow;
    t -= kMarqueePause;
    if (t < kMarqueePause)
        return 0.0f;
    t -= kMarqueePause;
    const float shift = t * kMarqueeSpeed;
    return shift < overflow ? shift : overflow;
}

} // namespace hui::concepts::explorer
