// EliVier Explorer - The console's eight-channel audio output, for films with surround sound.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/surround_out.hpp"

#include "audio/mixer.hpp"
#include "explorer/trace.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cstdint>

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
}

namespace hui::ps5
{

namespace
{

constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
constexpr int kSystemUser = 0xff;
constexpr int kPortMain = 0;
constexpr unsigned int kFormatS16EightChannels = 2;

} // namespace

SurroundOut::~SurroundOut()
{
    close();
}

bool SurroundOut::open()
{
    if (handle_ >= 0)
        return true;
    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized)
    {
        explorer::trace("surround: sceAudioOutInit 0x%08x", static_cast<unsigned>(init));
        return false;
    }
    handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames, audio::kSampleRate,
                              kFormatS16EightChannels);
    if (handle_ < 0)
    {
        explorer::trace("surround: eight-channel port refused 0x%08x",
                        static_cast<unsigned>(handle_));
        handle_ = -1;
        return false;
    }
    explorer::trace("surround: eight-channel port %d open", handle_);
    return true;
}

bool SurroundOut::start(Pull pull)
{
    if (handle_ < 0 || running_)
        return false;
    pull_ = std::move(pull);
    stop_.store(false);
    // Never name threads: pthread_setname_np hangs on this platform.
    if (pthread_create(&thread_, nullptr, &SurroundOut::thread_main, this) != 0)
    {
        explorer::trace("surround: no thread");
        return false;
    }
    running_ = true;
    return true;
}

void *SurroundOut::thread_main(void *self)
{
    static_cast<SurroundOut *>(self)->run();
    return nullptr;
}

void SurroundOut::run()
{
    alignas(64) float grain[kGrainFrames * audio::kSurroundChannels];
    alignas(64) std::int16_t out[kGrainFrames * audio::kSurroundChannels];
    while (!stop_.load(std::memory_order_relaxed))
    {
        pull_(grain, kGrainFrames);
        for (int i = 0; i < kGrainFrames * audio::kSurroundChannels; ++i)
            out[i] = static_cast<std::int16_t>(std::clamp(grain[i], -1.0f, 1.0f) * 32767.0f);
        // Blocks until the port takes the grain: that paces the thread, as the mixer's does.
        if (sceAudioOutOutput(handle_, out) < 0)
            sys::sleep_us(5000);
    }
    sceAudioOutOutput(handle_, nullptr); // drain the queued grain
}

void SurroundOut::close()
{
    if (running_)
    {
        stop_.store(true);
        pthread_join(thread_, nullptr);
        running_ = false;
    }
    pull_ = nullptr;
    if (handle_ >= 0)
    {
        sceAudioOutClose(handle_);
        explorer::trace("surround: port closed");
        handle_ = -1;
    }
}

} // namespace hui::ps5
