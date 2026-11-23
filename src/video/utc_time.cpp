// EliVier Explorer - UTC calendar conversion behind the gmtime_r the video libraries need.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/utc_time.hpp"

#include <limits>

namespace hui::video
{

bool utc_from_time(std::int64_t seconds, std::tm &out)
{
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0)
    {
        rest += 86400;
        days -= 1;
    }

    // Howard Hinnant's civil_from_days. The epoch moves to 0000-03-01 so that the leap day is the
    // last day of a year, and 400 years (146097 days) repeat exactly.
    const std::int64_t shifted = days + 719468;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const std::int64_t day_of_era = shifted - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t month_index = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * month_index + 2) / 5 + 1;
    const std::int64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);

    const std::int64_t tm_year = year - 1900;
    if (tm_year > std::numeric_limits<int>::max() || tm_year < std::numeric_limits<int>::min())
    {
        return false;
    }

    static constexpr int kDaysBeforeMonth[12] = {0,   31,  59,  90,  120, 151,
                                                 181, 212, 243, 273, 304, 334};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    std::int64_t weekday = (days + 4) % 7; // 1970-01-01 was a Thursday
    if (weekday < 0)
    {
        weekday += 7;
    }

    out.tm_sec = static_cast<int>(rest % 60);
    out.tm_min = static_cast<int>((rest / 60) % 60);
    out.tm_hour = static_cast<int>(rest / 3600);
    out.tm_mday = static_cast<int>(day);
    out.tm_mon = static_cast<int>(month - 1);
    out.tm_year = static_cast<int>(tm_year);
    out.tm_wday = static_cast<int>(weekday);
    out.tm_yday =
        kDaysBeforeMonth[month - 1] + static_cast<int>(day) - 1 + (leap && month > 2 ? 1 : 0);
    out.tm_isdst = 0;
    return true;
}

} // namespace hui::video
