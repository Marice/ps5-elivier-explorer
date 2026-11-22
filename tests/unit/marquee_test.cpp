// EliVier Explorer - When and how far a long name scrolls.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/marquee.hpp"

#include <gtest/gtest.h>

namespace
{

using namespace hui::concepts::explorer;

TEST(Marquee, ANameThatFitsNeverMoves)
{
    EXPECT_FLOAT_EQ(marquee_offset(200.0f, 300.0f, 5.0f), 0.0f);
    EXPECT_FLOAT_EQ(marquee_offset(300.0f, 300.0f, 5.0f), 0.0f); // exactly as wide
}

TEST(Marquee, StandsStillForTheFirstSecond)
{
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, 0.99f), 0.0f);
    EXPECT_GT(marquee_offset(500.0f, 300.0f, 1.2f), 0.0f);
}

TEST(Marquee, ScrollsAtASteadySpeedUpToTheOverflow)
{
    // 200 px of overflow takes 200 / 70 seconds.
    const float one = marquee_offset(500.0f, 300.0f, kMarqueeDelay + 1.0f);
    const float two = marquee_offset(500.0f, 300.0f, kMarqueeDelay + 2.0f);
    EXPECT_NEAR(one, kMarqueeSpeed, 0.01f);
    EXPECT_NEAR(two, 2.0f * kMarqueeSpeed, 0.01f);
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, kMarqueeDelay + 200.0f / kMarqueeSpeed), 200.0f);
}

TEST(Marquee, HoldsAtTheEndThenJumpsBackAndWaitsAtTheStart)
{
    const float travel = 200.0f / kMarqueeSpeed;
    const float end = kMarqueeDelay + travel;
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, end + 0.1f), 200.0f);
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, end + kMarqueePause - 0.01f), 200.0f);
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, end + kMarqueePause + 0.1f), 0.0f);
    EXPECT_FLOAT_EQ(marquee_offset(500.0f, 300.0f, end + 2.0f * kMarqueePause - 0.01f), 0.0f);
    EXPECT_GT(marquee_offset(500.0f, 300.0f, end + 2.0f * kMarqueePause + 0.3f), 0.0f);
}

TEST(Marquee, GoesOnForeverAndNeverLeavesItsRange)
{
    for (int step = 0; step < 3300; ++step)
    {
        const float seconds = static_cast<float>(step) * 0.037f;
        const float shift = marquee_offset(900.0f, 320.0f, seconds);
        EXPECT_GE(shift, 0.0f);
        EXPECT_LE(shift, 580.0f);
    }
}

} // namespace
