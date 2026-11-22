// EliVier Explorer - A pane's entries as table rows: ids, cells, dates, kinds.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/row_format.hpp"

#include "concepts/explorer/look.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <unordered_set>

namespace hui::concepts::explorer
{

namespace
{

using hui::explorer::Entry;
using hui::explorer::EntryKind;

// FNV-1a, kept to 31 bits so an id is never negative.
int hash_name(const std::string &name)
{
    std::uint32_t hash = 2166136261u;
    for (const char c : name)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return static_cast<int>(hash & 0x7fffffffu);
}

} // namespace

std::string format_time(std::int64_t unix_seconds)
{
    if (unix_seconds <= 0)
        return "-";
    // Days since 1970 to a calendar date (Howard Hinnant's civil_from_days): pure arithmetic.
    const std::int64_t days = unix_seconds / 86400;
    const std::int64_t rest = unix_seconds % 86400;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", static_cast<int>(year), month,
                  day, static_cast<int>(rest / 3600), static_cast<int>(rest % 3600 / 60));
    return text;
}

int paged_focus(int focus, int rows, int page, int direction)
{
    if (rows <= 0)
        return 0;
    const int target = focus + direction * (page < 1 ? 1 : page);
    return target < 0 ? 0 : (target > rows - 1 ? rows - 1 : target);
}

FileKind file_kind(const Entry &entry)
{
    if (entry.kind == EntryKind::directory)
        return FileKind::folder;
    return hui::viewer::icon_by_extension(hui::viewer::FileProbe(entry.name, entry.size).ext());
}

std::vector<ui::TableRow> to_rows(const hui::explorer::PaneState &state)
{
    const std::size_t count = state.entries.size();
    // Ids are handed out in name order, so a collision is resolved the same way whatever the
    // order the entries are in now.
    std::vector<std::size_t> by_name(count);
    std::iota(by_name.begin(), by_name.end(), std::size_t{0});
    std::sort(by_name.begin(), by_name.end(), [&state](std::size_t a, std::size_t b)
              { return state.entries[a].name < state.entries[b].name; });
    std::vector<int> ids(count);
    std::unordered_set<int> used;
    for (const std::size_t index : by_name)
    {
        int id = hash_name(state.entries[index].name);
        while (!used.insert(id).second)
            id = (id + 1) & 0x7fffffff;
        ids[index] = id;
    }

    std::vector<ui::TableRow> rows(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const Entry &entry = state.entries[i];
        ui::TableRow &row = rows[i];
        row.id = ids[i];
        row.tag = static_cast<int>(i);
        row.cells.resize(3);
        row.cells[kColName] = {entry.name, 0.0};
        row.cells[kColSize] = {entry.kind == EntryKind::directory ? std::string("-")
                                                                  : size_text(entry.size),
                               static_cast<double>(entry.size)};
        row.cells[kColModified] = {format_time(entry.modified_unix),
                                   static_cast<double>(entry.modified_unix)};
    }
    return rows;
}

} // namespace hui::concepts::explorer
