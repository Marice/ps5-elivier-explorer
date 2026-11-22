// EliVier Explorer - A pane's entries as table rows: ids, cells, dates, kinds.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure data in, rows out: no table, no drawing, no clock. The screen sets the columns (with the
// icon slot on the name) and hands every reload's rows to Table::set_rows.

#pragma once

#include "explorer/pane_model.hpp"
#include "ui/components/table.hpp"
#include "viewer/file_probe.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hui::concepts::explorer
{

// Which cell is which, in every row.
constexpr int kColName = 0;
constexpr int kColSize = 1;
constexpr int kColModified = 2;

// Rows in the entries' order. TableRow::id is a hash of the name, so a row keeps its place and
// focus when the sort order changes; unique across the pane (collisions are walked past in name
// order, so the same names get the same ids whatever the order). TableRow::tag is the index into
// the pane's entries.
std::vector<ui::TableRow> to_rows(const hui::explorer::PaneState &state);

// "2026-11-20 20:09" in UTC (the console has no local clock to ask: no localtime_r), "-" for no
// time.
std::string format_time(std::int64_t unix_seconds);

// Where the focus lands when a page is turned: `direction` (-1 or 1) times `page` rows away, held
// to the first and the last row. With no rows, 0.
int paged_focus(int focus, int rows, int page, int direction);

// The picture a row gets: the viewer core knows the kinds (and which a handler claims).
using FileKind = hui::viewer::IconKind;

FileKind file_kind(const hui::explorer::Entry &entry);

} // namespace hui::concepts::explorer
