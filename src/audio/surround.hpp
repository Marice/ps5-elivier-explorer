// EliVier Explorer - A film's sound on eight channels, for a receiver or a 5.1 set.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mixer is stereo and stays so for the interface's sounds and the background music. A film
// with more than two channels goes to an output of its own instead: a second port of the
// console's audio that takes eight channels, which the console mixes down itself when the TV is
// stereo (D65). The output (SurroundSink) is the platform's; it pulls the sound from its own
// thread a grain at a time. SurroundPlayer is the film's side: volume, pause, seek, and the clock,
// which counts the frames the output has taken.
//
//   if (sink->open())
//   {
//       SurroundPlayer player(*sink, read, seek);   // read: engine->read_sound
//       player.start();
//   }

#pragma once

#include "audio/sound_player.hpp"

#include <cstdint>
#include <functional>
#include <mutex>

namespace hui::audio
{

// Channels of the surround output, in the console's order: L R C LFE, side pair, back pair.
constexpr int kSurroundChannels = 8;

class SurroundSink
{
  public:
    // Fills `frames` frames of kSurroundChannels float samples (48 kHz, nominal -1..1) and says
    // how many it filled; the rest is silence.
    using Pull = std::function<int(float *out, int frames)>;

    virtual ~SurroundSink() = default;
    // Opens the output; false when the console refuses (the film then plays in stereo).
    virtual bool open() = 0;
    // Starts pulling, from the output's own thread. False when it cannot.
    virtual bool start(Pull pull) = 0;
    // Stops pulling and closes the output. Safe to call again.
    virtual void close() = 0;
};

class SurroundPlayer final : public SoundPlayer
{
  public:
    // Up to `frames` frames of the film's sound; Decoder::kTryLater when none is ready yet (the
    // clock then waits), 0 at the end.
    using Read = std::function<int(float *out, int frames)>;
    // Moves the film's sound to `seconds`.
    using Seek = std::function<void(double seconds)>;

    SurroundPlayer(SurroundSink &sink, Read read, Seek seek);
    ~SurroundPlayer() override;
    SurroundPlayer(const SurroundPlayer &) = delete;
    SurroundPlayer &operator=(const SurroundPlayer &) = delete;

    // Starts the output pulling. False when it cannot (the caller falls back to stereo).
    bool start();

    void stop() override;
    void pump(float dt) override;
    void set_paused(bool paused) override;
    void seek_to(double seconds) override;
    void set_volume(int steps) override;
    int volume() const override;
    bool finished() const override;
    double position() const override;

    // What the output's thread calls (public for tests): fills a grain.
    int pull(float *out, int frames);

  private:
    SurroundSink &sink_;
    Read read_;
    Seek seek_;
    mutable std::mutex mutex_;
    bool started_ = false;
    bool paused_ = false;
    bool ended_ = false;
    int volume_ = 10;
    double base_ = 0.0;         // seconds into the film where counting started
    std::uint64_t played_ = 0;  // frames the output has taken since then
};

} // namespace hui::audio
