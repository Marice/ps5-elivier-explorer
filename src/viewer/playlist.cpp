// EliVier Explorer - Playlist files (.m3u, .m3u8): a list of songs to play in order.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/playlist.hpp"

#include <cstddef>

namespace hui::viewer
{

namespace
{

constexpr std::size_t kMaxEntries = 5000;

std::string trimmed(const std::string &s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

} // namespace

std::string normalize_path(const std::string &path)
{
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= path.size())
    {
        std::size_t end = path.find('/', at);
        if (end == std::string::npos)
            end = path.size();
        const std::string part = path.substr(at, end - at);
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!part.empty() && part != ".")
            parts.push_back(part);
        at = end + 1;
    }
    std::string out;
    for (const std::string &part : parts)
        out += "/" + part;
    return out.empty() ? "/" : out;
}

std::vector<PlaylistEntry> parse_m3u(const std::string &text, const std::string &base_dir)
{
    std::vector<PlaylistEntry> entries;
    std::string title;
    std::size_t at = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        at = 3;
    while (at < text.size() && entries.size() < kMaxEntries)
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = trimmed(text.substr(at, end - at));
        at = end + 1;
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            if (line.compare(0, 8, "#EXTINF:") == 0)
            {
                const std::size_t comma = line.find(',');
                title = comma == std::string::npos ? "" : trimmed(line.substr(comma + 1));
            }
            continue;
        }
        if (line.find("://") != std::string::npos)
        {
            title.clear();
            continue;
        }
        std::string path = line;
        for (char &c : path)
            if (c == '\\')
                c = '/';
        if (path[0] != '/')
            path = base_dir + "/" + path;
        entries.push_back({normalize_path(path), title});
        title.clear();
    }
    return entries;
}

} // namespace hui::viewer
