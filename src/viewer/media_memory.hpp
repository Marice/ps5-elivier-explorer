// EliVier Explorer - What the video viewer remembers between films and runs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where each film was left, so that it resumes there (D73), and the audio and subtitle languages
// last picked, so that the next film starts with them. Kept in one small save file (save::encode,
// written atomically); a damaged file is not trusted and the memory starts empty.
//
//   MediaMemory memory(data_root + "/media.bin");
//   memory.load();
//   double at = memory.resume_at(path, duration);      // 0: from the beginning
//   memory.remember_position(path, position, duration);
//   memory.save();

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hui::viewer
{

// Whether two language tags name the same language: files say "en", "eng", "nl", "dut" or "nld"
// for the same thing (ISO 639-1 and both forms of ISO 639-2). Case does not matter; an empty tag
// matches nothing.
bool same_language(const std::string &a, const std::string &b);

class MediaMemory
{
  public:
    static constexpr std::size_t kMaxFilms = 300;

    enum class Subtitles
    {
        unknown,  // never picked
        off,      // picked "off"
        language, // picked a language
    };

    // `file` empty: kept in memory only (tests, the PC preview).
    explicit MediaMemory(std::string file = "");

    // Reads the file. False, and empty, when there is none or it is damaged.
    bool load();
    // Writes the file when something changed. False when writing failed.
    bool save();

    // Where `film` should start: where it was left, or 0 (never seen, barely started, or seen to
    // the end).
    double resume_at(const std::string &film, double duration) const;
    // The first minute is not remembered, and the end (the last two minutes or 3 %, whichever is
    // shorter) counts as seen: the film is forgotten.
    void remember_position(const std::string &film, double seconds, double duration);
    void forget(const std::string &film);
    std::size_t films() const
    {
        return films_.size();
    }

    const std::string &audio_language() const
    {
        return audio_language_;
    }
    Subtitles subtitle_choice() const
    {
        return subtitles_;
    }
    const std::string &subtitle_language() const
    {
        return subtitle_language_;
    }
    void remember_audio_language(const std::string &language);
    // An empty language means "off".
    void remember_subtitles(const std::string &language);

  private:
    struct Film
    {
        std::string path;
        double seconds = 0.0;
        std::uint64_t used = 0; // larger is more recent
    };

    std::string file_;
    std::vector<Film> films_;
    std::uint64_t clock_ = 0;
    std::string audio_language_;
    Subtitles subtitles_ = Subtitles::unknown;
    std::string subtitle_language_;
    bool dirty_ = false;
};

} // namespace hui::viewer
