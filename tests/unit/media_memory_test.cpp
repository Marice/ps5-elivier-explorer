// EliVier Explorer - What the video viewer remembers: where each film was, and the languages.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/media_memory.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

using hui::viewer::MediaMemory;
namespace fs = std::filesystem;

constexpr double kHour = 3600.0;

TEST(MediaMemory, AFilmResumesWhereItWasLeft)
{
    MediaMemory memory;
    EXPECT_EQ(memory.resume_at("/data/a.mkv", 2 * kHour), 0.0);
    memory.remember_position("/data/a.mkv", 1234.5, 2 * kHour);
    EXPECT_DOUBLE_EQ(memory.resume_at("/data/a.mkv", 2 * kHour), 1234.5);
    EXPECT_EQ(memory.resume_at("/data/b.mkv", 2 * kHour), 0.0);
}

TEST(MediaMemory, TheFirstMinuteAndTheEndAreNotRemembered)
{
    MediaMemory memory;
    memory.remember_position("/data/a.mkv", 45.0, 2 * kHour); // barely started
    EXPECT_EQ(memory.resume_at("/data/a.mkv", 2 * kHour), 0.0);
    memory.remember_position("/data/a.mkv", 3000.0, 2 * kHour);
    memory.remember_position("/data/a.mkv", 2 * kHour - 90.0, 2 * kHour); // the credits: watched
    EXPECT_EQ(memory.resume_at("/data/a.mkv", 2 * kHour), 0.0);
    EXPECT_EQ(memory.films(), 0u);
    // A short film: the last 3 % count as the end too.
    memory.remember_position("/data/short.mkv", 292.0, 300.0); // 8 s left of 300
    EXPECT_EQ(memory.resume_at("/data/short.mkv", 300.0), 0.0);
}

TEST(MediaMemory, TheLeastRecentlyUsedFilmGoesWhenItIsFull)
{
    MediaMemory memory;
    for (std::size_t i = 0; i < MediaMemory::kMaxFilms; ++i)
        memory.remember_position("/f/" + std::to_string(i), 600.0, kHour);
    memory.remember_position("/f/0", 700.0, kHour); // used again: stays
    memory.remember_position("/f/new", 600.0, kHour);
    EXPECT_EQ(memory.films(), MediaMemory::kMaxFilms);
    EXPECT_DOUBLE_EQ(memory.resume_at("/f/0", kHour), 700.0);
    EXPECT_EQ(memory.resume_at("/f/1", kHour), 0.0); // the oldest went
    EXPECT_DOUBLE_EQ(memory.resume_at("/f/new", kHour), 600.0);
}

TEST(MediaMemory, LanguagesAreWhatWasLastPicked)
{
    MediaMemory memory;
    EXPECT_EQ(memory.audio_language(), "");
    EXPECT_EQ(memory.subtitle_choice(), MediaMemory::Subtitles::unknown);
    memory.remember_audio_language("eng");
    memory.remember_subtitles("nld");
    EXPECT_EQ(memory.audio_language(), "eng");
    EXPECT_EQ(memory.subtitle_choice(), MediaMemory::Subtitles::language);
    EXPECT_EQ(memory.subtitle_language(), "nld");
    memory.remember_subtitles("");
    EXPECT_EQ(memory.subtitle_choice(), MediaMemory::Subtitles::off);
}

TEST(MediaMemory, ItIsSavedAndLoadedAgain)
{
    const fs::path folder = fs::temp_directory_path() / "hui-media-memory-test";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const std::string file = (folder / "media.bin").string();
    {
        MediaMemory memory(file);
        EXPECT_FALSE(memory.load()); // nothing yet: starts empty
        memory.remember_position("/data/ü film.mkv", 4321.0, 2 * kHour);
        memory.remember_audio_language("jpn");
        memory.remember_subtitles("eng");
        EXPECT_TRUE(memory.save());
    }
    MediaMemory again(file);
    ASSERT_TRUE(again.load());
    EXPECT_DOUBLE_EQ(again.resume_at("/data/ü film.mkv", 2 * kHour), 4321.0);
    EXPECT_EQ(again.audio_language(), "jpn");
    EXPECT_EQ(again.subtitle_language(), "eng");

    // A damaged file is not trusted: the memory starts empty.
    if (std::FILE *out = std::fopen(file.c_str(), "r+b"))
    {
        std::fseek(out, 20, SEEK_SET);
        std::fputc(0x5A, out);
        std::fclose(out);
    }
    MediaMemory damaged(file);
    EXPECT_FALSE(damaged.load());
    EXPECT_EQ(damaged.films(), 0u);
    fs::remove_all(folder);
}

} // namespace
