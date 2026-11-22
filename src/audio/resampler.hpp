// EliVier Explorer - Linear resampling of a stereo stream to the mixer's rate.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

namespace hui::audio
{

// Push stereo frames at the input rate, pull frames at the output rate. Unconsumed input stays
// inside, so pulling in small pieces gives the same samples as pulling at once.
class Resampler
{
  public:
    Resampler(int in_rate, int out_rate);

    void push(const float *stereo, int frames);
    // Writes up to `max_frames` frames; returns how many.
    int pull(float *stereo, int max_frames);
    // Output frames that pull() can produce from what is buffered now.
    int ready() const;
    void reset();

  private:
    double step_;
    bool passthrough_;
    double position_ = 0.0;     // into buffer_, in frames
    std::vector<float> buffer_; // stereo
};

} // namespace hui::audio
