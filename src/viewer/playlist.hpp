// EliVier Explorer - Playlist files (.m3u, .m3u8): a list of songs to play in order.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace hui::viewer
{

struct PlaylistEntry
{
    std::string path;  // absolute
    std::string title; // from #EXTINF, or empty
};

// Reads the text of an M3U file. Relative paths are taken from `base_dir`; backslashes count as
// slashes; "#" lines other than #EXTINF are comments; addresses (http://) are skipped. A UTF-8
// BOM and CRLF line ends are fine. Capped at 5000 entries.
std::vector<PlaylistEntry> parse_m3u(const std::string &text, const std::string &base_dir);

// "/data/a/../b/./c.mp3" -> "/data/b/c.mp3".
std::string normalize_path(const std::string &path);

} // namespace hui::viewer
