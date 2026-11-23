// EliVier Explorer - Plays one decoded song on the mixer's second stream.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game thread calls pump() every frame: it decodes about 0.7 s ahead into a ring the audio
// thread reads. One player at a time (the rings are shared, so the audio thread never reads a ring
// that has just been freed). Stopping, pausing and seeking switch the stream to another ring
// instead of draining one the audio thread is reading.

#pragma once

#include "audio/decoder.hpp"
#include "audio/mixer.hpp"
#include "audio/resampler.hpp"
#include "audio/sound_player.hpp"

#include <memory>

namespace hui::audio
{

class AudioPlayer final : public SoundPlayer
{
  public:
    explicit AudioPlayer(Mixer &mixer);
    ~AudioPlayer() override;
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    // Plays a song from its start (what played before stops).
    void start(std::unique_ptr<Decoder> decoder);
    // Silences the stream and lets go of the song. Safe to call again.
    void stop() override;
    void pump(float dt) override;

    void set_paused(bool paused) override;
    bool paused() const
    {
        return paused_;
    }
    // Moves by `seconds` (negative goes back), inside the song.
    void seek_by(double seconds);
    void seek_to(double seconds) override;
    // 0..10, a perceptual curve like the settings.
    void set_volume(int steps) override;
    int volume() const override
    {
        return volume_;
    }

    bool active() const
    {
        return decoder_ != nullptr;
    }
    // The song has played to its end.
    bool finished() const override;
    double position() const override;
    double duration() const
    {
        return decoder_ ? decoder_->duration() : 0.0;
    }
    const Decoder *decoder() const
    {
        return decoder_.get();
    }

  private:
    void apply_gain(float seconds);
    void attach(StreamRing *ring);

    Mixer &mixer_;
    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Resampler> resampler_;
    int current_ = 0;       // which of the two song rings is being written
    bool attached_ = false; // the mixer reads the current ring (not the silent one)
    bool paused_ = false;
    float pause_timer_ = 0.0f;  // seconds until the stream is parked after the fade-out
    bool ended_ = false;        // the decoder has nothing more
    double base_ = 0.0;         // seconds into the song where the current ring starts
    std::uint64_t written_ = 0; // frames written to the current ring since then
    int volume_ = 10;
    std::vector<float> chunk_;
    std::vector<float> out_;
};

} // namespace hui::audio
