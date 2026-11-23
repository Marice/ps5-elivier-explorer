// EliVier Explorer - A film's sound on eight channels, for a receiver or a 5.1 set.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/surround.hpp"

#include "audio/decoder.hpp"
#include "audio/mixer.hpp"

#include <algorithm>
#include <cstring>

namespace hui::audio
{

SurroundPlayer::SurroundPlayer(SurroundSink &sink, Read read, Seek seek)
    : sink_(sink), read_(std::move(read)), seek_(std::move(seek))
{
}

SurroundPlayer::~SurroundPlayer()
{
    stop();
}

bool SurroundPlayer::start()
{
    if (started_)
        return true;
    started_ = sink_.start([this](float *out, int frames) { return pull(out, frames); });
    return started_;
}

void SurroundPlayer::stop()
{
    // Not under the lock: closing waits for the output's thread, which takes it in pull().
    if (started_)
        sink_.close();
    started_ = false;
}

void SurroundPlayer::pump(float /*dt*/)
{
    // Nothing to do: the output pulls the sound itself.
}

int SurroundPlayer::pull(float *out, int frames)
{
    std::memset(out, 0, static_cast<std::size_t>(frames) * kSurroundChannels * sizeof(float));
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ || ended_ || frames <= 0)
        return 0;
    const int got = read_(out, frames);
    if (got == Decoder::kTryLater)
    {
        // Nothing yet (a seek being served): silence, and the clock does not move.
        std::memset(out, 0, static_cast<std::size_t>(frames) * kSurroundChannels * sizeof(float));
        return 0;
    }
    if (got <= 0)
    {
        ended_ = true;
        return 0;
    }
    const int filled = std::min(got, frames);
    const float t = static_cast<float>(volume_) / 10.0f;
    const float gain = t * t; // the same curve as the mixer's player and the settings
    if (gain != 1.0f)
        for (std::size_t i = 0; i < static_cast<std::size_t>(filled) * kSurroundChannels; ++i)
            out[i] *= gain;
    played_ += static_cast<std::uint64_t>(filled);
    return filled;
}

void SurroundPlayer::set_paused(bool paused)
{
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

void SurroundPlayer::seek_to(double seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const double target = std::max(seconds, 0.0);
    seek_(target);
    base_ = target;
    played_ = 0;
    ended_ = false;
}

void SurroundPlayer::set_volume(int steps)
{
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = std::clamp(steps, 0, 10);
}

int SurroundPlayer::volume() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return volume_;
}

bool SurroundPlayer::finished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_;
}

double SurroundPlayer::position() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return base_ + static_cast<double>(played_) / kSampleRate;
}

} // namespace hui::audio
