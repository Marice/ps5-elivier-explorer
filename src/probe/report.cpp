// EliVier Explorer - The report a probe run leaves, timing statistics and memory test patterns.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe/report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hui::probe
{

namespace
{

constexpr std::size_t kMaxValueLength = 200;

std::string clean_key(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!plain)
            c = '_';
    }
    return out;
}

std::string clean_value(std::string_view text)
{
    std::string out(text.substr(0, kMaxValueLength));
    for (char &c : out)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return out;
}

} // namespace

void Report::add(std::string_view probe, std::string_view key, std::string_view value)
{
    lines_.push_back("probe " + clean_key(probe) + " " + clean_key(key) + "=" + clean_value(value));
}

void Report::add(std::string_view probe, std::string_view key, std::int64_t value)
{
    add(probe, key, std::to_string(value));
}

void Report::add_real(std::string_view probe, std::string_view key, double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", std::clamp(decimals, 0, 9), value);
    add(probe, key, std::string_view(text));
}

void Report::add_hex(std::string_view probe, std::string_view key, std::uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(value));
    add(probe, key, std::string_view(text));
}

void Report::note(std::string_view probe, std::string_view text)
{
    add(probe, "note", text);
}

std::string Report::text() const
{
    std::string out;
    for (const std::string &line : lines_)
    {
        out += line;
        out += '\n';
    }
    return out;
}

void TimingStats::add(double milliseconds)
{
    samples_.push_back(milliseconds);
}

double TimingStats::average() const
{
    if (samples_.empty())
        return 0.0;
    double total = 0.0;
    for (const double value : samples_)
        total += value;
    return total / static_cast<double>(samples_.size());
}

double TimingStats::minimum() const
{
    return samples_.empty() ? 0.0 : *std::min_element(samples_.begin(), samples_.end());
}

double TimingStats::maximum() const
{
    return samples_.empty() ? 0.0 : *std::max_element(samples_.begin(), samples_.end());
}

double TimingStats::percentile(double fraction) const
{
    if (samples_.empty())
        return 0.0;
    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const double wanted = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(sorted.size());
    const double rank = std::ceil(wanted - 1e-9); // the epsilon keeps 0.95 * 100 at rank 95
    const std::size_t index = rank < 1.0 ? 0 : static_cast<std::size_t>(rank) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

double mib_per_second(std::size_t bytes, double seconds)
{
    if (seconds <= 0.0)
        return 0.0;
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

namespace
{

// xorshift32: fast, and the same sequence on every machine.
std::uint32_t next_word(std::uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace

void fill_pattern(void *data, std::size_t bytes, std::uint32_t seed)
{
    std::uint32_t state = seed | 1u;
    auto *out = static_cast<unsigned char *>(data);
    std::size_t at = 0;
    for (; at + 4 <= bytes; at += 4)
    {
        const std::uint32_t word = next_word(state);
        std::memcpy(out + at, &word, 4);
    }
    if (at < bytes)
    {
        const std::uint32_t word = next_word(state);
        std::memcpy(out + at, &word, bytes - at);
    }
}

std::size_t find_mismatch(const void *data, std::size_t bytes, std::uint32_t seed)
{
    std::uint32_t state = seed | 1u;
    const auto *in = static_cast<const unsigned char *>(data);
    std::size_t at = 0;
    for (; at + 4 <= bytes; at += 4)
    {
        const std::uint32_t expected = next_word(state);
        std::uint32_t actual = 0;
        std::memcpy(&actual, in + at, 4);
        if (actual != expected)
        {
            for (std::size_t i = 0; i < 4; ++i)
                if (in[at + i] != static_cast<unsigned char>(expected >> (8 * i)))
                    return at + i;
        }
    }
    if (at < bytes)
    {
        const std::uint32_t expected = next_word(state);
        for (std::size_t i = 0; at + i < bytes; ++i)
            if (in[at + i] != static_cast<unsigned char>(expected >> (8 * i)))
                return at + i;
    }
    return bytes;
}

std::vector<std::size_t> allocation_ladder(std::size_t max_bytes)
{
    std::vector<std::size_t> ladder;
    for (std::size_t size = std::size_t{16} << 20; size <= max_bytes; size <<= 1)
        ladder.push_back(size);
    return ladder;
}

} // namespace hui::probe
