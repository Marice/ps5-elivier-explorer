// EliVier Explorer - What the app can do with each drive: see it, list it, write it, how fast.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/probe.hpp"

#include "explorer/trace.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace hui::explorer
{

namespace
{

constexpr ProbePlace kPlaces[] = {
    {"/data", "Internal (/data)"},
    {"/user", "User (/user)"},
    {"/mnt/usb0", "USB 1"},
    {"/mnt/usb1", "USB 2"},
    {"/mnt/usb2", "USB 3"},
    {"/mnt/usb3", "USB 4"},
    {"/mnt/usb4", "USB 5"},
    {"/mnt/usb5", "USB 6"},
    {"/mnt/usb6", "USB 7"},
    {"/mnt/usb7", "USB 8"},
    {"/mnt/ext0", "Extended storage 1"},
    {"/mnt/ext1", "Extended storage 2"},
    {"/download0", "The app's own storage"},
    {"/system", "System (read-only)", false},
};

constexpr const char *kProbeName = "/.elivier-probe";

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

double seconds_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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

// Makes, reads back and removes a small file; 0 when that worked, else the errno.
int write_test(const std::string &folder)
{
    const std::string path = folder + kProbeName;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
        return errno;
    static const char kText[] = "EliVier Explorer probe\n";
    const bool written = write_all(fd, kText, sizeof(kText) - 1);
    const int write_error = errno;
    ::close(fd);
    if (!written)
    {
        ::unlink(path.c_str());
        return write_error != 0 ? write_error : EIO;
    }
    char back[sizeof(kText)] = {};
    const int in = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    const ssize_t got = in >= 0 ? ::read(in, back, sizeof(back) - 1) : -1;
    const int read_error = errno;
    if (in >= 0)
        ::close(in);
    const bool same = got == static_cast<ssize_t>(sizeof(kText) - 1) &&
                      std::memcmp(back, kText, sizeof(kText) - 1) == 0;
    if (::unlink(path.c_str()) != 0)
        return errno;
    if (!same)
        return read_error != 0 ? read_error : EIO;
    return 0;
}

} // namespace

std::span<const ProbePlace> probe_places()
{
    return kPlaces;
}

std::vector<PlaceCheck> check_places(const std::string &root, std::span<const ProbePlace> places)
{
    std::vector<PlaceCheck> out;
    for (const ProbePlace &place : places)
    {
        PlaceCheck check;
        check.path = place.path;
        check.label = place.label;
        const std::string path = root + place.path;
        trace("probe %s: stat", place.path);
        struct stat info
        {
        };
        struct stat parent
        {
        };
        check.exists = ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
        if (!check.exists)
        {
            out.push_back(std::move(check));
            continue;
        }
        check.mounted = ::stat((root + parent_of(place.path)).c_str(), &parent) == 0 &&
                        parent.st_dev != info.st_dev;
        trace("probe %s: list", place.path);
        errno = 0;
        if (DIR *dir = ::opendir(path.c_str()))
        {
            check.listable = true;
            while (const dirent *entry = ::readdir(dir))
            {
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                    ++check.entries;
            }
            if (errno != 0)
            {
                check.listable = false;
                check.list_error = errno;
            }
            // fstatvfs on a folder opened for this alone, never statvfs(path): statvfs crashed
            // the app on any path at all (the stop at "probe /data: space"), while fstatvfs on a
            // file descriptor is safe. dirfd() is declared but not linked on the PS5 SDK, so the
            // descriptor is a second open rather than the listing's own.
            trace("probe %s: space", place.path);
            struct statvfs space
            {
            };
            const int space_fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (space_fd >= 0 && ::fstatvfs(space_fd, &space) == 0)
            {
                const auto unit =
                    static_cast<std::uint64_t>(space.f_frsize ? space.f_frsize : space.f_bsize);
                const std::uint64_t total = static_cast<std::uint64_t>(space.f_blocks) * unit;
                // An empty slot (/mnt/usb3 with nothing in it) is a placeholder of a few
                // megabytes: unknown, not a nearly-full drive.
                if (total > 16ull * 1024 * 1024)
                {
                    check.total_bytes = total;
                    check.free_bytes = static_cast<std::uint64_t>(space.f_bavail) * unit;
                }
            }
            if (space_fd >= 0)
                ::close(space_fd);
            ::closedir(dir);
        }
        else
        {
            check.list_error = errno;
        }
        // The file system's name is left out: statfs crashed the app the same way statvfs did.
        // A mount point without a drive is the root file system's folder: a write there says
        // nothing.
        if (!place.test_write)
        {
            check.write_error = kWriteSkipped;
        }
        else
        {
            trace("probe %s: write", place.path);
            check.write_error = write_test(path);
        }
        check.writable = check.write_error == 0;
        out.push_back(std::move(check));
    }
    return out;
}

SpeedResult measure_speed(const std::string &folder, std::uint64_t bytes, std::size_t chunk)
{
    SpeedResult result;
    if (chunk == 0)
    {
        result.error = EINVAL;
        return result;
    }
    const std::string path = folder + "/.elivier-speed";
    std::vector<char> buffer(chunk);
    for (std::size_t i = 0; i < chunk; ++i)
        buffer[i] = static_cast<char>(i * 31 + 7);

    const auto write_start = std::chrono::steady_clock::now();
    const int out = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0)
    {
        result.error = errno;
        return result;
    }
    std::uint64_t written = 0;
    while (written < bytes)
    {
        const std::size_t size =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk, bytes - written));
        if (!write_all(out, buffer.data(), size))
        {
            result.error = errno != 0 ? errno : EIO;
            break;
        }
        written += size;
    }
    if (result.error == 0 && ::fsync(out) != 0)
        result.error = errno;
    ::close(out);
    const double write_seconds = seconds_since(write_start);
    if (result.error != 0)
    {
        ::unlink(path.c_str());
        return result;
    }

    const auto read_start = std::chrono::steady_clock::now();
    const int in = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    std::uint64_t read = 0;
    if (in < 0)
    {
        result.error = errno;
    }
    else
    {
        for (;;)
        {
            const ssize_t n = ::read(in, buffer.data(), chunk);
            if (n < 0)
            {
                result.error = errno;
                break;
            }
            if (n == 0)
                break;
            read += static_cast<std::uint64_t>(n);
        }
        ::close(in);
    }
    const double read_seconds = seconds_since(read_start);
    ::unlink(path.c_str());
    if (result.error != 0)
        return result;
    if (read != written)
    {
        result.error = EIO;
        return result;
    }
    constexpr double kMb = 1024.0 * 1024.0;
    result.ok = true;
    result.bytes = written;
    result.write_mb_s =
        write_seconds > 0.0 ? static_cast<double>(written) / kMb / write_seconds : 0.0;
    result.read_mb_s = read_seconds > 0.0 ? static_cast<double>(read) / kMb / read_seconds : 0.0;
    return result;
}

std::string probe_report(const AccessReport &access, const std::vector<PlaceCheck> &places,
                         const std::vector<SpeedRun> &speeds)
{
    std::string text = "EliVier Explorer probe\n\n";
    char line[320];
    std::snprintf(line, sizeof(line),
                  "access: %s%s%s; uid %d -> %d; /app0 %s; sandbox root back %s; %d ms\n",
                  route_name(access.route), access.request_taken.empty() ? "" : " ",
                  access.request_taken.c_str(), access.uid_before, access.uid_after,
                  access.app_folder ? "there" : "gone", access.root_put_back ? "yes" : "no",
                  access.milliseconds);
    text += line;
    for (const std::string &step : access.steps)
        text += "  " + step + "\n";
    text += "\nplaces:\n";
    for (const PlaceCheck &check : places)
    {
        if (!check.exists)
        {
            std::snprintf(line, sizeof(line), "  %-12s not there\n", check.path.c_str());
            text += line;
            continue;
        }
        std::snprintf(line, sizeof(line),
                      "  %-12s %s%s list %s(%d, errno %d) write %s free %llu of %llu "
                      "MiB\n",
                      check.path.c_str(),
                      check.file_system.empty() ? "?" : check.file_system.c_str(),
                      check.mounted ? " mounted" : "", check.listable ? "yes " : "no ",
                      check.entries, check.list_error,
                      check.write_error == kWriteSkipped ? "skipped"
                      : check.writable                   ? "yes"
                                                         : "no",
                      static_cast<unsigned long long>(check.free_bytes >> 20),
                      static_cast<unsigned long long>(check.total_bytes >> 20));
        text += line;
    }
    if (!speeds.empty())
        text += "\nspeed:\n";
    for (const SpeedRun &run : speeds)
    {
        if (run.result.ok)
            std::snprintf(line, sizeof(line),
                          "  %-24s chunk %zu KiB: %llu MiB, write %.1f MB/s, read %.1f MB/s\n",
                          run.folder.c_str(), run.chunk >> 10,
                          static_cast<unsigned long long>(run.result.bytes >> 20),
                          run.result.write_mb_s, run.result.read_mb_s);
        else
            std::snprintf(line, sizeof(line), "  %-24s chunk %zu KiB: failed, errno %d\n",
                          run.folder.c_str(), run.chunk >> 10, run.result.error);
        text += line;
    }
    return text;
}

} // namespace hui::explorer
