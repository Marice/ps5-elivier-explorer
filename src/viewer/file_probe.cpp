// EliVier Explorer - What is known about a file before a handler is chosen.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/file_probe.hpp"

#include <initializer_list>
#include <utility>

namespace hui::viewer
{

namespace
{

bool one_of(const std::string &ext, std::initializer_list<const char *> list)
{
    for (const char *item : list)
    {
        if (ext == item)
            return true;
    }
    return false;
}

std::string extension_of(const std::string &name)
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size())
        return std::string();
    std::string ext = name.substr(dot + 1);
    for (char &c : ext)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

} // namespace

IconKind icon_by_extension(const std::string &ext)
{
    if (one_of(ext, {"zip", "rar", "7z", "tar", "gz", "tgz", "xz", "bz2"}))
        return IconKind::archive;
    if (one_of(ext, {"png", "jpg", "jpeg", "bmp", "gif", "webp", "dds", "tga"}))
        return IconKind::image;
    if (one_of(ext, {"mp3", "ogg", "wav", "flac", "at9", "m4a", "opus"}))
        return IconKind::audio;
    if (one_of(ext, {"mp4", "mkv", "avi", "webm", "mov"}))
        return IconKind::video;
    if (one_of(ext, {"txt", "log", "ini", "cfg", "json", "xml", "md", "csv", "lua", "js", "html",
                     "yaml", "yml"}))
        return IconKind::text;
    if (one_of(ext, {"elf", "self", "prx", "sprx", "bin"}))
        return IconKind::program;
    if (one_of(ext, {"iso", "pkg", "ffpkg", "ffpfs", "ffpfsc", "exfat"}))
        return IconKind::disc;
    return IconKind::other;
}

FileProbe::FileProbe(std::string name, std::uint64_t size, std::function<std::string()> head_reader)
    : name_(std::move(name)), ext_(extension_of(name_)), size_(size),
      reader_(std::move(head_reader))
{
}

const std::string &FileProbe::head() const
{
    if (!loaded_)
    {
        loaded_ = true;
        if (reader_)
            head_ = reader_();
        if (head_.size() > kHeadBytes)
            head_.resize(kHeadBytes);
    }
    return head_;
}

} // namespace hui::viewer
