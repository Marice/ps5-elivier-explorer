// EliVier Explorer - The video viewer: pictures, sound clock, controls, subtitles and the handler.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"
#include "fake_textures.hpp"

#include "audio/mixer.hpp"
#include "explorer/local_source.hpp"
#include "viewer/registry.hpp"
#include "viewer/video_viewer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using hui::testing::ConceptFixture;
using hui::testing::FakeTextures;
using namespace hui::viewer;

const std::string kDir = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/video";
const hui::gfx::Rect kArea{96.0f, 270.0f, 1500.0f, 520.0f};

class VideoViewerTest : public ConceptFixture
{
  protected:
    std::unique_ptr<VideoViewer> open(const std::string &path, FakeTextures *textures,
                                      bool with_mixer = true)
    {
        ViewerContext context;
        context.area = kArea;
        context.fonts = &fonts_;
        context.textures = textures;
        context.mixer = with_mixer ? &mixer_ : nullptr;
        FileRef file;
        file.source = &source_;
        file.path = path;
        source_.stat(path, &file.entry);
        return std::make_unique<VideoViewer>(path.substr(path.rfind('/') + 1), &source_, path,
                                             file.entry, context);
    }

    // One frame of the app: the audio thread consumes some sound, the viewer updates, and the
    // engine's thread gets a moment to work.
    void tick(VideoViewer &viewer, const hui::InputFrame &input = idle(), bool audio = true)
    {
        if (audio)
        {
            std::int16_t out[800 * 2];
            mixer_.render(out, 800); // about one frame at 60 Hz: what the audio thread would do
        }
        hui::ui::Feedback feedback;
        viewer.update(input, 1.0f / 60.0f, feedback);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // Runs until `done` or about eight seconds have passed.
    template <typename Done>
    bool run_until(VideoViewer &viewer, Done done, bool audio = true)
    {
        for (int i = 0; i < 1600; ++i)
        {
            tick(viewer, idle(), audio);
            if (done())
                return true;
        }
        return false;
    }

    std::size_t drawn(const VideoViewer &viewer)
    {
        hui::gfx::DrawList list;
        hui::ui::Painter paint(list, fonts_, hui::ui::default_theme());
        hui::ui::Canvas canvas{list, fonts_, 0, 0.0f};
        viewer.draw(paint, canvas, kArea);
        for (const hui::gfx::Instance &instance : list.instances())
            for (const float value : instance.rect)
                EXPECT_TRUE(std::isfinite(value));
        return list.instances().size();
    }

    hui::explorer::LocalSource source_;
    hui::audio::Mixer mixer_;
};

TEST_F(VideoViewerTest, ShowsPicturesInATextureThatIsUpdatedNotRemade)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/h264-aac.mp4", &textures);
    ASSERT_TRUE(viewer->error().empty()) << viewer->error();
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->pictures_shown() >= 5; }));
    EXPECT_EQ(textures.created, 1);
    EXPECT_EQ(textures.last_width, 160);
    EXPECT_EQ(textures.last_height, 90);
    EXPECT_GE(textures.updated, 3);
    EXPECT_GE(textures.last_pixel0, 0);
    EXPECT_GT(drawn(*viewer), 0u);
    EXPECT_GT(viewer->position(), 0.1); // the sound is the clock and it moves
}

TEST_F(VideoViewerTest, CrossPausesAndResumes)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/h264-aac.mp4", &textures);
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->position() > 0.2; }));
    EXPECT_TRUE(viewer->playing());
    tick(*viewer, press(hui::Action::confirm));
    EXPECT_FALSE(viewer->playing());
    for (int i = 0; i < 6; ++i)
        tick(*viewer); // the audio thread still runs; a paused player is parked
    const double held = viewer->position();
    for (int i = 0; i < 30; ++i)
        tick(*viewer);
    EXPECT_NEAR(viewer->position(), held, 0.02);
    tick(*viewer, press(hui::Action::confirm));
    EXPECT_TRUE(viewer->playing());
    EXPECT_TRUE(run_until(*viewer, [&] { return viewer->position() > held + 0.2; }));
}

TEST_F(VideoViewerTest, PlaysToTheEndAndCrossStartsOver)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/h264-aac.mp4", &textures);
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->ended(); }));
    EXPECT_FALSE(viewer->playing());
    EXPECT_GT(drawn(*viewer), 0u); // "Ended" with the bar up
    tick(*viewer, press(hui::Action::confirm));
    EXPECT_FALSE(viewer->ended());
    EXPECT_TRUE(run_until(
        *viewer, [&] { return viewer->position() > 0.2 && viewer->position() < 1.0; }));
}

TEST_F(VideoViewerTest, TheDpadSeeksAndTheClockFollows)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/h264-aac.mp4", &textures);
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->position() > 0.2; }));
    tick(*viewer, nav(hui::Direction::right)); // +10 s, clamped to the end of a 2 s clip
    for (int i = 0; i < 20; ++i)
        tick(*viewer);
    EXPECT_GT(viewer->position(), 1.2);
    tick(*viewer, nav(hui::Direction::left)); // back 10 s: the start
    for (int i = 0; i < 20; ++i)
        tick(*viewer);
    EXPECT_LT(viewer->position(), 1.0);
}

TEST_F(VideoViewerTest, CircleClosesAndGivesEverythingBack)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/h264-aac.mp4", &textures);
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->pictures_shown() >= 1; }));
    ASSERT_EQ(textures.live.size(), 1u);
    tick(*viewer, press(hui::Action::back));
    EXPECT_TRUE(viewer->closing());
    EXPECT_TRUE(textures.live.empty());
    EXPECT_EQ(viewer->engine(), nullptr);
    EXPECT_GT(drawn(*viewer), 0u); // still drawable until the screen drops it
}

TEST_F(VideoViewerTest, AFileThatIsNotMediaSaysSoAtOnce)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/lines.srt", &textures);
    EXPECT_FALSE(viewer->error().empty());
    EXPECT_EQ(viewer->engine(), nullptr);
    EXPECT_GT(drawn(*viewer), 0u);
    tick(*viewer);
}

TEST_F(VideoViewerTest, WithoutAMixerTheFrameTimeIsTheClock)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/h264-aac.mp4", &textures, false);
    ASSERT_TRUE(viewer->error().empty()) << viewer->error();
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->pictures_shown() >= 3; }, false));
    EXPECT_GT(viewer->position(), 0.01);
    EXPECT_TRUE(run_until(*viewer, [&] { return viewer->ended(); }, false));
}

TEST_F(VideoViewerTest, WithoutATextureHostTheSoundStillPlays)
{
    const auto viewer = open(kDir + "/h264-aac.mp4", nullptr);
    ASSERT_TRUE(viewer->error().empty()) << viewer->error();
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->position() > 0.3; }));
    EXPECT_EQ(viewer->pictures_shown(), 0);
    EXPECT_GT(drawn(*viewer), 0u);
}

TEST_F(VideoViewerTest, AFileOnlyWithSoundPlaysAsSoundOnly)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/ac3-surround.mkv", &textures);
    ASSERT_TRUE(viewer->error().empty()) << viewer->error();
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->position() > 0.3; }));
    EXPECT_EQ(textures.created, 0);
    EXPECT_GT(drawn(*viewer), 0u);
}

TEST_F(VideoViewerTest, ChoosesBetweenOffTheFilesTracksAndFilesBesideIt)
{
    FakeTextures textures;
    const auto multi = open(kDir + "/multi-track.mkv", &textures);
    const std::vector<std::string> choices = multi->subtitle_choices();
    ASSERT_EQ(choices.size(), 3u); // Off, and the two tracks of the file
    EXPECT_EQ(choices[0], "Off");
    EXPECT_NE(choices[1].find("eng"), std::string::npos) << choices[1];
    EXPECT_NE(choices[2].find("nld"), std::string::npos) << choices[2];
    EXPECT_EQ(multi->subtitle_choice(), 0);
    ASSERT_TRUE(run_until(*multi, [&] { return multi->pictures_shown() >= 1; }));
    tick(*multi, press(hui::Action::west));
    EXPECT_EQ(multi->subtitle_choice(), 1);
    tick(*multi, press(hui::Action::west));
    EXPECT_EQ(multi->subtitle_choice(), 2);
    tick(*multi, press(hui::Action::west));
    EXPECT_EQ(multi->subtitle_choice(), 0); // around to off
}

TEST_F(VideoViewerTest, ASubtitleFileBesideTheFilmIsOfferedAndShown)
{
    const fs::path folder = fs::temp_directory_path() / "hui-video-viewer-test";
    fs::remove_all(folder);
    fs::create_directories(folder);
    fs::copy_file(kDir + "/h264-aac.mp4", folder / "film.mp4");
    fs::copy_file(kDir + "/lines.srt", folder / "film.en.srt");
    fs::copy_file(kDir + "/lines.srt", folder / "other.srt"); // another film's: not offered

    FakeTextures textures;
    const auto viewer = open((folder / "film.mp4").string(), &textures);
    ASSERT_TRUE(viewer->error().empty()) << viewer->error();
    const std::vector<std::string> choices = viewer->subtitle_choices();
    ASSERT_EQ(choices.size(), 2u);
    EXPECT_EQ(choices[1], "film.en.srt");

    const std::size_t without = drawn(*viewer);
    tick(*viewer, press(hui::Action::west));
    EXPECT_EQ(viewer->subtitle_choice(), 1);
    // At about 0.5 s the first line is up: the drawing has more in it than before.
    ASSERT_TRUE(run_until(
        *viewer, [&] { return viewer->position() > 0.4 && viewer->position() < 0.8; }));
    EXPECT_GT(drawn(*viewer), without);
    fs::remove_all(folder);
}

TEST_F(VideoViewerTest, SwitchingSubtitlesInTheMiddleDoesNotEndThePlayback)
{
    // The engine said "everything is decoded" before the switch; the queues were emptied by it
    // and the viewer must not take that for the end of the film.
    FakeTextures textures;
    const auto viewer = open(kDir + "/multi-track.mkv", &textures);
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->position() > 0.3; }));
    // Let the engine decode to the end of the short clip while the audio queue holds it back.
    for (int i = 0; i < 30; ++i)
        tick(*viewer);
    tick(*viewer, press(hui::Action::west));
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_FALSE(viewer->ended()) << "frame " << i;
        tick(*viewer);
    }
    EXPECT_LT(viewer->position(), 1.6);
}

TEST_F(VideoViewerTest, TheSoundTracksCanBeSwitched)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/multi-track.mkv", &textures);
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->position() > 0.2; }));
    const int first = viewer->engine()->audio_stream();
    tick(*viewer, press(hui::Action::page_next));
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->engine()->audio_stream() != first; }));
    tick(*viewer, press(hui::Action::page_prev));
    ASSERT_TRUE(run_until(*viewer, [&] { return viewer->engine()->audio_stream() == first; }));
}

TEST_F(VideoViewerTest, TheHintsFollowWhatTheFileOffers)
{
    FakeTextures textures;
    const auto plain = open(kDir + "/h264-aac.mp4", &textures);
    const auto rich = open(kDir + "/multi-track.mkv", &textures);
    EXPECT_EQ(plain->hints().size(), 3u);
    EXPECT_EQ(rich->hints().size(), 5u); // sound and subtitles can be chosen
}

TEST(VideoHandler, ClaimsVideoByExtensionAndByContent)
{
    const VideoHandler handler;
    EXPECT_EQ(handler.match(FileProbe("film.mp4", 10)), Match::extension);
    EXPECT_EQ(handler.match(FileProbe("film.MKV", 10)), Match::extension); // the probe lower-cases
    EXPECT_EQ(handler.match(FileProbe("film.mkv", 10)), Match::extension);
    EXPECT_EQ(handler.match(FileProbe("notes.txt", 10)), Match::none);

    const auto with_head = [](const std::string &name, const std::string &head) {
        return FileProbe(name, head.size(), [head] { return head; });
    };
    EXPECT_EQ(handler.match(with_head("movie", std::string("\x1A\x45\xDF\xA3rest", 8))),
              Match::content);
    EXPECT_EQ(handler.match(with_head("movie", std::string("\0\0\0\x18" "ftypmp42", 12))),
              Match::content);
    EXPECT_EQ(handler.match(with_head("movie", std::string("RIFF\x10\0\0\0" "AVI LIST", 16))),
              Match::content);
    EXPECT_EQ(handler.match(with_head("movie", "just some text, nothing else")), Match::none);
    EXPECT_STREQ(handler.id(), "video");
    EXPECT_EQ(handler.icon(), IconKind::video);
}

TEST(VideoHandler, IsInTheDefaultRegistryAndBeatsTheHexFallback)
{
    const HandlerRegistry &registry = default_registry();
    const FileProbe probe("film.mp4", 10);
    const FileHandler *best = registry.best(probe);
    ASSERT_NE(best, nullptr);
    EXPECT_STREQ(best->id(), "video");
    EXPECT_TRUE(registry.supported(probe));
    EXPECT_EQ(registry.icon_for(probe), IconKind::video);
}

} // namespace
