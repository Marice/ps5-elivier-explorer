// EliVier Explorer - A line in the trace when the app crashes: signal, address, where it was.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Modelled on ps5-smbd's crash handler (on_crash in its src/main.c).

#include "platform/ps5/crash.hpp"

#include "explorer/trace.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace hui::ps5
{

namespace
{

const void *g_anchor = nullptr;
alignas(16) char g_stack[64 * 1024]; // the handler's own, so a stack overflow is reported too

void on_crash(int signal_number, siginfo_t *info, void *context)
{
    const mcontext_t *mc = &static_cast<ucontext_t *>(context)->uc_mcontext;
    const auto base = reinterpret_cast<std::uintptr_t>(g_anchor);
    const auto *sp = reinterpret_cast<const std::uintptr_t *>(mc->mc_rsp);
    char line[1024];
    int n = std::snprintf(
        line, sizeof(line), "CRASH signal %d at %p: rip anchor%+ld, rsp %p; stack", signal_number,
        info->si_addr, static_cast<long>(mc->mc_rip - base), reinterpret_cast<void *>(mc->mc_rsp));
    for (int i = 0; i < 64 && n < static_cast<int>(sizeof(line)) - 24; ++i)
    {
        const long offset = static_cast<long>(sp[i] - base);
        if (offset > -0x400000 && offset < 0x400000)
            n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " anchor%+ld",
                               offset);
    }
    n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "\n");
    const char *path = explorer::trace_path();
    if (path[0] != '\0')
    {
        const int fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd >= 0)
        {
            ::write(fd, line, static_cast<std::size_t>(n));
            ::fsync(fd);
            ::close(fd);
        }
    }
    ::write(1, line, static_cast<std::size_t>(n));
    ::_exit(128 + signal_number);
}

} // namespace

void catch_crashes(const void *anchor)
{
    g_anchor = anchor;
    stack_t alternate{};
    alternate.ss_sp = g_stack;
    alternate.ss_size = sizeof(g_stack);
    const int alt = ::sigaltstack(&alternate, nullptr);
    struct sigaction action
    {
    };
    action.sa_sigaction = on_crash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int failed = 0;
    for (int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        failed += ::sigaction(signal_number, &action, nullptr) == 0 ? 0 : 1;
    explorer::trace("crash handler: sigaltstack %d, %d of 5 signals refused", alt, failed);
}

} // namespace hui::ps5
