// EliVier Explorer - Paths as strings: join, normalize, "inside".
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Absolute paths within a Source, "/" separated, no trailing slash except the root. Header-only
// so the copy engine and the pane model share them.

#pragma once

#include <string>

namespace hui::explorer
{

inline std::string path_normalized(const std::string &path)
{
    std::string out = path;
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
    return out.empty() ? std::string("/") : out;
}

// path + "/" + name, without a double slash at the root.
inline std::string path_join(const std::string &path, const std::string &name)
{
    if (path.empty() || path.back() == '/')
        return path + name;
    return path + "/" + name;
}

// Whether `path` is `ancestor` or lies below it.
inline bool path_is_inside(const std::string &path, const std::string &ancestor)
{
    const std::string a = path_normalized(ancestor);
    const std::string p = path_normalized(path);
    if (a == "/")
        return true;
    return p == a || (p.size() > a.size() && p.compare(0, a.size(), a) == 0 && p[a.size()] == '/');
}

} // namespace hui::explorer
