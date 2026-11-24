// EliVier Explorer - Reads a RAR archive's index: stored entries only, RAR4 and RAR5, old
// .rar/.r00/.r01 and new .partNN.rar splits; nothing here decompresses.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// RAR4's block layout and RAR5's header layout are both documented at rarlab.com/technote.htm
// (RAR5 only; RAR4's exact byte offsets below were cross-checked against the LGPL-3.0 unarr
// project's rar/rar.h and rar/parse-rar.c, read as a reference, nothing copied from it). RAR5
// headers are built from "vint": a little-endian base-128 varint, 7 data bits per byte, the
// 8th bit set on every byte but the last.

#include "explorer/archive_formats.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

std::string lower(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// Days since 1970-01-01 for a given civil date. Howard Hinnant's days_from_civil, the companion
// to utc_from_time's civil_from_days (video/utc_time.cpp): pure arithmetic, no libc calendar call
// (the PS5 build's import whitelist has no timegm/mktime).
std::int64_t days_from_civil(std::int64_t year, int month, int day)
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t year_of_era = year - era * 400;
    const std::int64_t day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

// The standard MS-DOS packed date/time (same layout ZIP uses): seconds are only to 2s precision.
std::int64_t dos_date_to_unix(std::uint32_t dosdate)
{
    const int second = static_cast<int>((dosdate & 0x1f) * 2);
    const int minute = static_cast<int>((dosdate >> 5) & 0x3f);
    const int hour = static_cast<int>((dosdate >> 11) & 0x1f);
    const int day = static_cast<int>((dosdate >> 16) & 0x1f);
    const int month = static_cast<int>((dosdate >> 21) & 0x0f);
    const std::int64_t year = static_cast<std::int64_t>((dosdate >> 25) & 0x7f) + 1980;
    const std::int64_t days = days_from_civil(year, month > 0 ? month : 1, day > 0 ? day : 1);
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

constexpr std::size_t kRar4SignatureSize = 7;
constexpr std::size_t kRar5SignatureSize = 8;
constexpr std::size_t kRar4BlockHeaderSize = 7; // CRC(2) + type(1) + flags(2) + size(2)
constexpr std::uint8_t kRar4TypeMain = 0x73;
constexpr std::uint8_t kRar4TypeFile = 0x74;
constexpr std::uint16_t kRar4LhdLongBlock = 1 << 15;
constexpr std::uint16_t kRar4LhdSplitBefore = 1 << 0;
constexpr std::uint16_t kRar4LhdPassword = 1 << 2;
constexpr std::uint16_t kRar4LhdDirectory = (1 << 5) | (1 << 6) | (1 << 7); // all three bits set
constexpr std::uint16_t kRar4LhdLarge = 1 << 8;
constexpr std::uint8_t kRar4MethodStore = 0x30;

constexpr std::uint64_t kRar5HeaderTypeMain = 1;
constexpr std::uint64_t kRar5HeaderTypeFile = 2;
constexpr std::uint64_t kRar5HeaderFlagExtra = 0x0001;
constexpr std::uint64_t kRar5HeaderFlagData = 0x0002;
constexpr std::uint64_t kRar5FileFlagDirectory = 0x0001;
constexpr std::uint64_t kRar5FileFlagUnixTime = 0x0002;

// "name.part007.rar" -> {"name", "part", 3, 7}; anything else -> false. Reports the digit width
// (3 here) so every volume of the set is named with the same width the first one used.
bool split_part_number(const std::string &lower_name, std::string *stem, int *digits, int *number)
{
    const std::size_t dot_rar = lower_name.rfind(".rar");
    if (dot_rar == std::string::npos || dot_rar + 4 != lower_name.size())
        return false;
    const std::size_t part_at = lower_name.rfind(".part", dot_rar);
    if (part_at == std::string::npos)
        return false;
    const std::string digits_text = lower_name.substr(part_at + 5, dot_rar - (part_at + 5));
    if (digits_text.empty() || digits_text.size() > 9)
        return false;
    for (char c : digits_text)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    *stem = lower_name.substr(0, part_at);
    *digits = static_cast<int>(digits_text.size());
    *number = std::atoi(digits_text.c_str());
    return true;
}

// "name.r07" -> {"name", 7}; "name.rar" or anything else -> false.
bool split_old_number(const std::string &lower_name, std::string *stem, int *number)
{
    const std::size_t dot = lower_name.rfind('.');
    if (dot == std::string::npos || lower_name.size() - dot != 4 ||
        lower_name.compare(dot + 1, 1, "r") != 0)
        return false;
    const std::string digits = lower_name.substr(dot + 2);
    if (digits.size() != 2 || !std::isdigit(static_cast<unsigned char>(digits[0])) ||
        !std::isdigit(static_cast<unsigned char>(digits[1])))
        return false;
    *stem = lower_name.substr(0, dot);
    *number = (digits[0] - '0') * 10 + (digits[1] - '0');
    return true;
}

// Every volume of the set, in read order. New-style (.partNNN.rar) and old-style (.rar, then
// .r00, .r01, ...) are both found from any one member, by trying every naming scheme the given
// file could be part of and keeping whichever one actually has files on disk beside it.
bool find_volumes(const FormatReader &reader, std::vector<std::string> *volumes,
                  std::string *error)
{
    const std::string low = lower(reader.name);
    const std::string join = reader.folder.empty() ? "/" : reader.folder + "/";

    std::string stem;
    int digits = 0;
    int first_number = 0;
    if (split_part_number(low, &stem, &digits, &first_number))
    {
        for (int part = 1; part <= kMaxArchiveVolumes; ++part)
        {
            char number_text[16];
            std::snprintf(number_text, sizeof(number_text), "%0*d", digits, part);
            const std::string path = join + stem + ".part" + number_text + ".rar";
            Entry entry;
            if (!reader.parent.stat(path, &entry))
                break;
            volumes->push_back(path);
        }
        if (volumes->empty())
            return (*error = "No volumes found for this new-style RAR set"), false;
        if (static_cast<int>(volumes->size()) >= kMaxArchiveVolumes)
            return (*error = "Too many volumes"), false;
        return true;
    }

    // Old-style: the plain .rar is always first, then .r00, .r01, ...
    std::string old_stem = low;
    const std::size_t dot = low.rfind(".rar");
    if (dot != std::string::npos && dot + 4 == low.size())
        old_stem = low.substr(0, dot);
    else
    {
        int number = 0;
        if (!split_old_number(low, &old_stem, &number))
            return (*error = "Not part of a RAR set"), false;
    }
    const std::string rar_path = join + old_stem + ".rar";
    Entry entry;
    if (!reader.parent.stat(rar_path, &entry))
        return (*error = "The .rar of this set was not found beside it"), false;
    volumes->push_back(rar_path);
    for (int part = 0; part <= kMaxArchiveVolumes; ++part)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".r%02d", part);
        const std::string path = join + old_stem + suffix;
        Entry part_entry;
        if (!reader.parent.stat(path, &part_entry))
            break;
        volumes->push_back(path);
    }
    if (static_cast<int>(volumes->size()) >= kMaxArchiveVolumes)
        return (*error = "Too many volumes"), false;
    return true;
}

// One RAR4 block spanning possibly several volumes: the file's data follows its own header, in
// the next volume(s) if LHD_SPLIT_AFTER is set. Every volume of a RAR set, old-style or new-style
// alike, carries its own signature and its own Main Archive Header first (RAR5's own Main Archive
// Header documents a "volume number" field present "in all volumes except first", meaning every
// later volume still has one of its own); a continued entry's data resumes right after that, not
// at byte 0 of the file.
struct VolumeCursor
{
    int volume = 0;
    std::uint64_t offset = 0;
};

bool read_at(Source &parent, const std::vector<std::string> &volumes, const VolumeCursor &at,
            void *buffer, std::size_t size)
{
    if (at.volume < 0 || static_cast<std::size_t>(at.volume) >= volumes.size())
        return false;
    return read_exact(parent, volumes[static_cast<std::size_t>(at.volume)], at.offset, buffer, size);
}

// A RAR5 "vint": base-128, little-endian, continuation bit (0x80) set on every byte but the
// last. Returns the number of bytes consumed, or 0 on a malformed or out-of-bounds value.
std::size_t read_vint(const std::uint8_t *data, std::size_t available, std::uint64_t *value)
{
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 10 && i < available; ++i)
    {
        result |= static_cast<std::uint64_t>(data[i] & 0x7f) << (7 * i);
        if ((data[i] & 0x80) == 0)
        {
            *value = result;
            return i + 1;
        }
    }
    return 0;
}

// Where volume `index`'s own entries start: past its signature and its own Main Archive Header
// (every volume of a set repeats one). `is_rar5` picks the header shape to skip.
bool rar_volume_preamble(Source &parent, const std::string &path, bool is_rar5,
                         std::uint64_t *preamble)
{
    if (!is_rar5)
    {
        std::uint8_t header[kRar4BlockHeaderSize];
        if (!read_exact(parent, path, kRar4SignatureSize, header, sizeof(header)))
            return false;
        *preamble = kRar4SignatureSize + u16(header + 5); // + this header's own block size
        return true;
    }
    std::uint8_t probe[32];
    if (!read_exact(parent, path, kRar5SignatureSize, probe, sizeof(probe)))
        return false;
    std::size_t cursor = 4; // header CRC32
    std::uint64_t header_size = 0;
    std::size_t used = read_vint(probe + cursor, sizeof(probe) - cursor, &header_size);
    if (used == 0)
        return false;
    cursor += used;
    *preamble = kRar5SignatureSize + cursor + header_size;
    return true;
}

// Like span_volumes, but a RAR set's later volumes each start their own entries after their own
// signature and Main Archive Header, not at byte 0 (unlike ZIP's old-style .z01/.z02 splits).
std::vector<ArchiveSegment> span_rar_volumes(int start_volume, std::uint64_t start_offset,
                                             std::uint64_t length,
                                             const std::vector<std::uint64_t> &volume_sizes,
                                             const std::vector<std::uint64_t> &volume_preambles)
{
    std::vector<ArchiveSegment> segments;
    int volume = start_volume;
    std::uint64_t offset = start_offset;
    std::uint64_t left = length;
    while (left > 0)
    {
        if (volume < 0 || static_cast<std::size_t>(volume) >= volume_sizes.size())
            return {};
        const std::uint64_t size_here = volume_sizes[static_cast<std::size_t>(volume)];
        if (offset >= size_here)
            return {};
        const std::uint64_t here = std::min(left, size_here - offset);
        segments.push_back({volume, offset, here});
        left -= here;
        ++volume;
        offset = static_cast<std::size_t>(volume) < volume_preambles.size()
                    ? volume_preambles[static_cast<std::size_t>(volume)]
                    : 0;
    }
    return segments;
}

bool read_rar4(const FormatReader &reader, const std::vector<std::string> &volumes,
              const std::vector<std::uint64_t> &volume_sizes, ArchiveBuild &build,
              std::string *error)
{
    std::vector<std::uint64_t> volume_preambles(volumes.size(), 0);
    volume_preambles[0] = kRar4SignatureSize;
    for (std::size_t i = 1; i < volumes.size(); ++i)
        if (!rar_volume_preamble(reader.parent, volumes[i], false, &volume_preambles[i]))
            return (*error = "A damaged volume in this RAR4 set"), false;

    VolumeCursor at{0, kRar4SignatureSize};
    int entries_read = 0;
    while (true)
    {
        if (at.volume >= static_cast<int>(volumes.size()))
            break;
        if (at.offset >= volume_sizes[static_cast<std::size_t>(at.volume)])
        {
            // The marker/main header for this volume is read once, from volume 0, at the start;
            // a later volume's own copy of it is not re-read, since every volume of an old-style
            // RAR4 set repeats it before its own entries resume.
            break;
        }
        std::uint8_t header[kRar4BlockHeaderSize];
        if (!read_at(reader.parent, volumes, at, header, sizeof(header)))
            break;
        const std::uint16_t flags = u16(header + 3);
        const std::uint16_t block_size = u16(header + 5);
        const std::uint8_t type = header[2];
        if (block_size < kRar4BlockHeaderSize)
            return (*error = "A RAR4 block with an impossible size"), false;
        std::uint64_t data_size = 0;
        std::size_t extra = 0;
        if ((flags & kRar4LhdLongBlock) != 0 || type == kRar4TypeFile)
        {
            std::uint8_t size_field[4];
            if (!read_at(reader.parent,
                        volumes, {at.volume, at.offset + kRar4BlockHeaderSize}, size_field,
                        sizeof(size_field)))
                return (*error = "A truncated RAR4 block header"), false;
            data_size = u32(size_field);
            extra = sizeof(size_field);
        }
        if (type == kRar4TypeMain)
        {
            at.offset += block_size + data_size;
            continue;
        }
        if (type != kRar4TypeFile)
        {
            at.offset += block_size + data_size;
            ++entries_read;
            if (entries_read > kMaxArchiveEntries)
                return (*error = "Too many entries in this archive"), false;
            continue;
        }
        // FILE_HEAD: size(4) os(1) crc(4) dosdate(4) version(1) method(1) namelen(2) attrs(4).
        std::uint8_t entry_fixed[21];
        if (!read_at(reader.parent, volumes, {at.volume, at.offset + kRar4BlockHeaderSize + extra},
                    entry_fixed, sizeof(entry_fixed)))
            return (*error = "A truncated RAR4 file header"), false;
        std::uint64_t unpacked_size = u32(entry_fixed + 0);
        const std::uint8_t method = entry_fixed[14];
        const std::uint16_t name_len = u16(entry_fixed + 15);
        const std::uint32_t dosdate = u32(entry_fixed + 9);
        std::size_t fixed_total = sizeof(entry_fixed);
        if ((flags & kRar4LhdLarge) != 0)
        {
            std::uint8_t large_fields[8];
            if (!read_at(reader.parent, volumes,
                        {at.volume, at.offset + kRar4BlockHeaderSize + extra + fixed_total},
                        large_fields, sizeof(large_fields)))
                return (*error = "A truncated RAR4 large-file header"), false;
            data_size += static_cast<std::uint64_t>(u32(large_fields + 0)) << 32;
            unpacked_size += static_cast<std::uint64_t>(u32(large_fields + 4)) << 32;
            fixed_total += sizeof(large_fields);
        }
        const std::uint64_t name_offset = at.offset + kRar4BlockHeaderSize + extra + fixed_total;
        std::string name(name_len, '\0');
        if (name_len > 0 &&
            !read_at(reader.parent, volumes, {at.volume, name_offset}, name.data(), name_len))
            return (*error = "A truncated RAR4 file name"), false;
        for (char &c : name)
            if (c == '\\')
                c = '/';

        const VolumeCursor next_header{at.volume, at.offset + block_size + data_size};
        ++entries_read;
        if (entries_read > kMaxArchiveEntries)
            return (*error = "Too many entries in this archive"), false;
        if (!name.empty())
        {
            const bool is_directory = (flags & kRar4LhdDirectory) == kRar4LhdDirectory;
            ArchiveNode node;
            node.is_folder = is_directory;
            node.size = unpacked_size;
            node.modified_unix = dos_date_to_unix(dosdate);
            const bool split_before = (flags & kRar4LhdSplitBefore) != 0;
            if (is_directory)
                archive_add(build, name, std::move(node));
            else if (method != kRar4MethodStore || (flags & kRar4LhdPassword) != 0 || split_before)
                ++build.unreadable; // compressed, encrypted, or this is a later part of the file
            else
            {
                // data_size is only this header's own chunk (what fits in the current volume); a
                // stored entry split across volumes (LHD_SPLIT_AFTER) has its full length in
                // unpacked_size instead, which span_rar_volumes needs to follow it past this one.
                const VolumeCursor data_at{at.volume, at.offset + block_size};
                node.segments = span_rar_volumes(data_at.volume, data_at.offset, unpacked_size,
                                                 volume_sizes, volume_preambles);
                if (node.segments.empty() && unpacked_size > 0)
                    ++build.unreadable;
                else
                {
                    node.stored = true;
                    archive_add(build, name, std::move(node));
                }
            }
        }
        at = next_header;
    }
    return true;
}

bool read_rar5(const FormatReader &reader, const std::vector<std::string> &volumes,
              const std::vector<std::uint64_t> &volume_sizes, ArchiveBuild &build,
              std::string *error)
{
    std::vector<std::uint64_t> volume_preambles(volumes.size(), 0);
    volume_preambles[0] = kRar5SignatureSize;
    for (std::size_t i = 1; i < volumes.size(); ++i)
        if (!rar_volume_preamble(reader.parent, volumes[i], true, &volume_preambles[i]))
            return (*error = "A damaged volume in this RAR5 set"), false;

    VolumeCursor at{0, kRar5SignatureSize};
    int entries_read = 0;
    // A block's maximum size is documented as fitting in a 3-byte vint (2 MB); read generously.
    constexpr std::size_t kMaxHeaderBytes = 2u * 1024u * 1024u + 64u;
    while (true)
    {
        if (at.volume >= static_cast<int>(volumes.size()))
            break;
        if (at.offset >= volume_sizes[static_cast<std::size_t>(at.volume)])
            break;
        const std::uint64_t remaining = volume_sizes[static_cast<std::size_t>(at.volume)] - at.offset;
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kMaxHeaderBytes));
        std::vector<std::uint8_t> buffer(want);
        if (want < 7 || !read_at(reader.parent, volumes, at, buffer.data(), buffer.size()))
            break;
        // Header CRC32 (4 bytes), then Header size as a vint, counted from "Header type" onward.
        std::size_t cursor = 4;
        std::uint64_t header_size = 0;
        std::size_t used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &header_size);
        if (used == 0)
            return (*error = "A truncated RAR5 header"), false;
        cursor += used;
        const std::size_t header_fields_start = cursor;
        if (header_fields_start + header_size > buffer.size())
            return (*error = "A RAR5 header longer than what was read"), false;

        std::uint64_t header_type = 0;
        used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &header_type);
        if (used == 0)
            return (*error = "A truncated RAR5 header type"), false;
        cursor += used;

        std::uint64_t header_flags = 0;
        used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &header_flags);
        if (used == 0)
            return (*error = "A truncated RAR5 header flags"), false;
        cursor += used;

        std::uint64_t extra_area_size = 0;
        if ((header_flags & kRar5HeaderFlagExtra) != 0)
        {
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &extra_area_size);
            if (used == 0)
                return (*error = "A truncated RAR5 extra-area size"), false;
            cursor += used;
        }
        std::uint64_t data_size = 0;
        if ((header_flags & kRar5HeaderFlagData) != 0)
        {
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &data_size);
            if (used == 0)
                return (*error = "A truncated RAR5 data size"), false;
            cursor += used;
        }

        const std::uint64_t header_end = header_fields_start + header_size;
        // header_end already covers header type/flags/extra-area/data-size (header_size is
        // defined as "from header type onward"); the data area itself, if any, follows right
        // after it, never inside it.
        const VolumeCursor next_header{at.volume, at.offset + header_end + data_size};

        if (header_type == kRar5HeaderTypeFile)
        {
            std::uint64_t file_flags = 0;
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &file_flags);
            if (used == 0)
                return (*error = "A truncated RAR5 file flags"), false;
            cursor += used;
            std::uint64_t unpacked_size = 0;
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &unpacked_size);
            if (used == 0)
                return (*error = "A truncated RAR5 unpacked size"), false;
            cursor += used;
            std::uint64_t attributes = 0;
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &attributes);
            if (used == 0)
                return (*error = "A truncated RAR5 attributes field"), false;
            cursor += used;
            std::int64_t modified_unix = 0;
            if ((file_flags & kRar5FileFlagUnixTime) != 0)
            {
                if (cursor + 4 > buffer.size())
                    return (*error = "A truncated RAR5 mtime field"), false;
                modified_unix = static_cast<std::int64_t>(u32(buffer.data() + cursor));
                cursor += 4;
            }
            if ((file_flags & 0x0004) != 0) // data CRC32 present
                cursor += 4;
            std::uint64_t compression_info = 0;
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &compression_info);
            if (used == 0)
                return (*error = "A truncated RAR5 compression-info field"), false;
            cursor += used;
            std::uint64_t host_os = 0;
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &host_os);
            if (used == 0)
                return (*error = "A truncated RAR5 host-OS field"), false;
            cursor += used;
            std::uint64_t name_len = 0;
            used = read_vint(buffer.data() + cursor, buffer.size() - cursor, &name_len);
            if (used == 0)
                return (*error = "A truncated RAR5 name-length field"), false;
            cursor += used;
            if (cursor + name_len > buffer.size() || cursor + name_len > header_end)
                return (*error = "A truncated RAR5 file name"), false;
            std::string name(reinterpret_cast<const char *>(buffer.data() + cursor),
                             static_cast<std::size_t>(name_len));
            cursor += static_cast<std::size_t>(name_len);

            ++entries_read;
            if (entries_read > kMaxArchiveEntries)
                return (*error = "Too many entries in this archive"), false;
            if (!name.empty())
            {
                const bool is_directory = (file_flags & kRar5FileFlagDirectory) != 0;
                const unsigned method = static_cast<unsigned>((compression_info >> 7) & 0x7);
                const bool continues_from_before = (header_flags & 0x0008) != 0; // data from a prior volume
                ArchiveNode node;
                node.is_folder = is_directory;
                node.size = unpacked_size;
                node.modified_unix = modified_unix;
                if (is_directory)
                    archive_add(build, name, std::move(node));
                else if (method != 0 || continues_from_before)
                    ++build.unreadable; // compressed, or a later part of a file split before here
                else
                {
                    // data_size is only this header's own data area (this volume's share); a
                    // stored entry split across volumes has its full length in unpacked_size.
                    const std::uint64_t data_offset = at.offset + header_end;
                    node.segments = span_rar_volumes(at.volume, data_offset, unpacked_size,
                                                     volume_sizes, volume_preambles);
                    if (node.segments.empty() && data_size > 0)
                        ++build.unreadable;
                    else
                    {
                        node.stored = true;
                        archive_add(build, name, std::move(node));
                    }
                }
            }
        }
        else if (header_type != kRar5HeaderTypeMain)
        {
            ++entries_read;
            if (entries_read > kMaxArchiveEntries)
                return (*error = "Too many entries in this archive"), false;
        }
        at = next_header;
    }
    return true;
}

} // namespace

bool read_rar(const FormatReader &reader, ArchiveBuild &build, std::string *error)
{
    std::vector<std::string> volumes;
    if (!find_volumes(reader, &volumes, error))
        return false;
    build.volumes = volumes;
    std::vector<std::uint64_t> volume_sizes(volumes.size());
    for (std::size_t i = 0; i < volumes.size(); ++i)
    {
        Entry entry;
        if (!reader.parent.stat(volumes[i], &entry))
            return (*error = "A volume of this set went missing"), false;
        volume_sizes[i] = entry.size;
    }
    unsigned char head[kRar5SignatureSize] = {};
    const std::int64_t got = reader.parent.read_range(volumes.front(), 0, head, sizeof(head));
    if (got < static_cast<std::int64_t>(kRar4SignatureSize))
        return (*error = "Cannot read this volume's signature"), false;
    if (got >= static_cast<std::int64_t>(kRar5SignatureSize) &&
        std::memcmp(head, "Rar!\x1a\x07\x01\x00", kRar5SignatureSize) == 0)
        return read_rar5(reader, volumes, volume_sizes, build, error);
    if (std::memcmp(head, "Rar!\x1a\x07\x00", kRar4SignatureSize) == 0)
        return read_rar4(reader, volumes, volume_sizes, build, error);
    return (*error = "Not a recognised RAR signature"), false;
}

} // namespace hui::explorer
