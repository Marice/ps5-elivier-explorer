// EliVier Explorer - What the video viewer remembers between films and runs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/media_memory.hpp"

#include "core/save_file.hpp"

#include <algorithm>
#include <cstring>

namespace hui::viewer
{

namespace
{

constexpr std::uint16_t kVersion = 1;
constexpr double kRememberAfter = 60.0;  // seconds watched before a position is kept
constexpr double kEndSeconds = 120.0;    // the last two minutes count as the end...
constexpr double kEndFraction = 0.03;    // ...or the last 3 %, whichever is shorter
constexpr std::size_t kMaxText = 4096;   // a path or a language longer than this is damage

void put_u64(std::string &out, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void put_text(std::string &out, const std::string &text)
{
    put_u64(out, text.size());
    out += text;
}

struct Reader
{
    std::string_view data;
    std::size_t at = 0;
    bool ok = true;

    std::uint64_t u64()
    {
        if (at + 8 > data.size())
        {
            ok = false;
            return 0;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[at + i])) << (8 * i);
        at += 8;
        return value;
    }

    std::string text()
    {
        const std::uint64_t size = u64();
        if (!ok || size > kMaxText || at + size > data.size())
        {
            ok = false;
            return {};
        }
        std::string value(data.substr(at, size));
        at += size;
        return value;
    }
};

bool at_the_end(double seconds, double duration)
{
    if (duration <= 0.0)
        return false;
    return duration - seconds <= std::min(kEndSeconds, duration * kEndFraction);
}

} // namespace

MediaMemory::MediaMemory(std::string file) : file_(std::move(file))
{
}

bool MediaMemory::load()
{
    films_.clear();
    audio_language_.clear();
    subtitle_language_.clear();
    subtitles_ = Subtitles::unknown;
    clock_ = 0;
    dirty_ = false;
    std::string data;
    if (file_.empty() || !save::read_file(file_, &data, 1u << 20))
        return false;
    const save::Decoded decoded = save::decode(save::Kind::media, data);
    if (!decoded.ok || decoded.version != kVersion)
        return false;
    Reader in{decoded.payload};
    std::string audio = in.text();
    const std::uint64_t subtitles = in.u64();
    std::string subtitle_language = in.text();
    const std::uint64_t count = in.u64();
    std::vector<Film> films;
    for (std::uint64_t i = 0; in.ok && i < count && i < kMaxFilms; ++i)
    {
        Film film;
        film.path = in.text();
        const std::uint64_t bits = in.u64();
        std::memcpy(&film.seconds, &bits, sizeof(film.seconds));
        film.used = in.u64();
        films.push_back(std::move(film));
    }
    if (!in.ok || count > kMaxFilms || subtitles > 2 || in.at != decoded.payload.size())
        return false;
    audio_language_ = std::move(audio);
    subtitles_ = static_cast<Subtitles>(subtitles);
    subtitle_language_ = std::move(subtitle_language);
    films_ = std::move(films);
    for (const Film &film : films_)
        clock_ = std::max(clock_, film.used);
    return true;
}

bool MediaMemory::save()
{
    if (file_.empty() || !dirty_)
        return true;
    std::string payload;
    put_text(payload, audio_language_);
    put_u64(payload, static_cast<std::uint64_t>(subtitles_));
    put_text(payload, subtitle_language_);
    put_u64(payload, films_.size());
    for (const Film &film : films_)
    {
        put_text(payload, film.path);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &film.seconds, sizeof(bits));
        put_u64(payload, bits);
        put_u64(payload, film.used);
    }
    if (!save::write_atomic(file_, save::encode(save::Kind::media, kVersion, payload)).empty())
        return false;
    dirty_ = false;
    return true;
}

double MediaMemory::resume_at(const std::string &film, double duration) const
{
    for (const Film &known : films_)
        if (known.path == film)
            return known.seconds >= kRememberAfter && !at_the_end(known.seconds, duration)
                       ? known.seconds
                       : 0.0;
    return 0.0;
}

void MediaMemory::remember_position(const std::string &film, double seconds, double duration)
{
    if (seconds < kRememberAfter || at_the_end(seconds, duration))
    {
        forget(film);
        return;
    }
    dirty_ = true;
    for (Film &known : films_)
        if (known.path == film)
        {
            known.seconds = seconds;
            known.used = ++clock_;
            return;
        }
    if (films_.size() >= kMaxFilms)
        films_.erase(std::min_element(films_.begin(), films_.end(),
                                      [](const Film &a, const Film &b) { return a.used < b.used; }));
    films_.push_back({film, seconds, ++clock_});
}

void MediaMemory::forget(const std::string &film)
{
    const auto old = films_.size();
    films_.erase(std::remove_if(films_.begin(), films_.end(),
                                [&](const Film &known) { return known.path == film; }),
                 films_.end());
    dirty_ = dirty_ || films_.size() != old;
}

void MediaMemory::remember_audio_language(const std::string &language)
{
    if (language.empty() || language == audio_language_)
        return;
    audio_language_ = language;
    dirty_ = true;
}

void MediaMemory::remember_subtitles(const std::string &language)
{
    const Subtitles choice = language.empty() ? Subtitles::off : Subtitles::language;
    if (choice == subtitles_ && language == subtitle_language_)
        return;
    subtitles_ = choice;
    subtitle_language_ = language;
    dirty_ = true;
}

} // namespace hui::viewer
