// EliVier Explorer - The playback engine: pictures, sound and subtitles in time, driven by pump().
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_fixtures.hpp"

#include "video/playback_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace hui::video;
using hui::testing::reader_for;

std::vector<std::uint8_t> read_all(const std::string &name)
{
    return hui::testing::read_video_fixture(name);
}

std::unique_ptr<PlaybackEngine> open_engine(const std::vector<std::uint8_t> &bytes,
                                            std::string *error, const EngineOptions &options = {})
{
    return PlaybackEngine::open(reader_for(bytes), bytes.size(), options, error);
}

// Runs the engine until it has nothing left to do (bounded, so a bug cannot hang the test).
void run_to_idle(PlaybackEngine &engine, int limit = 20000)
{
    for (int i = 0; i < limit && engine.pump(); ++i)
    {
    }
}

int stream_of(const PlaybackEngine &engine, StreamKind kind, int nth = 0)
{
    int seen = 0;
    for (const StreamInfo &stream : engine.info().streams)
        if (stream.kind == kind && seen++ == nth)
            return stream.index;
    return -1;
}

// What the sound queue holds right now, and no more: a read past it is "try later", not sound.
std::vector<float> read_queued(PlaybackEngine &engine, hui::audio::Decoder &source)
{
    std::vector<float> all;
    std::vector<float> block(1152 * 2);
    int left = static_cast<int>(engine.stats().audio_seconds_ready * 48000.0 + 0.5);
    while (left > 0)
    {
        const int got = source.read(block.data(), std::min(left, 1152));
        if (got <= 0)
            break;
        all.insert(all.end(), block.begin(), block.begin() + got * 2);
        left -= got;
    }
    return all;
}

// Reads the way the player does, up to `max_frames`; stops at "try later" and at the end.
std::vector<float> read_sound(hui::audio::Decoder &source, int max_frames)
{
    std::vector<float> all;
    std::vector<float> block(1152 * 2);
    int total = 0;
    while (total < max_frames)
    {
        const int got = source.read(block.data(), 1152);
        if (got <= 0)
            break;
        all.insert(all.end(), block.begin(), block.begin() + got * 2);
        total += got;
    }
    return all;
}

TEST(PlaybackEngine, OpensAFileAndReportsItsStreams)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    EXPECT_TRUE(engine->has_video());
    EXPECT_TRUE(engine->has_audio());
    EXPECT_EQ(engine->video_stream(), 0);
    EXPECT_EQ(engine->audio_stream(), 1);
    EXPECT_EQ(engine->subtitle_stream(), -1);
    EXPECT_NEAR(engine->duration(), 2.0, 0.05);
    EXPECT_TRUE(engine->video_error().empty());
}

TEST(PlaybackEngine, BigPicturesAreQueuedWithinTheMemoryBudget)
{
    // The console's heap is 128 MiB; a 1080p picture at 16 bits a channel is 16.6 MB, and ten of
    // them crashed it (docs/decision-log.md, D54). Here a picture is 160 x 90 x 8 bytes.
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    EngineOptions options;
    options.picture_budget_bytes = 3u * 160u * 90u * 8u; // room for three pictures in all
    auto engine = open_engine(bytes, &error, options);
    ASSERT_TRUE(engine) << error;
    run_to_idle(*engine);
    EXPECT_EQ(engine->stats().pictures_ready, 2); // the third is the one held for the queue
    EXPECT_LE(engine->stats().pictures_owned, 3);

    VideoPicture picture;
    ASSERT_TRUE(engine->take_picture(0.0, picture));
    run_to_idle(*engine);
    EXPECT_LE(engine->stats().pictures_owned, 3); // a returned buffer is not kept beyond it

    auto roomy = open_engine(bytes, &error); // small pictures: the queue's own size rules
    ASSERT_TRUE(roomy) << error;
    run_to_idle(*roomy);
    EXPECT_EQ(roomy->stats().pictures_ready, EngineOptions{}.picture_queue);
}

TEST(PlaybackEngine, PicturesComeOutInTimeAndLookLikeRealPictures)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    run_to_idle(*engine);
    ASSERT_GT(engine->stats().pictures_ready, 0);

    VideoPicture picture;
    ASSERT_TRUE(engine->take_picture(0.0, picture));
    EXPECT_EQ(picture.width, 160);
    EXPECT_EQ(picture.height, 90);
    ASSERT_EQ(picture.rgba.size(), 160u * 90u * 4u);
    EXPECT_NEAR(picture.pts, 0.0, 0.05);
    long sum = 0;
    for (std::size_t i = 0; i < picture.rgba.size(); i += 4)
    {
        sum += (picture.rgba[i] + picture.rgba[i + 1] + picture.rgba[i + 2]) / 257;
        EXPECT_EQ(picture.rgba[i + 3], 65535);
    }
    const double mean = static_cast<double>(sum) / (160.0 * 90.0 * 3.0);
    EXPECT_GT(mean, 20.0); // a test pattern, not black
    EXPECT_LT(mean, 235.0);

    // The clock moves on and later pictures follow, never earlier ones.
    double last = picture.pts;
    for (double clock = 0.1; clock < 1.9; clock += 0.1)
    {
        run_to_idle(*engine);
        if (engine->take_picture(clock, picture))
        {
            EXPECT_GE(picture.pts, last);
            EXPECT_LE(picture.pts, clock + 0.05);
            last = picture.pts;
        }
    }
    EXPECT_GT(last, 1.0);
}

TEST(PlaybackEngine, TheQueuesAreBoundedSoTheEngineWaitsForTheGameThread)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    EngineOptions options;
    options.picture_queue = 3;
    options.audio_queue_seconds = 0.5;
    auto engine = open_engine(bytes, &error, options);
    ASSERT_TRUE(engine) << error;
    run_to_idle(*engine);
    const PlaybackEngine::Stats stats = engine->stats();
    EXPECT_LE(stats.pictures_ready, 3);
    EXPECT_LE(stats.audio_seconds_ready, 0.5 + 0.1); // one chunk may overshoot
    EXPECT_FALSE(engine->ended()); // it stopped because the queues were full, not at the end
    EXPECT_FALSE(engine->pump());  // and it stays idle until something is taken
}

TEST(PlaybackEngine, AFullPictureQueueDoesNotKeepTheSoundFromBeingRead)
{
    // The clock is the sound. If reading waited for the picture queue, a file that keeps its sound
    // behind its pictures would stop with a full queue, the sound would never come, the clock
    // would never move and no picture would ever fall due.
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    EngineOptions options;
    options.picture_queue = 1; // full after the first picture, and nobody takes it
    auto engine = open_engine(bytes, &error, options);
    ASSERT_TRUE(engine) << error;
    run_to_idle(*engine);
    EXPECT_EQ(engine->stats().pictures_ready, 1);
    EXPECT_GT(engine->stats().audio_seconds_ready, 0.5); // the sound was read and decoded anyway
}

TEST(PlaybackEngine, TheSoundSourceGivesStereoAndEndsWithTheFile)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    auto source = engine->make_audio_source();
    ASSERT_TRUE(source);
    EXPECT_EQ(source->rate(), 48000);
    EXPECT_FALSE(engine->make_audio_source()); // only one at a time

    // Keep the queues moving the way the game thread would: pump, take pictures, read sound.
    std::vector<float> all;
    VideoPicture picture;
    for (int round = 0; round < 400; ++round)
    {
        run_to_idle(*engine);
        engine->take_picture(1e9, picture);
        std::vector<float> chunk = read_queued(*engine, *source);
        all.insert(all.end(), chunk.begin(), chunk.end());
        if (engine->ended() && chunk.empty())
            break;
    }
    EXPECT_TRUE(engine->ended());
    const int frames = static_cast<int>(all.size() / 2);
    EXPECT_NEAR(frames, 2 * 48000, 0.05 * 2 * 48000);
    EXPECT_EQ(engine->stats().starved_reads, 0u); // only what was queued was read
    // At the very end the source says so: 0, not silence.
    std::vector<float> block(1152 * 2);
    EXPECT_EQ(source->read(block.data(), 1152), 0);
}

TEST(PlaybackEngine, ReadingBeforeAnythingIsDecodedSaysTryLaterNotTheEnd)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    auto source = engine->make_audio_source();
    std::vector<float> block(1152 * 2, 1.0f);
    // Not 0 (the player would stop) and not silence (the player would write it and the sound
    // would trail the picture): "ask again", and the player's clock waits.
    EXPECT_EQ(source->read(block.data(), 1152), hui::audio::Decoder::kTryLater);
    EXPECT_EQ(engine->stats().starved_reads, 1u);
    for (const float v : block)
        ASSERT_EQ(v, 1.0f); // nothing was written into the buffer
}

TEST(PlaybackEngine, ASeekIsFollowedByTryLaterUntilTheSoundOfTheNewPlaceIsThere)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    auto source = engine->make_audio_source();
    run_to_idle(*engine);
    std::vector<float> block(1152 * 2);
    ASSERT_GT(source->read(block.data(), 1152), 0);
    engine->seek(1.0);
    EXPECT_EQ(source->read(block.data(), 1152), hui::audio::Decoder::kTryLater); // nothing stale
    run_to_idle(*engine);
    EXPECT_GT(source->read(block.data(), 1152), 0); // and then the sound from the new place
}

TEST(PlaybackEngine, SeekingDropsWhatWasQueuedAndStartsAtTheTarget)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    auto source = engine->make_audio_source();
    run_to_idle(*engine);
    ASSERT_GT(engine->stats().pictures_ready, 0);

    engine->seek(1.2);
    EXPECT_EQ(engine->stats().pictures_ready, 0); // at once, before the engine has caught up
    EXPECT_EQ(engine->stats().audio_seconds_ready, 0.0);
    run_to_idle(*engine);

    VideoPicture picture;
    ASSERT_TRUE(engine->take_picture(1.3, picture));
    EXPECT_GE(picture.pts, 1.15); // nothing from before the target is ever shown
    EXPECT_LE(picture.pts, 1.4);
    // The sound starts at the target: about 0.8 s are left of the two seconds.
    std::vector<float> sound;
    for (int round = 0; round < 200; ++round)
    {
        run_to_idle(*engine);
        engine->take_picture(1e9, picture);
        std::vector<float> chunk = read_queued(*engine, *source);
        if (chunk.empty() && engine->ended())
            break;
        sound.insert(sound.end(), chunk.begin(), chunk.end());
    }
    const double seconds = static_cast<double>(sound.size() / 2) / 48000.0;
    EXPECT_NEAR(seconds, 0.8, 0.12);
}

TEST(PlaybackEngine, SubtitlesOfTheChosenTrackAppearAtTheirTime)
{
    const auto bytes = read_all("multi-track.mkv");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    EXPECT_TRUE(engine->subtitles_at(0.5).empty()); // off by default

    const int srt = stream_of(*engine, StreamKind::subtitle, 0);
    const int ass = stream_of(*engine, StreamKind::subtitle, 1);
    ASSERT_GE(srt, 0);
    engine->select_subtitle(srt, 0.0);
    run_to_idle(*engine);
    VideoPicture picture;
    for (int i = 0; i < 20; ++i)
    {
        engine->take_picture(1e9, picture);
        run_to_idle(*engine);
    }
    EXPECT_EQ(engine->subtitle_stream(), srt);
    const auto at_half = engine->subtitles_at(0.5);
    ASSERT_EQ(at_half.size(), 1u);
    EXPECT_EQ(at_half[0].text, "Hello, this is the first line.");
    EXPECT_TRUE(engine->subtitles_at(0.95).empty());
    ASSERT_EQ(engine->subtitles_at(1.5).size(), 1u);
    EXPECT_EQ(engine->subtitles_at(1.5)[0].text, "And this is the second line.");

    engine->select_subtitle(ass, 0.0);
    for (int i = 0; i < 20; ++i)
    {
        run_to_idle(*engine);
        engine->take_picture(1e9, picture);
    }
    ASSERT_EQ(engine->subtitles_at(0.5).size(), 1u);
    EXPECT_EQ(engine->subtitles_at(0.5)[0].text, "Styled line one");

    engine->select_subtitle(-1, 0.0); // off again
    run_to_idle(*engine);
    EXPECT_TRUE(engine->subtitles_at(0.5).empty());
    EXPECT_EQ(engine->subtitle_stream(), -1);
}

double pitch_of(const std::vector<float> &stereo)
{
    const int frames = static_cast<int>(stereo.size() / 2);
    const int margin = 4800;
    if (frames < 2 * margin)
        return 0.0;
    int crossings = 0;
    for (int i = margin + 1; i < frames - margin; ++i)
        if (stereo[static_cast<std::size_t>(2 * (i - 1))] < 0.0f &&
            stereo[static_cast<std::size_t>(2 * i)] >= 0.0f)
            ++crossings;
    return crossings / (static_cast<double>(frames - 2 * margin) / 48000.0);
}

TEST(PlaybackEngine, SwitchingTheSoundTrackChangesTheTone)
{
    const auto bytes = read_all("multi-track.mkv");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    auto source = engine->make_audio_source();
    const int second = stream_of(*engine, StreamKind::audio, 1);
    ASSERT_GE(second, 0);
    EXPECT_EQ(engine->audio_stream(), stream_of(*engine, StreamKind::audio, 0));

    engine->select_audio(second, 0.0);
    std::vector<float> sound;
    VideoPicture picture;
    for (int round = 0; round < 200; ++round)
    {
        run_to_idle(*engine);
        engine->take_picture(1e9, picture);
        std::vector<float> chunk = read_queued(*engine, *source);
        if (chunk.empty() && engine->ended())
            break;
        sound.insert(sound.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(engine->audio_stream(), second);
    EXPECT_NEAR(pitch_of(sound), 880.0, 880.0 * 0.04);
}

TEST(PlaybackEngine, AFileWithOnlySoundPlaysWithoutAPicture)
{
    const auto bytes = read_all("ac3-surround.mkv");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    EXPECT_FALSE(engine->has_video());
    EXPECT_TRUE(engine->has_audio());
    auto source = engine->make_audio_source();
    ASSERT_TRUE(source);
    run_to_idle(*engine);
    VideoPicture picture;
    EXPECT_FALSE(engine->take_picture(1.0, picture));
    std::vector<float> sound = read_sound(*source, 48000);
    EXPECT_GT(sound.size(), 1000u);
}

TEST(PlaybackEngine, AFileWithOnlyAPictureHasNoSoundSource)
{
    const auto bytes = read_all("av1.mkv");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    EXPECT_TRUE(engine->has_video());
    EXPECT_FALSE(engine->has_audio());
    EXPECT_FALSE(engine->make_audio_source());
    run_to_idle(*engine);
    VideoPicture picture;
    EXPECT_TRUE(engine->take_picture(0.0, picture));
}

TEST(PlaybackEngine, APictureThatIsTooLargeStillLetsTheSoundPlay)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    EngineOptions options;
    options.limits.max_pixels = 100;
    auto engine = open_engine(bytes, &error, options);
    ASSERT_TRUE(engine) << error;
    EXPECT_FALSE(engine->has_video());
    EXPECT_NE(engine->video_error().find("too large"), std::string::npos) << engine->video_error();
    EXPECT_TRUE(engine->has_audio());
}

TEST(PlaybackEngine, RefusesAFileThatIsNotMedia)
{
    const std::vector<std::uint8_t> zeros(4096, 0);
    std::string error;
    EXPECT_FALSE(open_engine(zeros, &error));
    EXPECT_FALSE(error.empty());
}

TEST(PlaybackEngine, ARunningThreadDeliversTheSameAndStopsCleanly)
{
    const auto bytes = read_all("h264-aac.mp4");
    std::string error;
    auto engine = open_engine(bytes, &error);
    ASSERT_TRUE(engine) << error;
    auto source = engine->make_audio_source();
    engine->start();

    // The game thread's side: ask for pictures and sound for up to ten seconds.
    VideoPicture picture;
    int pictures = 0;
    std::int64_t sound_frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::vector<float> block(1152 * 2);
    while (std::chrono::steady_clock::now() < deadline && !(engine->ended() && pictures > 0))
    {
        if (engine->take_picture(1e9, picture))
            ++pictures;
        const int got = source->read(block.data(), 1152);
        sound_frames += got;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (got == 0 && engine->ended())
            break;
    }
    engine->stop();
    EXPECT_GT(pictures, 0);
    EXPECT_GT(sound_frames, 48000);
    engine->stop(); // a second stop is harmless
}

TEST(PlaybackEngine, SurvivesDamagedFilesWhateverTheyDo)
{
    // Flipped bytes: the picture or the sound may be wrong or missing. Nothing may crash or hang.
    const char *names[] = {"h264-aac.mp4", "multi-track.mkv", "vp9-opus.webm"};
    std::uint32_t state = 31337;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (const char *name : names)
    {
        const auto original = read_all(name);
        for (int round = 0; round < 10; ++round)
        {
            std::vector<std::uint8_t> damaged = original;
            for (int flips = 0; flips < 4 + round; ++flips)
                damaged[original.size() / 4 + next() % (damaged.size() - original.size() / 4)] ^=
                    static_cast<std::uint8_t>(1u << (next() % 8));
            std::string error;
            auto engine = open_engine(damaged, &error);
            if (!engine)
                continue;
            engine->select_subtitle(stream_of(*engine, StreamKind::subtitle, 0), 0.0);
            auto source = engine->make_audio_source();
            VideoPicture picture;
            for (int i = 0; i < 300; ++i)
            {
                engine->pump();
                engine->take_picture(1e9, picture);
                if (source)
                    read_sound(*source, 4800);
                (void)engine->subtitles_at(0.5);
            }
            engine->seek(0.7);
            run_to_idle(*engine, 2000);
        }
    }
    SUCCEED();
}

} // namespace
