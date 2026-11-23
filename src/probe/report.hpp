// EliVier Explorer - The report a probe run leaves, timing statistics and memory test patterns.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plain C++, no console calls, so that the parts that can be tested are tested on the PC. The
// console side (src/platform/ps5/probes.cpp) only asks the questions and writes the answers here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::probe
{

// One line per answer: "probe <name> <key>=<value>". A person reads it and so does a script, so
// the shape never changes: keys are letters, digits and _.-, and a value is one line of at most
// 200 characters (a longer one is cut).
class Report
{
  public:
    void add(std::string_view probe, std::string_view key, std::string_view value);
    void add(std::string_view probe, std::string_view key, std::int64_t value);
    void add_real(std::string_view probe, std::string_view key, double value, int decimals = 2);
    void add_hex(std::string_view probe, std::string_view key, std::uint32_t value);
    // key "note": a sentence about what was seen or why a step was skipped.
    void note(std::string_view probe, std::string_view text);

    const std::vector<std::string> &lines() const
    {
        return lines_;
    }
    std::string text() const; // every line followed by '\n'

  private:
    std::vector<std::string> lines_;
};

// Milliseconds per repetition of one thing: what it costs on average, at its worst, and at the
// 95th percentile (nearest rank), which is what a frame budget has to survive.
class TimingStats
{
  public:
    void add(double milliseconds);
    std::size_t count() const
    {
        return samples_.size();
    }
    double average() const;
    double minimum() const;
    double maximum() const;
    // 0 < fraction <= 1; 0 for no samples.
    double percentile(double fraction) const;

  private:
    std::vector<double> samples_;
};

double mib_per_second(std::size_t bytes, double seconds);

// Fills `bytes` bytes with a pseudo-random sequence that depends only on `seed`, and checks it
// again later: a memory block that comes back different was not stored faithfully.
void fill_pattern(void *data, std::size_t bytes, std::uint32_t seed);
// The index of the first byte that differs, or `bytes` when all of it matches.
std::size_t find_mismatch(const void *data, std::size_t bytes, std::uint32_t seed);

// Block sizes to try, 16 MiB doubling up to `max_bytes` (only those that fit). The probe stops at
// the first one that cannot be allocated, so a console short of memory is not pushed further.
std::vector<std::size_t> allocation_ladder(std::size_t max_bytes);

} // namespace hui::probe
