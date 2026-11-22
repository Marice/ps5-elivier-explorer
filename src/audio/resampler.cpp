// EliVier Explorer - Linear resampling of a stereo stream to the mixer's rate.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/resampler.hpp"

#include <algorithm>
#include <cmath>

namespace hui::audio
{

Resampler::Resampler(int in_rate, int out_rate)
    : step_(static_cast<double>(in_rate) / static_cast<double>(out_rate)),
      passthrough_(in_rate == out_rate)
{
}

void Resampler::push(const float *stereo, int frames)
{
    buffer_.insert(buffer_.end(), stereo, stereo + static_cast<std::size_t>(frames) * 2);
}

int Resampler::ready() const
{
    const std::size_t frames = buffer_.size() / 2;
    if (passthrough_)
        return static_cast<int>(frames);
    if (frames < 2 || position_ >= static_cast<double>(frames - 1))
        return 0;
    return static_cast<int>(std::ceil((static_cast<double>(frames - 1) - position_) / step_));
}

int Resampler::pull(float *stereo, int max_frames)
{
    const std::size_t frames = buffer_.size() / 2;
    int made = 0;
    if (passthrough_)
    {
        made =
            static_cast<int>(std::min<std::size_t>(frames, static_cast<std::size_t>(max_frames)));
        std::copy(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(made) * 2, stereo);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(made) * 2);
        return made;
    }
    while (made < max_frames && frames >= 2 && position_ < static_cast<double>(frames - 1))
    {
        const std::size_t i = static_cast<std::size_t>(position_);
        const float f = static_cast<float>(position_ - static_cast<double>(i));
        for (int c = 0; c < 2; ++c)
        {
            const float a = buffer_[i * 2 + static_cast<std::size_t>(c)];
            const float b = buffer_[(i + 1) * 2 + static_cast<std::size_t>(c)];
            stereo[made * 2 + c] = a + (b - a) * f;
        }
        ++made;
        position_ += step_;
    }
    // Drop what has been passed, keeping the frame the next sample interpolates from.
    const std::size_t drop = std::min(static_cast<std::size_t>(position_), frames);
    if (drop > 0)
    {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop) * 2);
        position_ -= static_cast<double>(drop);
    }
    return made;
}

void Resampler::reset()
{
    buffer_.clear();
    position_ = 0.0;
}

} // namespace hui::audio
