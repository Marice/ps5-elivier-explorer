// EliVier Explorer - The surround player: its clock, volume, pause and seek, on a fake output.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/decoder.hpp"
#include "audio/surround.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{

using namespace hui::audio;

// An output that pulls only when the test says so.
class FakeSink final : public SurroundSink
{
  public:
    bool open() override
    {
        return opened = true;
    }
    bool start(Pull value) override
    {
        pull = std::move(value);
        return true;
    }
    void close() override
    {
        pull = nullptr;
        closed = true;
    }
    std::vector<float> grain(int frames)
    {
        std::vector<float> out(static_cast<std::size_t>(frames) * kSurroundChannels, 9.0f);
        if (pull)
            pull(out.data(), frames);
        return out;
    }

    Pull pull;
    bool opened = false;
    bool closed = false;
};

// A film's sound: 0.5 on every channel, `frames` frames of it, then the end; kTryLater while
// `waiting`.
struct FakeFilm
{
    int frames = 48000;
    bool waiting = false;
    double sought = -1.0;

    int read(float *out, int wanted)
    {
        if (waiting)
            return Decoder::kTryLater;
        const int count = std::min(wanted, frames);
        for (int i = 0; i < count * kSurroundChannels; ++i)
            out[i] = 0.5f;
        frames -= count;
        return count;
    }
};

TEST(SurroundPlayer, TheClockCountsWhatTheOutputTook)
{
    FakeSink sink;
    FakeFilm film;
    SurroundPlayer player(sink, [&](float *o, int f) { return film.read(o, f); },
                          [&](double s) { film.sought = s; });
    ASSERT_TRUE(player.start());
    EXPECT_DOUBLE_EQ(player.position(), 0.0);
    const std::vector<float> grain = sink.grain(480);
    EXPECT_FLOAT_EQ(grain[0], 0.5f);
    EXPECT_FLOAT_EQ(grain[7], 0.5f); // all eight channels pass
    EXPECT_NEAR(player.position(), 0.01, 1e-9);
}

TEST(SurroundPlayer, WaitingForSoundIsSilenceAndTheClockStands)
{
    FakeSink sink;
    FakeFilm film;
    film.waiting = true;
    SurroundPlayer player(sink, [&](float *o, int f) { return film.read(o, f); },
                          [&](double s) { film.sought = s; });
    player.start();
    const std::vector<float> grain = sink.grain(480);
    EXPECT_FLOAT_EQ(grain[0], 0.0f);
    EXPECT_DOUBLE_EQ(player.position(), 0.0);
    EXPECT_FALSE(player.finished());
}

TEST(SurroundPlayer, VolumePauseSeekAndTheEnd)
{
    FakeSink sink;
    FakeFilm film;
    film.frames = 1000;
    SurroundPlayer player(sink, [&](float *o, int f) { return film.read(o, f); },
                          [&](double s) { film.sought = s; });
    player.start();
    player.set_volume(5);
    EXPECT_EQ(player.volume(), 5);
    EXPECT_FLOAT_EQ(sink.grain(100)[3], 0.5f * 0.25f); // the settings' curve: (5/10)^2

    player.set_paused(true);
    const double paused_at = player.position();
    EXPECT_FLOAT_EQ(sink.grain(100)[0], 0.0f);
    EXPECT_DOUBLE_EQ(player.position(), paused_at);
    player.set_paused(false);

    player.seek_to(12.0);
    EXPECT_DOUBLE_EQ(film.sought, 12.0);
    EXPECT_DOUBLE_EQ(player.position(), 12.0);

    sink.grain(1000); // the rest of the film (900 frames)
    sink.grain(100);  // nothing more
    EXPECT_TRUE(player.finished());
    player.stop();
    EXPECT_TRUE(sink.closed);
}

} // namespace
