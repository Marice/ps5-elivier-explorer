// EliVier Explorer - The console's own drives (and the PC's, in tests): plain POSIX.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the console taught (see probe.cpp): statvfs(path) kills a title, so free space is
// fstatvfs on a folder opened for it; dirfd() is declared but not linked, so that is a second
// open and not the listing's own; readdir's d_type is not relied on, every entry is stat'ed.

#include "explorer/local_source.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace hui::explorer
{

namespace
{

const char kKindTag = 0; // the address is the tag

// A drive with no media in it shows a few megabytes of the parent's: unknown, not "nearly full".
constexpr std::uint64_t kPlaceholderBytes = 16ull * 1024 * 1024;

EntryKind kind_of(mode_t mode)
{
    if (S_ISDIR(mode))
        return EntryKind::directory;
    if (S_ISREG(mode))
        return EntryKind::file;
    return EntryKind::other;
}

void fill(Entry *entry, const struct stat &info)
{
    entry->kind = kind_of(info.st_mode);
    entry->size = entry->kind == EntryKind::directory ? 0 : static_cast<std::uint64_t>(info.st_size);
    entry->modified_unix = static_cast<std::int64_t>(info.st_mtime);
}

std::string leaf_of(const std::string &path)
{
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/')
        --end;
    const std::size_t slash = path.rfind('/', end - 1);
    if (slash == std::string::npos)
        return path.substr(0, end);
    return path.substr(slash + 1, end - slash - 1);
}

bool write_all(int fd, const char *data, std::size_t size)
{
    while (size > 0)
    {
        const ssize_t n = ::write(fd, data, size);
        if (n <= 0)
            return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

std::string LocalSource::display_name() const
{
    return "Local";
}

ListResult LocalSource::list(const std::string &path) const
{
    ListResult result;
    DIR *dir = ::opendir(path.c_str());
    if (dir == nullptr)
    {
        result.error = errno;
        return result;
    }
    std::string base = path;
    if (base.empty() || base.back() != '/')
        base += '/';
    for (;;)
    {
        // readdir leaves errno alone at the end of a folder and sets it on a failure; a stat that
        // failed on one entry must not read as the folder's own error.
        errno = 0;
        const dirent *item = ::readdir(dir);
        if (item == nullptr)
            break;
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
            continue;
        Entry entry;
        entry.name = item->d_name;
        struct stat info
        {
        };
        if (::stat((base + entry.name).c_str(), &info) != 0 &&
            ::lstat((base + entry.name).c_str(), &info) != 0)
            entry.kind = EntryKind::other; // vanished or unreadable: listed, but not touched
        else
            fill(&entry, info);
        result.entries.push_back(std::move(entry));
    }
    result.error = errno;
    ::closedir(dir);
    result.ok = result.error == 0;
    return result;
}

bool LocalSource::stat(const std::string &path, Entry *out) const
{
    struct stat info
    {
    };
    if (::stat(path.c_str(), &info) != 0)
        return false;
    out->name = leaf_of(path);
    fill(out, info);
    return true;
}

SpaceInfo LocalSource::space(const std::string &path) const
{
    SpaceInfo space;
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return space;
    struct statvfs info
    {
    };
    if (::fstatvfs(fd, &info) == 0)
    {
        const std::uint64_t unit = info.f_frsize != 0 ? info.f_frsize : info.f_bsize;
        const std::uint64_t total = static_cast<std::uint64_t>(info.f_blocks) * unit;
        if (total > kPlaceholderBytes)
        {
            space.known = true;
            space.total_bytes = total;
            space.free_bytes = static_cast<std::uint64_t>(info.f_bavail) * unit;
        }
    }
    ::close(fd);
    return space;
}

int LocalSource::make_directory(const std::string &path)
{
    if (::mkdir(path.c_str(), 0777) != 0)
        return errno;
    // Open for everyone: a folder made as one user may be written by another (the console's root
    // does not override another user's folder permissions).
    ::chmod(path.c_str(), 0777);
    return 0;
}

int LocalSource::rename(const std::string &from, const std::string &to)
{
    return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
}

int LocalSource::remove_file(const std::string &path)
{
    return ::unlink(path.c_str()) == 0 ? 0 : errno;
}

int LocalSource::remove_directory(const std::string &path)
{
    return ::rmdir(path.c_str()) == 0 ? 0 : errno;
}

int LocalSource::copy_file(const std::string &from, const std::string &to, std::size_t chunk,
                           const ProgressFn &on_progress)
{
    if (chunk == 0)
        return EINVAL;
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return errno;
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0)
    {
        const int error = errno;
        ::close(in);
        return error;
    }
    ::fchmod(out, 0666); // not narrowed by the umask: the other user must be able to write it
    std::vector<char> buffer(chunk);
    std::uint64_t copied = 0;
    int error = 0;
    for (;;)
    {
        const ssize_t n = ::read(in, buffer.data(), chunk);
        if (n < 0)
        {
            error = errno;
            break;
        }
        if (n == 0)
            break;
        if (!write_all(out, buffer.data(), static_cast<std::size_t>(n)))
        {
            error = errno != 0 ? errno : EIO;
            break;
        }
        copied += static_cast<std::uint64_t>(n);
        if (on_progress && !on_progress(copied))
        {
            error = ECANCELED;
            break;
        }
    }
    if (error == 0 && ::fsync(out) != 0)
        error = errno;
    ::close(in);
    if (::close(out) != 0 && error == 0)
        error = errno;
    if (error != 0)
        ::unlink(to.c_str()); // never leave a half-written file behind
    return error;
}

bool LocalSource::same_device(const std::string &a, const std::string &b) const
{
    struct stat first
    {
    };
    struct stat second
    {
    };
    // st_dev 0 is a real value on the console (a nullfs mount reported it), so a failed stat is
    // what means "cannot tell", not a zero.
    return ::stat(a.c_str(), &first) == 0 && ::stat(b.c_str(), &second) == 0 &&
           first.st_dev == second.st_dev;
}

const void *LocalSource::kind_tag() const
{
    return &kKindTag;
}

} // namespace hui::explorer
