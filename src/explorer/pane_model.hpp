// EliVier Explorer - What one pane shows: the folder, its rows in order, the selection, the focus.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No drawing and no UI components here: the screen turns this state into rows. Everything goes
// through a Source, so a pane can show a drive today and an SMB share later.
//
//   hui::explorer::PaneModel pane(source);
//   pane.go_to("/mnt/usb0");
//   pane.enter("Roms");
//   pane.toggle_selected("game.zip");

#pragma once

#include "explorer/source.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace hui::explorer
{

// Table::set_rows takes every row and keeps animation state for each, so a folder with tens of
// thousands of files would stall the pane that reloads most often. The first rows in sort order
// are kept and the header says "N of M" (decision D8); the selection is capped the same (D16).
constexpr int kPaneRowCap = 2000;

// Case-insensitive, with runs of digits compared as numbers: "file2" before "file10". 0 when the
// two are equal this way. Used for sorting a pane's rows, and reused wherever file names need
// the same order (the video viewer's "next episode" among its folder's other videos).
int natural_compare(const std::string &a, const std::string &b);

enum class SortKey : std::uint8_t
{
    name,
    size,
    modified,
    kind, // by extension
};

struct PaneState
{
    std::string path;           // absolute, no trailing slash except "/"
    std::vector<Entry> entries; // sorted, at most kPaneRowCap
    SortKey sort_key = SortKey::name;
    bool sort_descending = false;
    bool dirs_first = true;
    std::vector<std::string> selected; // names in this folder, in the order they were marked
    int focus_index = 0;               // into entries
    bool truncated = false;            // entries holds only the first kPaneRowCap
    int total_entries = 0;             // what the folder really holds
    SpaceInfo space;                   // the drive this folder is on, read when it was listed
    // Whether things may be changed here: allowed by guard.hpp, and no link in the path.
    bool changeable = false;
    int list_error = 0; // errno of the last listing that failed, else 0
};

class PaneModel
{
  public:
    explicit PaneModel(Source &source);

    // Lists `path` and makes it the pane's folder. False (and the pane stays where it was, with
    // list_error set) when it cannot be listed.
    bool go_to(const std::string &path);
    bool enter(const std::string &name); // a folder in this one
    bool go_up();                        // false at "/"; the focus lands on the folder left
    // Lists the same folder again. A folder that has gone sends the pane up to one that exists.
    void reload();

    void set_sort(SortKey key, bool descending);
    void set_dirs_first(bool dirs_first);

    void set_focus(int index); // clamped to the rows
    const Entry *focused() const;
    const Entry *entry_at(int index) const;

    bool is_selected(std::string_view name) const;
    void toggle_selected(const std::string &name); // names that are not in this folder are ignored
    void select_all();
    void invert_selection();
    void clear_selection();

    // Both return 0 or an errno, reload, and put the focus on the new name. A name is one path
    // part: "", ".", ".." and anything with a slash is EINVAL; a name that exists is EEXIST.
    int make_folder(const std::string &name);
    int rename_entry(const std::string &from, const std::string &to);

    const PaneState &state() const
    {
        return state_;
    }
    Source &source() const
    {
        return *source_;
    }
    // Points this pane at a different Source from now on (entering or leaving an archive): the
    // caller still has to go_to() wherever the new source should open; nothing here lists on its
    // own, since the right starting path is the caller's to know, not this model's to guess.
    void set_source(Source &source)
    {
        source_ = &source;
    }

    // path + "/" + name, without a double slash at the root.
    static std::string join(const std::string &path, const std::string &name);
    // The folder a path is in; "/" for a top-level path and for "/" itself.
    static std::string parent_of(const std::string &path);

  private:
    // Lists `path`; on success the pane shows it, the focus on `focus_name` or else on `fallback`.
    bool list_into(const std::string &path, const std::string &focus_name, int fallback);
    void reload_focusing(const std::string &focus_name);
    void sort_entries(std::vector<Entry> *entries) const;
    int index_of(std::string_view name) const;

    Source *source_;
    PaneState state_;
};

} // namespace hui::explorer
