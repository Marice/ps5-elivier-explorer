// EliVier Explorer - The two time functions FFmpeg needs that the clean-room libc does not export.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libavutil parses and prints dates (av_parse_time, avpriv_dict_set_timestamp) with gmtime_r and
// localtime_r. The console has no time zone database, so both answer in UTC, as the file list
// does. Not part of the host tests: the host has the real functions (the conversion itself is
// tested through utc_time.hpp).

#include "video/utc_time.hpp"

#include <cerrno>
#include <ctime>

extern "C" struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    if (!hui::video::utc_from_time(static_cast<std::int64_t>(*timer), *result))
    {
        errno = EOVERFLOW;
        return nullptr;
    }
    result->tm_gmtoff = 0;
    result->tm_zone = const_cast<char *>("UTC");
    return result;
}

extern "C" struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    return gmtime_r(timer, result);
}
