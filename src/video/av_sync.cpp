// EliVier Explorer - Audio clock and frame selection for video playback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/av_sync.hpp"

#include <algorithm>
#include <cmath>

namespace hui::video
{

AudioClock::AudioClock(int sample_rate) : rate_(sample_rate > 0 ? sample_rate : 48000)
{
}

void AudioClock::reset(double base_seconds)
{
    base_ = base_seconds;
    written_ = 0;
}

void AudioClock::on_written(std::uint64_t frames)
{
    written_ += frames;
}

void AudioClock::set_latency(double seconds)
{
    latency_ = std::max(seconds, 0.0);
}

double AudioClock::time(std::uint64_t buffered_frames) const
{
    const std::uint64_t played = written_ > buffered_frames ? written_ - buffered_frames : 0;
    return std::max(base_ + static_cast<double>(played) / rate_ - latency_, base_);
}

Selection select_frame(std::span<const double> pts, double clock_seconds, const SyncConfig &config)
{
    Selection selection;
    const double due_until = clock_seconds + config.lead;
    // Pictures are in presentation order: everything up to the first one in the future is due.
    std::size_t due = 0;
    while (due < pts.size() && (std::isnan(pts[due]) || pts[due] <= due_until))
    {
        ++due;
    }
    if (due == 0)
    {
        return selection;
    }
    selection.present = true;
    selection.drop = due - 1;
    return selection;
}

} // namespace hui::video
