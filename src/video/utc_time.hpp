// EliVier Explorer - UTC calendar conversion behind the gmtime_r the video libraries need.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <ctime>

namespace hui::video
{

// Seconds since 1970-01-01 00:00:00 UTC to calendar fields. Pure arithmetic: the console has no
// time zone database, so there is no local time. Fills tm_sec to tm_yday and clears tm_isdst; the
// platform fields (tm_gmtoff, tm_zone) are left to the caller. Returns false when the year does
// not fit in tm_year.
bool utc_from_time(std::int64_t seconds, std::tm &out);

} // namespace hui::video
