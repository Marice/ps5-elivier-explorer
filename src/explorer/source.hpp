// EliVier Explorer - Where files come from: the interface a pane lists and copies through.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A pane never touches the file system itself: it asks a Source. LocalSource is the console's own
// drives (plain POSIX); an SMB or FTP source later implements the same calls. Walking a folder
// tree is the engine's job (copy_job), built from these primitives, so a new source stays small.
// Paths are absolute within the source. Calls return 0 or an errno, never throw (no exceptions
// on the console).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hui::explorer
{

enum class EntryKind : std::uint8_t
{
    file,
    directory,
    other, // a device, socket, broken link ...
};

struct Entry
{
    std::string name; // the last part only, no path
    EntryKind kind = EntryKind::file;
    std::uint64_t size = 0; // 0 for directories
    std::int64_t modified_unix = 0;
};

struct ListResult
{
    bool ok = false;
    int error = 0; // errno when !ok
    std::vector<Entry> entries;
};

// Free space of the drive a path is on. known is false when the source cannot tell, or when the
// drive is a placeholder mount point of a few megabytes (an empty /mnt/usb3).
struct SpaceInfo
{
    bool known = false;
    std::uint64_t free_bytes = 0;
    std::uint64_t total_bytes = 0;
};

// What a copy does when the name is already at the destination.
enum class ConflictPolicy : std::uint8_t
{
    ask,
    skip,
    overwrite,
    rename,
};

class Source
{
  public:
    virtual ~Source() = default;

    // "Internal (/data)": what the pane header calls this source.
    virtual std::string display_name() const = 0;

    virtual ListResult list(const std::string &path) const = 0;
    virtual bool stat(const std::string &path, Entry *out) const = 0;
    virtual SpaceInfo space(const std::string &path) const = 0;

    // Reads up to `size` bytes at `offset` without reading the rest of the file: the one primitive
    // the viewers read through, so a 23 GB file opens at once. Returns the bytes read (fewer than
    // asked at the end of the file, 0 past it) or -errno. Short reads are not repeated for the
    // caller.
    virtual std::int64_t read_range(const std::string &path, std::uint64_t offset, void *buffer,
                                    std::size_t size) const = 0;

    virtual int make_directory(const std::string &path) = 0;
    virtual int rename(const std::string &from, const std::string &to) = 0;
    virtual int remove_file(const std::string &path) = 0;
    // Empty folders only: emptying one first is the engine's job.
    virtual int remove_directory(const std::string &path) = 0;

    // Copies one file, `chunk` bytes at a time, so memory use stays small whatever the size.
    // on_progress gets the bytes written so far after every chunk; returning false stops the copy
    // and removes the partial destination (the result is ECANCELED).
    using ProgressFn = std::function<bool(std::uint64_t copied)>;
    virtual int copy_file(const std::string &from, const std::string &to, std::size_t chunk,
                          const ProgressFn &on_progress) = 0;

    // Whether two paths are on the same device, so a rename between them moves instead of
    // copying. False when either cannot be told (a missing path, a source without devices).
    virtual bool same_device(const std::string &a, const std::string &b) const = 0;

    // Whether both are the same kind of source. Rename may do for a move only then, and only on
    // the same device as well (the engine checks that): two local drives are one kind of source.
    // No RTTI on the console, so each kind hands out the address of its own static tag.
    virtual const void *kind_tag() const = 0;
    bool same_kind(const Source &other) const
    {
        return kind_tag() == other.kind_tag();
    }
};

} // namespace hui::explorer
