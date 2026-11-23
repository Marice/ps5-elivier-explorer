// EliVier Explorer - UTC calendar conversion behind the gmtime_r the video libraries need.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/utc_time.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>
#include <limits>

namespace
{

using hui::video::utc_from_time;

std::tm convert(std::int64_t seconds)
{
    std::tm result{};
    EXPECT_TRUE(utc_from_time(seconds, result));
    return result;
}

TEST(UtcTime, EpochIsThursdayTheFirstOfJanuary1970)
{
    const std::tm epoch = convert(0);
    EXPECT_EQ(epoch.tm_year, 70);
    EXPECT_EQ(epoch.tm_mon, 0);
    EXPECT_EQ(epoch.tm_mday, 1);
    EXPECT_EQ(epoch.tm_wday, 4);
    EXPECT_EQ(epoch.tm_yday, 0);
    EXPECT_EQ(epoch.tm_hour + epoch.tm_min + epoch.tm_sec, 0);
    EXPECT_EQ(epoch.tm_isdst, 0);
}

TEST(UtcTime, OneSecondBeforeTheEpochIsTheLastSecondOf1969)
{
    const std::tm before = convert(-1);
    EXPECT_EQ(before.tm_year, 69);
    EXPECT_EQ(before.tm_mon, 11);
    EXPECT_EQ(before.tm_mday, 31);
    EXPECT_EQ(before.tm_hour, 23);
    EXPECT_EQ(before.tm_min, 59);
    EXPECT_EQ(before.tm_sec, 59);
    EXPECT_EQ(before.tm_wday, 3);
    EXPECT_EQ(before.tm_yday, 364);
}

TEST(UtcTime, LeapDayOf2024CountsInTheDayOfTheYear)
{
    const std::tm leap = convert(1709164800); // 2024-02-29 00:00:00 UTC
    EXPECT_EQ(leap.tm_year, 124);
    EXPECT_EQ(leap.tm_mon, 1);
    EXPECT_EQ(leap.tm_mday, 29);
    EXPECT_EQ(leap.tm_yday, 59);
    EXPECT_EQ(leap.tm_wday, 4);

    const std::tm after = convert(1709164800 + 86400); // 2024-03-01
    EXPECT_EQ(after.tm_mon, 2);
    EXPECT_EQ(after.tm_mday, 1);
    EXPECT_EQ(after.tm_yday, 60);
}

TEST(UtcTime, TheYear2100IsNotALeapYear)
{
    const std::tm day = convert(4107542400); // 2100-03-01 00:00:00 UTC
    EXPECT_EQ(day.tm_year, 200);
    EXPECT_EQ(day.tm_mon, 2);
    EXPECT_EQ(day.tm_mday, 1);
    EXPECT_EQ(day.tm_yday, 59);
}

TEST(UtcTime, MatchesTheSystemConversionAcrossFourCenturies)
{
    // 1600 to 2600, a prime step so every hour, minute and second shows up.
    long checked = 0;
    for (std::int64_t seconds = -11676096000LL; seconds < 20000000000LL; seconds += 100003)
    {
        const std::time_t system_seconds = static_cast<std::time_t>(seconds);
        std::tm expected{};
        if (gmtime_r(&system_seconds, &expected) == nullptr)
        {
            continue;
        }
        std::tm actual{};
        ASSERT_TRUE(utc_from_time(seconds, actual)) << seconds;
        ASSERT_EQ(actual.tm_year, expected.tm_year) << seconds;
        ASSERT_EQ(actual.tm_mon, expected.tm_mon) << seconds;
        ASSERT_EQ(actual.tm_mday, expected.tm_mday) << seconds;
        ASSERT_EQ(actual.tm_hour, expected.tm_hour) << seconds;
        ASSERT_EQ(actual.tm_min, expected.tm_min) << seconds;
        ASSERT_EQ(actual.tm_sec, expected.tm_sec) << seconds;
        ASSERT_EQ(actual.tm_wday, expected.tm_wday) << seconds;
        ASSERT_EQ(actual.tm_yday, expected.tm_yday) << seconds;
        ++checked;
    }
    EXPECT_GT(checked, 300000);
}

TEST(UtcTime, RefusesAYearThatDoesNotFitInTmYear)
{
    std::tm result{};
    EXPECT_FALSE(utc_from_time(std::numeric_limits<std::int64_t>::max(), result));
    EXPECT_FALSE(utc_from_time(std::numeric_limits<std::int64_t>::min(), result));
}

} // namespace
