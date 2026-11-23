// EliVier Explorer - Text subtitles: SRT, WebVTT and ASS files read into a timeline of cues.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No FFmpeg and no drawing here: plain text in, a sorted list of cues out. Styling is dropped
// (italics, colours, fonts); only the line breaks and a vertical position survive. Styled ASS
// will go through libass later; this is what every text track falls back to. Subtitle files are
// untrusted input: their size, the number of cues and the length of one cue are capped.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hui::video
{

inline constexpr int kMaxCues = 200000;
inline constexpr std::size_t kMaxSubtitleFileBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxCueTextBytes = 4096;

enum class CueAlign
{
    bottom,
    middle,
    top,
};

struct Cue
{
    double start = 0.0; // seconds, as in the file; the player adds the user's delay
    double end = 0.0;
    std::string text; // UTF-8, lines separated by '\n', no markup
    CueAlign align = CueAlign::bottom;
};

// Cues in order of start time. Seeking makes the demuxer deliver a packet a second time, so a cue
// that is already there (same times and text) is not added again.
class SubtitleTrack
{
  public:
    // False when it was a duplicate or the limit is reached.
    bool add(Cue cue);
    void clear();
    std::size_t size() const
    {
        return cues_.size();
    }
    const std::vector<Cue> &cues() const
    {
        return cues_;
    }
    // The cues showing at `seconds` (start inclusive, end exclusive), in order of start.
    std::vector<const Cue *> active_at(double seconds) const;

  private:
    std::vector<Cue> cues_;
    double longest_ = 0.0; // the longest cue, so that a lookup knows how far back to look
};

// One cue from raw text (markup, entities and ASS line breaks are handled as in the file readers):
// the text is cleaned and cut to kMaxCueTextBytes, and the cue is added to the track. False when
// it was empty, ends before it starts, or was a duplicate. `align` is the position when the text
// itself says none.
bool add_cue(SubtitleTrack &track, double start, double end, std::string_view raw_text,
             CueAlign align = CueAlign::bottom);

// Text file bytes to UTF-8: a byte order mark is read (UTF-8, UTF-16 either way), bytes that are
// valid UTF-8 are kept as they are, and anything else is taken as Windows-1252 (the usual
// encoding of old Western European subtitle files). Other code pages are not guessed.
std::string to_utf8(std::string_view bytes);

// Drops HTML-like tags (<i>, <font ...>, <v Name>, <00:00:01.000>) and ASS overrides ({\i1}),
// decodes entities (&amp; &nbsp; &#233;) and turns ASS line breaks (\N) into '\n'. Text such as
// "I <3 you" and "{music}" stays. `align` (optional) is set from {\anN}.
std::string strip_markup(std::string_view text, CueAlign *align = nullptr);

SubtitleTrack parse_srt(std::string_view utf8);
SubtitleTrack parse_webvtt(std::string_view utf8);
SubtitleTrack parse_ass(std::string_view utf8);

// Reads a whole file's bytes (any supported encoding), deciding the format by its content: a
// WEBVTT header, an [Events] section, or else SRT. False and `error` when no cue was found.
bool load_subtitle_file(std::string_view bytes, SubtitleTrack &out, std::string *error);

} // namespace hui::video
