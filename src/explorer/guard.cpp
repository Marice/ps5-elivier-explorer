// EliVier Explorer - Where the explorer may change things: /data and the drives, nowhere else.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/guard.hpp"

#include <algorithm>
#include <cctype>

namespace hui::explorer
{

namespace
{

// "." and ".." resolved by name, so a path cannot step out of where it starts; "" for a path that
// is not absolute. Symbolic links are not known here: see Source::is_plain_path.
std::string resolved(const std::string &path)
{
    if (path.empty() || path[0] != '/')
        return std::string();
    std::vector<std::string> parts;
    std::size_t at = 1;
    while (at <= path.size())
    {
        const std::size_t next = path.find('/', at);
        const std::string part = path.substr(at, next == std::string::npos ? next : next - at);
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        if (next == std::string::npos)
            break;
        at = next + 1;
    }
    std::string out;
    for (const std::string &part : parts)
        out += "/" + part;
    return out.empty() ? std::string("/") : out;
}

// "usb" or "ext" followed by one or more digits, and nothing else.
bool is_drive_name(const std::string &name)
{
    if (name.size() < 4 || (name.compare(0, 3, "usb") != 0 && name.compare(0, 3, "ext") != 0))
        return false;
    return std::all_of(name.begin() + 3, name.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

bool console_allows(const std::string &path)
{
    if (path_is_inside(path, "/data"))
        return path != "/";
    if (path_is_inside(path, "/mnt") && path != "/mnt")
    {
        const std::string rest = path.substr(5); // after "/mnt/"
        return is_drive_name(rest.substr(0, rest.find('/')));
    }
    return false;
}

} // namespace

bool ChangePolicy::allows(const std::string &folder) const
{
    const std::string path = resolved(folder);
    if (path.empty())
        return false;
    if (console_allows(path))
        return true;
    return std::any_of(extra_.begin(), extra_.end(),
                       [&path](const std::string &root) { return path_is_inside(path, root); });
}

void ChangePolicy::allow_also(std::string root)
{
    extra_.push_back(resolved(root));
}

void ChangePolicy::clear_extra()
{
    extra_.clear();
}

ChangePolicy &change_policy()
{
    static ChangePolicy policy;
    return policy;
}

} // namespace hui::explorer
