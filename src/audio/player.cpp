// EliVier Explorer - Plays one decoded song on the mixer's second stream.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/player.hpp"

#include <algorithm>

namespace hui::audio
{

namespace
{

constexpr std::size_t kStreamSlot = 1; // slot 0 is the background music
constexpr std::size_t kAhead = kSampleRate * 7 / 10;
constexpr int kChunk = 1152;
constexpr int kMaxDecodesPerPump = 8;
constexpr float kFade = 0.05f;
constexpr float kParkAfter = 0.08f;

// Shared for the life of the process: the audio thread may still hold a pointer to one for a grain
// after the player that wrote it is gone.
struct Rings
{
    StreamRing song[2]{StreamRing(1u << 16), StreamRing(1u << 16)};
    StreamRing silent{1u << 10};
};

Rings &rings()
{
    static Rings shared;
    return shared;
}

} // namespace

AudioPlayer::AudioPlayer(Mixer &mixer)
    : mixer_(mixer), chunk_(static_cast<std::size_t>(kChunk) * 2),
      out_(static_cast<std::size_t>(kChunk) * 4)
{
}

AudioPlayer::~AudioPlayer()
{
    stop();
}

void AudioPlayer::attach(StreamRing *ring)
{
    // A song or film the player opened is as loud as its own volume says, not scaled again by
    // the background music setting (docs/decision-log.md, D57).
    mixer_.set_stream_follows_music(kStreamSlot, false);
    mixer_.swap_stream(kStreamSlot, ring);
}

void AudioPlayer::apply_gain(float seconds)
{
    const float t = static_cast<float>(volume_) / 10.0f;
    mixer_.set_stream_gain(kStreamSlot, paused_ ? 0.0f : t * t, seconds);
}

void AudioPlayer::start(std::unique_ptr<Decoder> decoder)
{
    stop();
    if (!decoder)
        return;
    decoder_ = std::move(decoder);
    resampler_ = std::make_unique<Resampler>(decoder_->rate(), kSampleRate);
    paused_ = false;
    ended_ = false;
    base_ = 0.0;
    written_ = 0;
    current_ = 0;
    rings().song[0].drain();
    attach(&rings().song[0]);
    attached_ = true;
    apply_gain(0.02f);
    pump(0.0f);
}

void AudioPlayer::stop()
{
    if (!decoder_)
        return;
    mixer_.set_stream_gain(kStreamSlot, 0.0f, 0.0f);
    attach(&rings().silent);
    attached_ = false;
    decoder_.reset();
    resampler_.reset();
}

void AudioPlayer::pump(float dt)
{
    if (!decoder_)
        return;
    if (paused_ && pause_timer_ > 0.0f)
    {
        pause_timer_ -= dt;
        if (pause_timer_ <= 0.0f && attached_)
        {
            attach(&rings().silent); // nothing reads the song ring while it is paused
            attached_ = false;
        }
    }
    StreamRing &ring = rings().song[current_];
    for (int decodes = 0; !ended_ && ring.available() < kAhead && ring.space() > 0;)
    {
        if (resampler_->ready() < kChunk)
        {
            if (++decodes > kMaxDecodesPerPump)
                break;
            const int got = decoder_->read(chunk_.data(), kChunk);
            if (got == Decoder::kTryLater)
                break; // nothing yet, not the end: nothing is written, so the clock waits
            if (got <= 0)
            {
                ended_ = true;
                break;
            }
            resampler_->push(chunk_.data(), got);
            continue;
        }
        const int room = static_cast<int>(std::min<std::size_t>(ring.space(), kChunk * 2));
        const int made = resampler_->pull(out_.data(), room);
        if (made <= 0)
            break;
        written_ += ring.write(out_.data(), static_cast<std::size_t>(made));
    }
    // The end: what the resampler still holds goes out too.
    if (ended_ && resampler_->ready() > 0 && ring.space() > 0)
    {
        const int room = static_cast<int>(std::min<std::size_t>(ring.space(), kChunk * 2));
        const int made = resampler_->pull(out_.data(), room);
        if (made > 0)
            written_ += ring.write(out_.data(), static_cast<std::size_t>(made));
    }
}

void AudioPlayer::set_paused(bool paused)
{
    if (!decoder_ || paused == paused_)
        return;
    paused_ = paused;
    if (paused)
    {
        apply_gain(kFade);
        pause_timer_ = kParkAfter;
        return;
    }
    pause_timer_ = 0.0f;
    if (!attached_)
    {
        attach(&rings().song[current_]);
        attached_ = true;
    }
    apply_gain(kFade);
}

void AudioPlayer::seek_to(double seconds)
{
    if (!decoder_)
        return;
    const double length = decoder_->duration();
    const double target = std::clamp(seconds, 0.0, length > 0.2 ? length - 0.2 : 0.0);
    if (!decoder_->seek(target))
        return;
    resampler_->reset();
    ended_ = false;
    // A fresh ring: the audio thread keeps reading the old one until it has switched.
    const int next = 1 - current_;
    rings().song[next].drain();
    current_ = next;
    base_ = target;
    written_ = 0;
    if (attached_)
        attach(&rings().song[current_]);
    pump(0.0f);
}

void AudioPlayer::seek_by(double seconds)
{
    seek_to(position() + seconds);
}

void AudioPlayer::set_volume(int steps)
{
    volume_ = std::clamp(steps, 0, 10);
    if (decoder_)
        apply_gain(0.05f);
}

bool AudioPlayer::finished() const
{
    return decoder_ != nullptr && ended_ && resampler_->ready() == 0 &&
           rings().song[current_].available() == 0;
}

double AudioPlayer::position() const
{
    if (!decoder_)
        return 0.0;
    const std::uint64_t buffered = rings().song[current_].available();
    const std::uint64_t played = written_ > buffered ? written_ - buffered : 0;
    const double at = base_ + static_cast<double>(played) / kSampleRate;
    const double length = decoder_->duration();
    return length > 0.0 ? std::min(at, length) : at;
}

} // namespace hui::audio
