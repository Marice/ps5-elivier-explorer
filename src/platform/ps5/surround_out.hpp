// EliVier Explorer - The console's eight-channel audio output, for films with surround sound.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A second port of sceAudioOut beside the mixer's stereo one, opened only while a film with more
// than two channels plays: S16, eight channels, 48 kHz, 256 frames a grain, as EVO Player, VLC
// and MPC for PS5 open theirs (D65). The console mixes it down itself when the TV is stereo.

#pragma once

#include "audio/surround.hpp"

#include <atomic>
#include <pthread.h>

namespace hui::ps5
{

class SurroundOut final : public audio::SurroundSink
{
  public:
    static constexpr int kGrainFrames = 256;

    SurroundOut() = default;
    SurroundOut(const SurroundOut &) = delete;
    SurroundOut &operator=(const SurroundOut &) = delete;
    ~SurroundOut() override;

    bool open() override;
    bool start(Pull pull) override;
    void close() override;

  private:
    static void *thread_main(void *self);
    void run();

    int handle_ = -1;
    Pull pull_;
    pthread_t thread_{};
    bool running_ = false;
    std::atomic<bool> stop_{false};
};

} // namespace hui::ps5
