// EliVier Explorer - Audio clock and frame selection for video playback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio is the master clock: it cannot skip or repeat without being heard, a picture can. The
// player counts the samples it hands to the stream ring and asks the ring how many are still
// waiting; the difference is what has been played. select_frame() then picks which decoded
// picture goes on screen for the current refresh.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace hui::video
{

// A picture without a presentation time (some containers omit it). It is shown at once.
inline constexpr double kNoTimestamp = std::numeric_limits<double>::quiet_NaN();

// Presentation time of the sound that is coming out of the speakers now, in seconds of the media.
class AudioClock
{
  public:
    explicit AudioClock(int sample_rate);

    // Start over at a media time: at the start of the file, and after every seek.
    void reset(double base_seconds);

    // Frames (one sample per channel) decoded and pushed into the ring since reset().
    void on_written(std::uint64_t frames);

    // Delay between the ring and the speakers. The clock runs this far behind what was written.
    void set_latency(double seconds);

    // buffered_frames: what the ring still holds. It does not advance while the output is paused
    // (nothing leaves the ring) and it never runs before the base time.
    double time(std::uint64_t buffered_frames) const;

  private:
    double rate_;
    double base_ = 0.0;
    double latency_ = 0.0;
    std::uint64_t written_ = 0;
};

struct SyncConfig
{
    // A picture counts as due this long before its time: half a refresh at 60 Hz, so that it is
    // not shown a whole refresh late.
    double lead = 1.0 / 120.0;
};

struct Selection
{
    // True when a picture goes on screen now. It is queue[drop]; the `drop` pictures before it
    // are late and are thrown away. Pictures after it wait for their turn.
    bool present = false;
    std::size_t drop = 0;
};

// pts: presentation times of the decoded pictures, oldest first (kNoTimestamp allowed). Among the
// pictures that are due, the newest is shown and the older ones are dropped to catch up. A late
// picture is still shown when it is the only one, so the picture never freezes.
Selection select_frame(std::span<const double> pts, double clock_seconds, const SyncConfig &config);

} // namespace hui::video
