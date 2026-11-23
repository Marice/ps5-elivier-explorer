// EliVier Explorer - Audio clock and frame selection for video playback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/av_sync.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{

using namespace hui::video;

TEST(AudioClock, StartsAtTheBaseTime)
{
    AudioClock clock(48000);
    clock.reset(10.0);
    EXPECT_DOUBLE_EQ(clock.time(0), 10.0);
}

TEST(AudioClock, CountsWhatWasHandedToTheOutputMinusWhatIsStillQueued)
{
    AudioClock clock(48000);
    clock.reset(10.0);
    clock.on_written(48000); // one second decoded and pushed into the ring
    EXPECT_NEAR(clock.time(12000), 10.75, 1e-9); // a quarter second is still waiting to be heard
    EXPECT_NEAR(clock.time(0), 11.0, 1e-9);
}

TEST(AudioClock, StandsStillWhileTheOutputIsPaused)
{
    AudioClock clock(48000);
    clock.reset(0.0);
    clock.on_written(24000);
    const double first = clock.time(24000);
    const double second = clock.time(24000); // nothing was consumed in between
    EXPECT_DOUBLE_EQ(first, second);
}

TEST(AudioClock, SubtractsTheOutputLatency)
{
    AudioClock clock(48000);
    clock.reset(5.0);
    clock.set_latency(0.02);
    clock.on_written(48000);
    EXPECT_NEAR(clock.time(0), 5.98, 1e-9);
}

TEST(AudioClock, NeverRunsBeforeItsBase)
{
    AudioClock clock(48000);
    clock.reset(3.0);
    clock.set_latency(0.5);
    clock.on_written(100);
    EXPECT_DOUBLE_EQ(clock.time(0), 3.0);
    EXPECT_DOUBLE_EQ(clock.time(1000000), 3.0); // a ring that reports more than was ever written
}

TEST(AudioClock, ResetStartsOverAfterASeek)
{
    AudioClock clock(48000);
    clock.reset(0.0);
    clock.on_written(96000);
    clock.reset(60.0);
    EXPECT_DOUBLE_EQ(clock.time(0), 60.0);
    clock.on_written(24000);
    EXPECT_NEAR(clock.time(0), 60.5, 1e-9);
}

TEST(FrameSelection, NoFramesMeansNothingToShow)
{
    const Selection pick = select_frame({}, 1.0, SyncConfig{});
    EXPECT_FALSE(pick.present);
    EXPECT_EQ(pick.drop, 0u);
}

TEST(FrameSelection, AFrameInTheFutureIsHeld)
{
    const std::vector<double> queue{1.5, 1.54};
    const Selection pick = select_frame(queue, 1.0, SyncConfig{});
    EXPECT_FALSE(pick.present);
    EXPECT_EQ(pick.drop, 0u);
}

TEST(FrameSelection, AFrameThatIsDueIsShown)
{
    const std::vector<double> queue{1.0, 1.04};
    const Selection pick = select_frame(queue, 1.0, SyncConfig{});
    EXPECT_TRUE(pick.present);
    EXPECT_EQ(pick.drop, 0u);
}

TEST(FrameSelection, ALittleEarlyIsStillShownWithinTheLead)
{
    SyncConfig config;
    config.lead = 0.008;
    const std::vector<double> queue{1.005};
    EXPECT_TRUE(select_frame(queue, 1.0, config).present);
    const std::vector<double> too_early{1.02};
    EXPECT_FALSE(select_frame(too_early, 1.0, config).present);
}

TEST(FrameSelection, CatchesUpByDroppingOlderDueFramesAndShowingTheNewest)
{
    const std::vector<double> queue{0.90, 0.94, 0.98, 1.02};
    const Selection pick = select_frame(queue, 1.0, SyncConfig{});
    EXPECT_TRUE(pick.present);
    EXPECT_EQ(pick.drop, 2u); // 0.90 and 0.94 go; 0.98 is shown; 1.02 waits
}

TEST(FrameSelection, AVeryLateSingleFrameIsStillShownSoThePictureDoesNotFreeze)
{
    const std::vector<double> queue{0.2};
    const Selection pick = select_frame(queue, 5.0, SyncConfig{});
    EXPECT_TRUE(pick.present);
    EXPECT_EQ(pick.drop, 0u);
}

TEST(FrameSelection, AnUnknownTimestampIsShownAtOnce)
{
    const std::vector<double> queue{kNoTimestamp, 7.0};
    const Selection pick = select_frame(queue, 1.0, SyncConfig{});
    EXPECT_TRUE(pick.present);
    EXPECT_EQ(pick.drop, 0u);
}

} // namespace
