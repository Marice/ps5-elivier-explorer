// EliVier Explorer - What one pane shows: the folder, its rows in order, the selection, the focus.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/pane_model.hpp"

#include "explorer/guard.hpp"
#include "explorer/trace.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <unordered_set>
#include <utility>

namespace hui::explorer
{

namespace
{

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool digit(char c)
{
    return c >= '0' && c <= '9';
}

std::string extension_of(const std::string &name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot + 1);
}

template <typename T> int three_way(T a, T b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

std::string normalized(const std::string &path)
{
    std::string out = path;
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
    return out.empty() ? std::string("/") : out;
}

// 0, or the errno a bad name earns.
int check_name(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos)
        return EINVAL;
    if (name.size() > 255)
        return ENAMETOOLONG;
    return 0;
}

} // namespace

int natural_compare(const std::string &a, const std::string &b)
{
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        if (digit(a[i]) && digit(b[j]))
        {
            std::size_t ie = i;
            std::size_t je = j;
            while (i < a.size() && a[i] == '0')
                ++i;
            while (j < b.size() && b[j] == '0')
                ++j;
            std::size_t is = i;
            std::size_t js = j;
            while (ie < a.size() && digit(a[ie]))
                ++ie;
            while (je < b.size() && digit(b[je]))
                ++je;
            // After the zeros, more digits is a bigger number; the same count compares by digit.
            const std::size_t la = ie - is;
            const std::size_t lb = je - js;
            if (la != lb)
                return la < lb ? -1 : 1;
            for (; is < ie; ++is, ++js)
            {
                if (a[is] != b[js])
                    return a[is] < b[js] ? -1 : 1;
            }
            i = ie;
            j = je;
            continue;
        }
        const char x = lower(a[i]);
        const char y = lower(b[j]);
        if (x != y)
            return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

PaneModel::PaneModel(Source &source) : source_(source)
{
}

std::string PaneModel::join(const std::string &path, const std::string &name)
{
    if (path.empty() || path.back() == '/')
        return path + name;
    return path + "/" + name;
}

std::string PaneModel::parent_of(const std::string &path)
{
    const std::string clean = normalized(path);
    const std::size_t slash = clean.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return clean.substr(0, slash);
}

void PaneModel::sort_entries(std::vector<Entry> *entries) const
{
    const SortKey key = state_.sort_key;
    const bool descending = state_.sort_descending;
    const bool dirs_first = state_.dirs_first;
    std::sort(entries->begin(), entries->end(),
              [key, descending, dirs_first](const Entry &a, const Entry &b)
              {
                  if (dirs_first)
                  {
                      const bool ad = a.kind == EntryKind::directory;
                      const bool bd = b.kind == EntryKind::directory;
                      if (ad != bd)
                          return ad;
                  }
                  int c = 0;
                  switch (key)
                  {
                  case SortKey::name:
                      c = natural_compare(a.name, b.name);
                      break;
                  case SortKey::size:
                      c = three_way(a.size, b.size);
                      break;
                  case SortKey::modified:
                      c = three_way(a.modified_unix, b.modified_unix);
                      break;
                  case SortKey::kind:
                      c = natural_compare(extension_of(a.name), extension_of(b.name));
                      break;
                  }
                  if (descending)
                      c = -c;
                  if (c != 0)
                      return c < 0;
                  // Ties always fall back to the name, ascending, so the order is the same each
                  // time.
                  c = natural_compare(a.name, b.name);
                  if (c != 0)
                      return c < 0;
                  return a.name < b.name;
              });
}

int PaneModel::index_of(std::string_view name) const
{
    for (std::size_t i = 0; i < state_.entries.size(); ++i)
    {
        if (state_.entries[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

bool PaneModel::list_into(const std::string &path, const std::string &focus_name, int fallback)
{
    ListResult listing = source_.list(path);
    if (!listing.ok)
    {
        state_.list_error = listing.error;
        trace("pane: list %s failed, errno %d", path.c_str(), listing.error);
        return false;
    }
    state_.list_error = 0;
    const bool same_folder = state_.path == path;
    state_.path = path;
    state_.space = source_.space(path);
    state_.changeable = !is_protected_folder(path) && source_.is_plain_path(path);
    state_.total_entries = static_cast<int>(listing.entries.size());
    sort_entries(&listing.entries);
    state_.truncated = listing.entries.size() > static_cast<std::size_t>(kPaneRowCap);
    if (state_.truncated)
        listing.entries.resize(static_cast<std::size_t>(kPaneRowCap));
    state_.entries = std::move(listing.entries);

    // A selection belongs to the folder: kept across a reload, minus what is gone.
    if (!same_folder)
    {
        state_.selected.clear();
    }
    else
    {
        std::unordered_set<std::string> present;
        for (const Entry &entry : state_.entries)
            present.insert(entry.name);
        std::vector<std::string> kept;
        for (const std::string &name : state_.selected)
        {
            if (present.count(name) != 0)
                kept.push_back(name);
        }
        state_.selected = std::move(kept);
    }

    const int found = focus_name.empty() ? -1 : index_of(focus_name);
    const int last = static_cast<int>(state_.entries.size()) - 1;
    state_.focus_index = found >= 0 ? found : std::max(0, std::min(fallback, last));
    trace("pane: %s: %d entries%s", path.c_str(), state_.total_entries,
          state_.truncated ? " (capped)" : "");
    return true;
}

bool PaneModel::go_to(const std::string &path)
{
    return list_into(normalized(path), std::string(), 0);
}

bool PaneModel::enter(const std::string &name)
{
    const int index = index_of(name);
    if (index < 0 || state_.entries[static_cast<std::size_t>(index)].kind != EntryKind::directory)
        return false;
    return go_to(join(state_.path, name));
}

bool PaneModel::go_up()
{
    if (state_.path.empty() || state_.path == "/")
        return false;
    const std::string left = state_.path.substr(state_.path.rfind('/') + 1);
    return list_into(parent_of(state_.path), left, 0);
}

void PaneModel::reload_focusing(const std::string &focus_name)
{
    if (state_.path.empty())
        return;
    std::string path = state_.path;
    std::string name = focus_name;
    for (;;)
    {
        if (list_into(path, name, state_.focus_index))
            return;
        if (path == "/")
        {
            // Not even the root can be listed: show nothing rather than something stale.
            state_.entries.clear();
            state_.selected.clear();
            state_.total_entries = 0;
            state_.truncated = false;
            state_.focus_index = 0;
            return;
        }
        path = parent_of(path); // the folder is gone: the nearest one that is not
        name.clear();
    }
}

void PaneModel::reload()
{
    const Entry *current = focused();
    reload_focusing(current != nullptr ? current->name : std::string());
}

void PaneModel::set_sort(SortKey key, bool descending)
{
    state_.sort_key = key;
    state_.sort_descending = descending;
    reload();
}

void PaneModel::set_dirs_first(bool dirs_first)
{
    state_.dirs_first = dirs_first;
    reload();
}

void PaneModel::set_focus(int index)
{
    const int last = static_cast<int>(state_.entries.size()) - 1;
    state_.focus_index = std::max(0, std::min(index, last));
}

const Entry *PaneModel::focused() const
{
    return entry_at(state_.focus_index);
}

const Entry *PaneModel::entry_at(int index) const
{
    if (index < 0 || index >= static_cast<int>(state_.entries.size()))
        return nullptr;
    return &state_.entries[static_cast<std::size_t>(index)];
}

bool PaneModel::is_selected(std::string_view name) const
{
    return std::find(state_.selected.begin(), state_.selected.end(), name) != state_.selected.end();
}

void PaneModel::toggle_selected(const std::string &name)
{
    const auto it = std::find(state_.selected.begin(), state_.selected.end(), name);
    if (it != state_.selected.end())
    {
        state_.selected.erase(it);
        return;
    }
    if (index_of(name) >= 0 && static_cast<int>(state_.selected.size()) < kPaneRowCap)
        state_.selected.push_back(name);
}

void PaneModel::select_all()
{
    state_.selected.clear();
    for (const Entry &entry : state_.entries)
        state_.selected.push_back(entry.name); // entries is capped already, so is this
}

void PaneModel::invert_selection()
{
    std::vector<std::string> inverted;
    for (const Entry &entry : state_.entries)
    {
        if (!is_selected(entry.name))
            inverted.push_back(entry.name);
    }
    state_.selected = std::move(inverted);
}

void PaneModel::clear_selection()
{
    state_.selected.clear();
}

int PaneModel::make_folder(const std::string &name)
{
    if (const int bad = check_name(name))
        return bad;
    if (!state_.changeable)
        return EPERM;
    const std::string path = join(state_.path, name);
    Entry existing;
    if (source_.stat(path, &existing))
        return EEXIST;
    const int error = source_.make_directory(path);
    trace("pane: new folder %s: %d", path.c_str(), error);
    if (error == 0)
        reload_focusing(name);
    return error;
}

int PaneModel::rename_entry(const std::string &from, const std::string &to)
{
    if (const int bad = check_name(from))
        return bad;
    if (const int bad = check_name(to))
        return bad;
    if (from == to)
        return 0;
    if (!state_.changeable)
        return EPERM;
    const std::string target = join(state_.path, to);
    Entry existing;
    if (source_.stat(target, &existing))
        return EEXIST; // never over something that is there
    const int error = source_.rename(join(state_.path, from), target);
    trace("pane: rename %s -> %s: %d", from.c_str(), to.c_str(), error);
    if (error == 0)
    {
        std::replace(state_.selected.begin(), state_.selected.end(), from, to);
        reload_focusing(to);
    }
    return error;
}

} // namespace hui::explorer
