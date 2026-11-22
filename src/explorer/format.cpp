// EliVier Explorer - Numbers as text for people.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/format.hpp"

#include <cstdio>

namespace hui::explorer
{

std::string format_bytes(std::uint64_t bytes)
{
    static const char *const kUnits[] = {"KB", "MB", "GB", "TB"};
    char text[32];
    if (bytes < 1024)
    {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
        return text;
    }
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3)
    {
        value /= 1024.0;
        ++unit;
    }
    std::snprintf(text, sizeof(text), value >= 100.0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return text;
}

} // namespace hui::explorer
