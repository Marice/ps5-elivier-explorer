// EliVier Explorer - Asking the HEN to free the app from its sandbox, so it sees every drive.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Porpoise's porpoise_jailbreak.cpp (Copyright (C) 2026 Ruben, Project Porpoise,
// GPL-3.0-or-later). What differs: the app asks to see the whole console (USB drives, extended
// storage, /user), not only /data, which etaHEN can already mount into the sandbox.

#include "platform/ps5/hen_access.hpp"

#include "explorer/trace.hpp"
#include "platform/ps5/system.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <iterator>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" int sceKernelUsleep(unsigned microseconds);
extern "C" int sceKernelDlsym(int handle, const char *name, void **address);
namespace hui::ps5
{

namespace
{

using explorer::AccessReport;
using explorer::AccessRoute;

// The request files daemons watch for in /download0. Each round publishes all three; whichever
// daemon is up takes its own.
const char *const kRequests[] = {
    "/download0/etahen_jailbreak",   // etaHEN, OnionHEN, older Lapy daemons
    "/download0/onionhen_jailbreak", // OnionHEN also takes this name
    "/download0/elevate_proc",       // the Lapy owned-root daemon
};

void step(AccessReport &report, const char *format, int a = 0, int b = 0, int c = 0)
{
    char line[200];
    std::snprintf(line, sizeof(line), format, a, b, c);
    sys::log("[EXPLORER] access: %s", line);
    explorer::trace("access: %s", line);
    report.steps.emplace_back(line);
}

bool is_directory(const char *path)
{
    struct stat info
    {
    };
    return ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

// The whole console: USB mount points and /user exist only outside the sandbox.
bool whole_console()
{
    return is_directory("/mnt/usb0") && is_directory("/user");
}

bool app_folder_here()
{
    struct stat info
    {
    };
    return ::stat("/app0/eboot.bin", &info) == 0;
}

// Writes {"PID":n} to one request path atomically (through a .tmp).
bool publish_one(const char *path, int pid)
{
    char staged[64];
    std::snprintf(staged, sizeof(staged), "%s.tmp", path);
    ::unlink(staged);
    ::unlink(path);
    const int fd = ::open(staged, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0)
        return false;
    ::fchmod(fd, 0666);
    char body[32];
    const int n = std::snprintf(body, sizeof(body), "{\"PID\":%d}\n", pid);
    const bool written = ::write(fd, body, static_cast<std::size_t>(n)) == n && ::fsync(fd) == 0;
    ::close(fd);
    if (!written || ::rename(staged, path) != 0)
    {
        ::unlink(staged);
        return false;
    }
    return true;
}

// One round of request files; true when a daemon took one (deleted it) within about 1.5 s. Only a
// name that was published counts: one that could not be made is not there, taken or not.
bool request_file(AccessReport &report)
{
    const int pid = static_cast<int>(::getpid());
    bool made[sizeof(kRequests) / sizeof(kRequests[0])] = {};
    int published = 0;
    for (std::size_t i = 0; i < std::size(kRequests); ++i)
    {
        made[i] = publish_one(kRequests[i], pid);
        published += made[i] ? 1 : 0;
    }
    if (published == 0)
    {
        step(report, "no request file could be made (errno %d)", errno);
        return false;
    }
    bool taken = false;
    for (int polls = 0; polls < 90 && !taken; ++polls)
    {
        sceKernelUsleep(16667);
        for (std::size_t i = 0; i < std::size(kRequests); ++i)
        {
            if (made[i] && ::access(kRequests[i], F_OK) != 0)
            {
                taken = true;
                report.request_taken = kRequests[i];
                step(report, "request file taken after %d polls; uid now %d", polls,
                     static_cast<int>(::geteuid()));
                break;
            }
        }
    }
    if (!taken)
        step(report, "%d request files made, none taken", published);
    for (const char *path : kRequests)
        ::unlink(path); // nothing left behind for the next round, whoever took what
    return taken;
}

// The legacy command servers: magic, command 5 (jailbreak), the pid. 0 when neither freed it.
int request_port(AccessReport &report)
{
    struct Command
    {
        int magic;
        int cmd;
        int pid;
        int ret;
        char msg1[0x500];
        char msg2[0x500];
    };
    for (int port : {9028, 9069})
    {
        Command cmd{};
        cmd.magic = static_cast<int>(0xDEADBEEF);
        cmd.cmd = 5;
        cmd.pid = static_cast<int>(::getpid());
        cmd.ret = -1337;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return 0;
        // A server that takes the connection but never answers must not hold the start: 2 s.
        timeval limit{};
        limit.tv_sec = 2;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(0x7F000001);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        {
            ::close(fd);
            step(report, "nothing on port %d", port);
            continue;
        }
        const bool sent = ::send(fd, &cmd, sizeof(cmd), 0) == static_cast<ssize_t>(sizeof(cmd));
        int got = 0;
        while (sent && got < static_cast<int>(sizeof(cmd)))
        {
            const ssize_t r = ::recv(fd, reinterpret_cast<char *>(&cmd) + got,
                                     sizeof(cmd) - static_cast<std::size_t>(got), 0);
            if (r <= 0)
                break;
            got += static_cast<int>(r);
        }
        ::close(fd);
        step(report, "port %d answered %d (%d bytes)", port, cmd.ret, got);
        if (sent && got == static_cast<int>(sizeof(cmd)) && cmd.ret == 0)
            return port;
    }
    return 0;
}

// A system call through libkernel's own "syscall" instruction (the console takes them only from
// there): libkernel's getpid is "mov rax, 20; mov r10, rcx; syscall; jb error; ret", so ten bytes
// in, with our number in rax, it makes ours and returns its result. For fchdir and chroot when
// the title's libkernel gives out neither.
extern "C" long explorer_kernel_call(long number, long argument, const void *at);
asm(".text\n"
    ".p2align 4\n"
    ".type explorer_kernel_call, @function\n"
    "explorer_kernel_call:\n"
    "  mov %rdi, %rax\n"
    "  mov %rsi, %rdi\n"
    "  jmp *%rdx\n"
    ".size explorer_kernel_call, . - explorer_kernel_call\n");

long raw_call(long number, long argument)
{
    const auto getpid_at = reinterpret_cast<std::uintptr_t>(&::getpid);
    if (getpid_at == 0)
        return -1;
    return explorer_kernel_call(number, argument, reinterpret_cast<const void *>(getpid_at + 0xa));
}

int raw_fchdir(int fd)
{
    return static_cast<int>(raw_call(13 /* SYS_fchdir */, fd));
}

int raw_chroot(const char *path)
{
    return static_cast<int>(raw_call(61 /* SYS_chroot */, reinterpret_cast<long>(path)));
}

// The daemon made the console's root the app's root, so /app0 is gone: the sandbox the app
// started in becomes its root again.
bool put_root_back(int sandbox_root, AccessReport &report)
{
    using FchdirFn = int (*)(int);
    using ChrootFn = int (*)(const char *);
    // Not linked: the SDK's stubs do not export them. Looked up by name below, else made as system
    // calls.
    FchdirFn to_dir = nullptr;
    ChrootFn to_root = nullptr;
    for (int handle : {0x2001, 0x2, 0x1}) // libkernel in a title, then libc's
    {
        void *address = nullptr;
        if (!to_dir && sceKernelDlsym(handle, "fchdir", &address) == 0 && address)
            to_dir = reinterpret_cast<FchdirFn>(address);
        address = nullptr;
        if (!to_root && sceKernelDlsym(handle, "chroot", &address) == 0 && address)
            to_root = reinterpret_cast<ChrootFn>(address);
    }
    if (!to_dir || !to_root)
    {
        step(report, "fchdir %d, chroot %d from libkernel; the rest as system calls",
             to_dir != nullptr, to_root != nullptr);
        if (!to_dir)
            to_dir = raw_fchdir;
        if (!to_root)
            to_root = raw_chroot;
    }
    if (sandbox_root < 0 || to_dir(sandbox_root) != 0 || to_root(".") != 0)
        return false;
    ::chdir("/");
    return app_folder_here();
}

} // namespace

void hen_prepare()
{
    if (::seteuid(::geteuid()) != 0)
        sys::log("[EXPLORER] access: seteuid(self) failed (errno %d)", errno);
}

explorer::AccessReport hen_free(int rounds, const char *install_folder)
{
    AccessReport report;
    const std::int64_t start = sys::monotonic_us();
    report.uid_before = static_cast<int>(::geteuid());
    if (whole_console())
    {
        report.route = AccessRoute::already_free;
        report.uid_after = report.uid_before;
        step(report, "the whole console is visible already (uid %d)", report.uid_before);
        return report;
    }
    step(report, "sandboxed (uid %d); asking the HEN", report.uid_before);
    const bool app_before = app_folder_here();
    // Held for put_root_back(), in case the daemon takes /app0 away.
    const int sandbox_root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    report.route = AccessRoute::none;
    for (int round = 0; round < rounds && report.route == AccessRoute::none; ++round)
    {
        AccessRoute asked = AccessRoute::request_file;
        // A daemon may take the request file without freeing the app (etaHEN does that for a title
        // that is not on its list): the command ports are tried unless the console shows up.
        bool free_now = request_file(report);
        for (int grace = 0; free_now && grace < 30 && !whole_console(); ++grace)
            sceKernelUsleep(16667);
        if (!whole_console())
        {
            const int port = request_port(report);
            if (port != 0)
                asked = port == 9028 ? AccessRoute::port_9028 : AccessRoute::port_9069;
            else if (!free_now)
                asked = AccessRoute::none;
        }
        // Up to about 1.5 s for the console's drives to appear after a daemon acted.
        for (int grace = 0; grace < 90; ++grace)
        {
            if (whole_console())
            {
                report.route = asked == AccessRoute::none ? AccessRoute::request_file : asked;
                step(report, "the whole console is visible (round %d)", round);
                break;
            }
            sceKernelUsleep(16667);
        }
    }
    // Freed with the console's root: /app0 is gone. The install folder holds the same files, so
    // the app reads them there and keeps the whole console; only without it does the sandbox
    // root come back (and the drives go with it).
    if (report.route != AccessRoute::none && app_before && !app_folder_here())
    {
        if (is_directory(install_folder))
        {
            step(report, "freed with the console's root (no /app0); files from the install folder");
        }
        else
        {
            report.root_put_back = put_root_back(sandbox_root, report);
            step(report, "freed with the console's root (no /app0); sandbox root back %d",
                 report.root_put_back);
        }
    }
    if (sandbox_root >= 0)
        ::close(sandbox_root);
    if (report.route == AccessRoute::none)
        step(report, "still sandboxed after %d rounds", rounds);
    report.app_folder = app_folder_here();
    report.uid_after = static_cast<int>(::geteuid());
    report.milliseconds = static_cast<int>((sys::monotonic_us() - start) / 1000);
    return report;
}

} // namespace hui::ps5
