// EliVier Explorer - A pane's entries as table rows: ids, cells, dates, kinds.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/row_format.hpp"
#include "explorer/pane_model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace
{

using hui::concepts::explorer::file_kind;
using hui::concepts::explorer::FileKind;
using hui::concepts::explorer::format_time;
using hui::concepts::explorer::kColModified;
using hui::concepts::explorer::kColName;
using hui::concepts::explorer::kColSize;
using hui::concepts::explorer::to_rows;
using hui::explorer::Entry;
using hui::explorer::EntryKind;
using hui::explorer::PaneState;

Entry file(const std::string &name, std::uint64_t size, std::int64_t modified = 1700000000)
{
    Entry entry;
    entry.name = name;
    entry.kind = EntryKind::file;
    entry.size = size;
    entry.modified_unix = modified;
    return entry;
}

Entry folder(const std::string &name)
{
    Entry entry;
    entry.name = name;
    entry.kind = EntryKind::directory;
    entry.modified_unix = 1700000000;
    return entry;
}

TEST(RowFormat, OneRowPerEntryWithTextAndNumbers)
{
    PaneState state;
    state.entries = {folder("Roms"), file("game.zip", 1536), file("empty.bin", 0)};
    const auto rows = to_rows(state);
    ASSERT_EQ(rows.size(), 3u);
    ASSERT_EQ(rows[0].cells.size(), 3u);
    EXPECT_EQ(rows[0].cells[kColName].text, "Roms");
    EXPECT_EQ(rows[0].cells[kColSize].text, "-"); // a folder has no size to show
    EXPECT_EQ(rows[0].cells[kColModified].text, "2023-11-14 22:13");
    EXPECT_EQ(rows[1].cells[kColSize].text, "1.5 KB");
    EXPECT_DOUBLE_EQ(rows[1].cells[kColSize].number, 1536.0);
    EXPECT_EQ(rows[2].cells[kColSize].text, "0 B"); // a 0-byte file is not a folder
    EXPECT_DOUBLE_EQ(rows[1].cells[kColModified].number, 1700000000.0);
    EXPECT_FALSE(rows[0].disabled);
}

TEST(RowFormat, TheTagIsTheIndexIntoTheEntries)
{
    PaneState state;
    state.entries = {file("b", 1), file("a", 2), file("c", 3)};
    const auto rows = to_rows(state);
    for (std::size_t i = 0; i < rows.size(); ++i)
        EXPECT_EQ(rows[i].tag, static_cast<int>(i));
}

TEST(RowFormat, AnEntryKeepsItsIdWhenTheOrderChanges)
{
    PaneState by_name;
    by_name.entries = {file("a.txt", 9), file("b.txt", 1), file("c.txt", 5)};
    PaneState by_size;
    by_size.entries = {file("b.txt", 1), file("c.txt", 5), file("a.txt", 9)};
    const auto first = to_rows(by_name);
    const auto second = to_rows(by_size);
    // Table keeps a row's place and focus by id: a re-sort must not renumber the rows.
    EXPECT_EQ(first[0].id, second[2].id); // a.txt
    EXPECT_EQ(first[1].id, second[0].id); // b.txt
    EXPECT_EQ(first[2].id, second[1].id); // c.txt
}

TEST(RowFormat, IdsAreUniqueAndNotNegativeAcrossAFullPane)
{
    PaneState state;
    for (int i = 0; i < hui::explorer::kPaneRowCap; ++i)
        state.entries.push_back(file("Track " + std::to_string(i) + ".ogg", 1));
    const auto rows = to_rows(state);
    std::set<int> ids;
    for (const auto &row : rows)
    {
        EXPECT_GE(row.id, 0);
        ids.insert(row.id);
    }
    EXPECT_EQ(ids.size(), rows.size());
}

TEST(RowFormat, FormatsTimeInUtcWithoutALocalClock)
{
    EXPECT_EQ(format_time(1700000000), "2023-11-14 22:13");
    EXPECT_EQ(format_time(951782400), "2000-02-29 00:00"); // a leap day
    EXPECT_EQ(format_time(1795205353), "2026-11-20 20:09");
    EXPECT_EQ(format_time(86399), "1970-01-01 23:59");
    EXPECT_EQ(format_time(4102444800), "2100-01-01 00:00");
    EXPECT_EQ(format_time(0), "-");
    EXPECT_EQ(format_time(-5), "-");
}

TEST(RowFormat, ATurnedPageMovesTheFocusAndStopsAtTheEnds)
{
    using hui::concepts::explorer::paged_focus;
    EXPECT_EQ(paged_focus(0, 100, 9, 1), 9);
    EXPECT_EQ(paged_focus(9, 100, 9, 1), 18);
    EXPECT_EQ(paged_focus(18, 100, 9, -1), 9);
    EXPECT_EQ(paged_focus(3, 100, 9, -1), 0);  // the first row, not before it
    EXPECT_EQ(paged_focus(95, 100, 9, 1), 99); // the last row, not past it
    EXPECT_EQ(paged_focus(0, 0, 9, 1), 0);     // nothing to focus
    EXPECT_EQ(paged_focus(0, 5, 0, 1), 1);     // a page of nothing is still one row
}

TEST(RowFormat, KindsFollowTheExtensionAnyCase)
{
    EXPECT_EQ(file_kind(folder("Anything.zip")), FileKind::folder);
    EXPECT_EQ(file_kind(file("a.ZIP", 1)), FileKind::archive);
    EXPECT_EQ(file_kind(file("a.tar.gz", 1)), FileKind::archive);
    EXPECT_EQ(file_kind(file("a.Png", 1)), FileKind::image);
    EXPECT_EQ(file_kind(file("a.ogg", 1)), FileKind::audio);
    EXPECT_EQ(file_kind(file("a.mkv", 1)), FileKind::video);
    EXPECT_EQ(file_kind(file("notes.txt", 1)), FileKind::text);
    EXPECT_EQ(file_kind(file("game.elf", 1)), FileKind::program);
    EXPECT_EQ(file_kind(file("game.ffpfsc", 1)), FileKind::disc);
    EXPECT_EQ(file_kind(file("README", 1)), FileKind::other);
    EXPECT_EQ(file_kind(file(".hidden", 1)), FileKind::other); // a dot first is not an extension
    EXPECT_EQ(file_kind(file("a.unknownext", 1)), FileKind::other);
}

} // namespace
