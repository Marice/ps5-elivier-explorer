// EliVier Explorer - Reads a ZIP's central directory: stored entries only, old-style .z01 splits
// and ZIP64 sizes understood; nothing here decompresses.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PKWARE's APPNOTE.TXT is the format: a central directory (one record per entry) near the end of
// the last volume, found through an End Of Central Directory record (and, for a large archive,
// the ZIP64 locator and record in front of it). Each central directory record points at a local
// header, which the actual bytes follow; reading one means finding its data past that header,
// since the local header repeats the name and may have its own, possibly different, extra data.

#include "explorer/archive_formats.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace hui::explorer
{

namespace
{

std::uint16_t u16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t u32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
          (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t u64(const std::uint8_t *p)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | p[static_cast<std::size_t>(i)];
    return value;
}

constexpr std::size_t kEocdFixedSize = 22;
constexpr std::size_t kZip64LocatorSize = 20;
constexpr std::size_t kZip64EocdFixedSize = 56;
constexpr std::size_t kCentralHeaderFixedSize = 46;
constexpr std::size_t kLocalHeaderFixedSize = 30;
constexpr std::uint32_t kEocdSignature = 0x06054b50;
constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50;
constexpr std::uint32_t kZip64EocdSignature = 0x06064b50;
constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::uint16_t kZip64ExtraId = 0x0001;

std::string lower(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// "name.z07" -> {"name", 7}; "name.zip" or anything else -> {"", 0}.
bool split_part_number(const std::string &lower_name, std::string *stem, int *number)
{
    const std::size_t dot = lower_name.rfind('.');
    if (dot == std::string::npos || lower_name.size() - dot != 4 ||
        lower_name.compare(dot + 1, 1, "z") != 0)
        return false;
    const std::string digits = lower_name.substr(dot + 2);
    if (digits.size() != 2 || !std::isdigit(static_cast<unsigned char>(digits[0])) ||
        !std::isdigit(static_cast<unsigned char>(digits[1])))
        return false;
    *stem = lower_name.substr(0, dot);
    *number = (digits[0] - '0') * 10 + (digits[1] - '0');
    return true;
}

// Every volume of the set, in read order (the split parts, then the .zip that holds the central
// directory), and the .zip file's own path (the last one). Empty `error` on a problem finding one.
bool find_volumes(const FormatReader &reader, std::vector<std::string> *volumes,
                  std::string *error)
{
    const std::string low = lower(reader.name);
    std::string stem = low;
    const std::size_t dot = low.rfind('.');
    if (dot != std::string::npos && low.compare(dot + 1, 3, "zip") == 0)
        stem = low.substr(0, dot);
    else
    {
        int number = 0;
        std::string part_stem;
        if (!split_part_number(low, &part_stem, &number))
            return (*error = "Not part of a ZIP set"), false;
        stem = part_stem;
    }
    const std::string join = reader.folder.empty() ? "/" : reader.folder + "/";
    for (int part = 1; part <= kMaxArchiveVolumes; ++part)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".z%02d", part);
        const std::string path = join + stem + suffix;
        Entry entry;
        if (!reader.parent.stat(path, &entry))
            break;
        volumes->push_back(path);
    }
    if (static_cast<int>(volumes->size()) >= kMaxArchiveVolumes)
        return (*error = "Too many volumes"), false;
    const std::string zip_path = join + stem + ".zip";
    Entry entry;
    if (!reader.parent.stat(zip_path, &entry))
        return (*error = "The .zip of this set was not found beside it"), false;
    volumes->push_back(zip_path);
    return true;
}

// The classic End Of Central Directory record, or its ZIP64 replacement when the classic one's
// counts are the all-ones sentinel. Either way: where the central directory starts, how many
// bytes it is, and which volume it starts on.
struct Eocd
{
    int disk = 0;
    std::uint64_t central_directory_offset = 0;
    std::uint64_t central_directory_size = 0;
    std::uint64_t entry_count = 0;
};

bool read_eocd(Source &parent, const std::string &last_volume, Eocd *eocd, std::string *error)
{
    Entry entry;
    if (!parent.stat(last_volume, &entry))
        return (*error = "Cannot read the last volume"), false;
    const std::uint64_t tail_wanted = std::min<std::uint64_t>(entry.size, kMaxEocdSearchBytes);
    const std::uint64_t tail_start = entry.size - tail_wanted;
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_wanted));
    if (!tail.empty() &&
        !read_exact(parent, last_volume, tail_start, tail.data(), tail.size()))
        return (*error = "Cannot read the end of the archive"), false;
    std::size_t eocd_at = tail.size();
    if (tail.size() >= kEocdFixedSize)
    {
        for (std::size_t i = tail.size() - kEocdFixedSize + 1; i-- > 0;)
        {
            if (u32(&tail[i]) == kEocdSignature)
            {
                eocd_at = i;
                break;
            }
        }
    }
    if (eocd_at == tail.size() || eocd_at + kEocdFixedSize > tail.size())
        return (*error = "No end-of-central-directory record found"), false;
    const std::uint8_t *e = &tail[eocd_at];
    std::uint32_t total_entries = u16(e + 10);
    std::uint64_t cd_size = u32(e + 12);
    std::uint64_t cd_offset = u32(e + 16);
    int disk = u16(e + 6); // "disk with the start of the central directory", not "disk of this record"
    const bool need_zip64 = u16(e + 10) == 0xFFFF || cd_size == 0xFFFFFFFF ||
                            cd_offset == 0xFFFFFFFF || u16(e + 6) == 0xFFFF;
    if (need_zip64)
    {
        if (eocd_at < kZip64LocatorSize)
            return (*error = "A ZIP64 archive with no locator record"), false;
        const std::uint8_t *locator = &tail[eocd_at - kZip64LocatorSize];
        if (u32(locator) != kZip64LocatorSignature)
            return (*error = "A ZIP64 archive with no locator record"), false;
        const std::uint64_t zip64_offset = u64(locator + 8);
        std::uint8_t record[kZip64EocdFixedSize];
        if (!read_exact(parent, last_volume, zip64_offset, record, sizeof(record)) ||
            u32(record) != kZip64EocdSignature)
            return (*error = "A damaged ZIP64 end-of-central-directory record"), false;
        disk = static_cast<int>(u32(record + 20)); // "disk with CD start", not "disk of this record"
        total_entries = 0; // not used below past this point; zip64's own count is 64-bit
        cd_offset = u64(record + 48);
        cd_size = u64(record + 40);
        eocd->entry_count = u64(record + 32);
    }
    else
    {
        eocd->entry_count = total_entries;
    }
    eocd->disk = disk;
    eocd->central_directory_offset = cd_offset;
    eocd->central_directory_size = cd_size;
    if (eocd->central_directory_size > kMaxCentralDirectoryBytes)
        return (*error = "The central directory is implausibly large"), false;
    return true;
}

// The ZIP64 extra field, if present, replaces whichever of these fields the main record marked
// with the all-ones sentinel, in this fixed order; only the marked ones are actually present.
void apply_zip64_extra(const std::uint8_t *extra, std::uint16_t extra_len,
                       std::uint64_t *uncompressed_size, std::uint64_t *compressed_size,
                       std::uint64_t *local_offset, std::uint32_t *disk)
{
    std::size_t at = 0;
    while (at + 4 <= extra_len)
    {
        const std::uint16_t id = u16(extra + at);
        const std::uint16_t size = u16(extra + at + 2);
        if (at + 4 + size > extra_len)
            return;
        if (id == kZip64ExtraId)
        {
            std::size_t field = at + 4;
            const std::size_t end = field + size;
            if (*uncompressed_size == 0xFFFFFFFF && field + 8 <= end)
            {
                *uncompressed_size = u64(extra + field);
                field += 8;
            }
            if (*compressed_size == 0xFFFFFFFF && field + 8 <= end)
            {
                *compressed_size = u64(extra + field);
                field += 8;
            }
            if (*local_offset == 0xFFFFFFFF && field + 8 <= end)
            {
                *local_offset = u64(extra + field);
                field += 8;
            }
            if (*disk == 0xFFFF && field + 4 <= end)
                *disk = u32(extra + field);
            return;
        }
        at += 4u + size;
    }
}

} // namespace

bool read_zip(const FormatReader &reader, ArchiveBuild &build, std::string *error)
{
    std::vector<std::string> volumes;
    if (!find_volumes(reader, &volumes, error))
        return false;
    build.volumes = volumes;
    const std::string &last_volume = volumes.back();
    Eocd eocd;
    if (!read_eocd(reader.parent, last_volume, &eocd, error))
        return false;
    std::vector<std::uint64_t> volume_sizes(volumes.size());
    for (std::size_t i = 0; i < volumes.size(); ++i)
    {
        Entry entry;
        if (!reader.parent.stat(volumes[i], &entry))
            return (*error = "A volume of this set went missing"), false;
        volume_sizes[i] = entry.size;
    }
    if (eocd.disk < 0 || static_cast<std::size_t>(eocd.disk) >= volumes.size())
        return (*error = "The central directory's disk number is out of range"), false;
    std::vector<std::uint8_t> central(static_cast<std::size_t>(eocd.central_directory_size));
    if (!central.empty() &&
        !read_exact(reader.parent, volumes[static_cast<std::size_t>(eocd.disk)],
                   eocd.central_directory_offset, central.data(), central.size()))
        return (*error = "Cannot read the central directory"), false;

    std::size_t at = 0;
    int entries_read = 0;
    while (at + kCentralHeaderFixedSize <= central.size())
    {
        if (entries_read >= kMaxArchiveEntries)
            return (*error = "Too many entries in this archive"), false;
        const std::uint8_t *h = &central[at];
        if (u32(h) != kCentralSignature)
            break; // the directory ends (or is damaged); what came before still stands
        const std::uint16_t flags = u16(h + 8);
        const std::uint16_t method = u16(h + 10);
        std::uint64_t compressed_size = u32(h + 20);
        std::uint64_t uncompressed_size = u32(h + 24);
        const std::uint16_t name_len = u16(h + 28);
        const std::uint16_t extra_len = u16(h + 30);
        const std::uint16_t comment_len = u16(h + 32);
        std::uint32_t disk = u16(h + 34);
        const std::uint32_t external_attrs = u32(h + 38);
        std::uint64_t local_offset = u32(h + 42);
        const std::size_t record_size = kCentralHeaderFixedSize + name_len + extra_len + comment_len;
        if (at + record_size > central.size())
            break; // a truncated last record: stop, do not guess at it
        const std::string name(reinterpret_cast<const char *>(h + kCentralHeaderFixedSize),
                               name_len);
        apply_zip64_extra(h + kCentralHeaderFixedSize + name_len, extra_len, &uncompressed_size,
                          &compressed_size, &local_offset, &disk);
        at += record_size;
        ++entries_read;
        if (name.empty())
            continue;
        const bool is_folder = !name.empty() && name.back() == '/';
        // Bit 3 of general-purpose flags (a "data descriptor" follows the data instead of sizes
        // being known up front) is common for streamed entries; without a size here there is
        // nothing safe to read, so such an entry is left out, counted as unreadable.
        const bool streamed_sizes = (flags & 0x0008) != 0;
        ArchiveNode node;
        node.is_folder = is_folder;
        node.size = uncompressed_size;
        if (is_folder || method != 0 || streamed_sizes || (flags & 0x0001) != 0 /* encrypted */)
        {
            if (!is_folder)
                ++build.unreadable;
            if (is_folder)
                archive_add(build, name, std::move(node));
            continue;
        }
        if (disk >= volumes.size())
        {
            ++build.unreadable;
            continue;
        }
        // The local header repeats the name (and may carry its own extra data of a different
        // length), so the data starts this many bytes after it, not right after the fixed size.
        std::uint8_t local[kLocalHeaderFixedSize];
        if (!read_exact(reader.parent, volumes[disk], local_offset, local, sizeof(local)) ||
            u32(local) != kLocalSignature)
        {
            ++build.unreadable;
            continue;
        }
        const std::uint16_t local_name_len = u16(local + 26);
        const std::uint16_t local_extra_len = u16(local + 28);
        const std::uint64_t data_offset =
            local_offset + kLocalHeaderFixedSize + local_name_len + local_extra_len;
        node.segments =
            span_volumes(static_cast<int>(disk), data_offset, uncompressed_size, volume_sizes);
        if (node.segments.empty() && uncompressed_size > 0)
        {
            ++build.unreadable;
            continue;
        }
        node.stored = true;
        (void)external_attrs; // MS-DOS/Unix external attributes are not needed: "/" already says folder
        archive_add(build, name, std::move(node));
    }
    return true;
}

} // namespace hui::explorer
