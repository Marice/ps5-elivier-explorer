// EliVier Explorer - A small trace file: what the app got through before it stopped.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/trace.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hui::explorer
{

namespace
{

constexpr off_t kMaxBytes = 64 * 1024;

char g_path[256] = "";
long long g_start_ms = 0;

long long now_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void append(const char *line, int length)
{
    const int fd = ::open(g_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0)
        return;
    if (::write(fd, line, static_cast<std::size_t>(length)) == length)
        ::fsync(fd);
    ::close(fd);
}

} // namespace

bool trace_open(std::span<const char *const> folders)
{
    g_path[0] = '\0';
    for (const char *folder : folders)
    {
        ::mkdir(folder, 0777);
        // Open for everyone: freed by a HEN the app is root, and on the console root may not write
        // in a folder another user owns (the app made this one as itself, before it was freed).
        ::chmod(folder, 0777);
        char path[sizeof(g_path)];
        std::snprintf(path, sizeof(path), "%s/trace.txt", folder);
        struct stat info
        {
        };
        const bool start_over = ::stat(path, &info) == 0 && info.st_size > kMaxBytes;
        const int fd = ::open(
            path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | (start_over ? O_TRUNC : 0), 0666);
        if (fd < 0)
            continue;
        ::fchmod(fd, 0666);
        ::close(fd);
        std::snprintf(g_path, sizeof(g_path), "%s", path);
        break;
    }
    if (g_path[0] == '\0')
        return false;
    g_start_ms = now_ms();
    char line[96];
    const int n = std::snprintf(line, sizeof(line), "=== run, pid %d, clock %ld\n",
                                static_cast<int>(::getpid()), static_cast<long>(::time(nullptr)));
    append(line, n);
    return true;
}

void trace(const char *format, ...)
{
    if (g_path[0] == '\0')
        return;
    char line[320];
    int n = std::snprintf(line, sizeof(line), "%7lld ms  ", now_ms() - g_start_ms);
    va_list arguments;
    va_start(arguments, format);
    n +=
        std::vsnprintf(line + n, sizeof(line) - 2 - static_cast<std::size_t>(n), format, arguments);
    va_end(arguments);
    if (n > static_cast<int>(sizeof(line)) - 2)
        n = static_cast<int>(sizeof(line)) - 2;
    line[n++] = '\n';
    append(line, n);
}

const char *trace_path()
{
    return g_path;
}

} // namespace hui::explorer
