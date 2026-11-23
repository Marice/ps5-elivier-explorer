// EliVier Explorer - What a viewer needs of whatever plays its sound.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A film's sound goes either through the mixer in stereo (AudioPlayer) or to an eight-channel
// output of its own (SurroundPlayer, D65). The video viewer drives both the same way, and both
// are its clock: position() is the time of the sound being heard.

#pragma once

namespace hui::audio
{

class SoundPlayer
{
  public:
    virtual ~SoundPlayer() = default;
    // Lets go of the sound. Safe to call again.
    virtual void stop() = 0;
    // Every frame, on the game thread.
    virtual void pump(float dt) = 0;
    virtual void set_paused(bool paused) = 0;
    virtual void seek_to(double seconds) = 0;
    // 0..10, a perceptual curve like the settings.
    virtual void set_volume(int steps) = 0;
    virtual int volume() const = 0;
    // The sound has played to its end.
    virtual bool finished() const = 0;
    virtual double position() const = 0;
};

} // namespace hui::audio
