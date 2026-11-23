// EliVier Explorer - Text subtitles: SRT, WebVTT and ASS files read into a timeline of cues.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/subtitle_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>

namespace hui::video
{

namespace
{

// A cue longer than this is cut to it: a damaged file must not make every lookup scan the whole
// track.
constexpr double kMaxCueSeconds = 600.0;

// ---- encodings -------------------------------------------------------------------------------

void append_utf8(std::string &out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

bool valid_utf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (byte < 0x80)
        {
            ++i;
            continue;
        }
        if (byte >= 0xC2 && byte <= 0xDF)
        {
            length = 2;
            code = byte & 0x1F;
        }
        else if (byte >= 0xE0 && byte <= 0xEF)
        {
            length = 3;
            code = byte & 0x0F;
        }
        else if (byte >= 0xF0 && byte <= 0xF4)
        {
            length = 4;
            code = byte & 0x07;
        }
        else
        {
            return false;
        }
        if (i + length > text.size())
            return false;
        for (std::size_t k = 1; k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3F);
        }
        const bool overlong = (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);
        if (overlong || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return false;
        i += length;
    }
    return true;
}

std::string from_utf16(std::string_view bytes, bool little_endian)
{
    std::string out;
    const auto unit = [&](std::size_t at) {
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]));
        const auto second = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1]));
        return little_endian ? (first | (second << 8)) : ((first << 8) | second);
    };
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
    {
        std::uint32_t code = unit(i);
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < bytes.size())
        {
            const std::uint32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
            else
            {
                code = 0xFFFD;
            }
        }
        else if (code >= 0xD800 && code <= 0xDFFF)
        {
            code = 0xFFFD;
        }
        append_utf8(out, code);
    }
    return out;
}

// The code points of 0x80 to 0x9F in Windows-1252; the five bytes it leaves unused stay as they
// are (the C1 controls), like most decoders do.
constexpr std::uint16_t kWindows1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

std::string from_windows_1252(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 4);
    for (const char c : bytes)
    {
        const auto byte = static_cast<unsigned char>(c);
        append_utf8(out, byte >= 0x80 && byte < 0xA0 ? kWindows1252[byte - 0x80] : byte);
    }
    return out;
}

// ---- small text helpers ----------------------------------------------------------------------

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Lines split on \n, \r\n or a lone \r. The views point into `text`.
std::vector<std::string_view> split_lines(std::string_view text)
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n' || text[i] == '\r')
        {
            lines.push_back(text.substr(start, i - start));
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            start = i + 1;
        }
    }
    if (start < text.size())
        lines.push_back(text.substr(start));
    return lines;
}

// "H:MM:SS,mmm", "MM:SS.mmm" (when min_parts is 2) and "H:MM:SS.cc": a fraction is whatever digits
// follow the separator, so ".5" is half a second and ",250" a quarter.
std::optional<double> parse_time(std::string_view text, int min_parts)
{
    text = trim(text);
    double parts[3] = {0.0, 0.0, 0.0};
    int count = 0;
    std::size_t i = 0;
    while (count < 3)
    {
        std::size_t digits = 0;
        double value = 0.0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
        {
            if (++digits > 9)
                return std::nullopt;
            value = value * 10.0 + (text[i] - '0');
            ++i;
        }
        if (digits == 0)
            return std::nullopt;
        parts[count++] = value;
        if (i < text.size() && text[i] == ':' && count < 3)
        {
            ++i;
            continue;
        }
        break;
    }
    double fraction = 0.0;
    if (i < text.size() && (text[i] == ',' || text[i] == '.'))
    {
        ++i;
        double scale = 0.1;
        std::size_t digits = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
        {
            if (++digits > 9)
                return std::nullopt;
            fraction += (text[i] - '0') * scale;
            scale /= 10.0;
            ++i;
        }
        if (digits == 0)
            return std::nullopt;
    }
    if (i != text.size() || count < min_parts || count < 2)
        return std::nullopt;
    const double seconds = count == 3 ? parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]
                                      : parts[0] * 60.0 + parts[1];
    return seconds + fraction;
}

// Keeps the text at most kMaxCueTextBytes, cut where a character ends.
void limit_text(std::string &text)
{
    if (text.size() <= kMaxCueTextBytes)
        return;
    std::size_t end = kMaxCueTextBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    text.resize(end);
}

bool add_cue(SubtitleTrack &track, double start, double end, std::string_view raw_text,
             CueAlign base_align)
{
    Cue cue;
    cue.align = base_align;
    cue.text = strip_markup(raw_text, &cue.align);
    limit_text(cue.text);
    if (cue.text.empty() || !(end > start) || start < 0.0)
        return false;
    cue.start = start;
    cue.end = end;
    return track.add(std::move(cue));
}

} // namespace

// ---- the timeline ----------------------------------------------------------------------------

bool SubtitleTrack::add(Cue cue)
{
    if (cues_.size() >= static_cast<std::size_t>(kMaxCues))
        return false;
    cue.end = std::min(cue.end, cue.start + kMaxCueSeconds);
    const auto later = std::upper_bound(
        cues_.begin(), cues_.end(), cue.start,
        [](double start, const Cue &existing) { return start < existing.start; });
    for (auto at = later; at != cues_.begin();)
    {
        --at;
        if (at->start != cue.start)
            break;
        if (at->end == cue.end && at->text == cue.text)
            return false;
    }
    longest_ = std::max(longest_, cue.end - cue.start);
    cues_.insert(later, std::move(cue));
    return true;
}

void SubtitleTrack::clear()
{
    cues_.clear();
    longest_ = 0.0;
}

std::vector<const Cue *> SubtitleTrack::active_at(double seconds) const
{
    std::vector<const Cue *> active;
    auto after = std::upper_bound(
        cues_.begin(), cues_.end(), seconds,
        [](double time, const Cue &existing) { return time < existing.start; });
    while (after != cues_.begin())
    {
        --after;
        if (after->start < seconds - longest_)
            break;
        if (after->end > seconds)
            active.push_back(&*after);
    }
    std::reverse(active.begin(), active.end());
    return active;
}

// ---- encodings, markup -----------------------------------------------------------------------

std::string to_utf8(std::string_view bytes)
{
    if (starts_with(bytes, "\xEF\xBB\xBF"))
        bytes.remove_prefix(3);
    else if (starts_with(bytes, "\xFF\xFE"))
        return from_utf16(bytes.substr(2), true);
    else if (starts_with(bytes, "\xFE\xFF"))
        return from_utf16(bytes.substr(2), false);
    if (valid_utf8(bytes))
        return std::string(bytes);
    return from_windows_1252(bytes);
}

namespace
{

// The tag at text[at] ('<'), if it is one: returns the index after its '>'. A tag starts with a
// letter or '/', or is a WebVTT timestamp (digits with at least one colon). "<3" and "2 < 3" are
// not tags.
std::optional<std::size_t> tag_end(std::string_view text, std::size_t at)
{
    if (at + 1 >= text.size())
        return std::nullopt;
    const char first = text[at + 1];
    const bool named = std::isalpha(static_cast<unsigned char>(first)) != 0 || first == '/';
    const std::size_t limit = std::min(text.size(), at + 80);
    std::size_t i = at + 1;
    bool colon = false;
    bool timestamp = std::isdigit(static_cast<unsigned char>(first)) != 0;
    for (; i < limit; ++i)
    {
        const char c = text[i];
        if (c == '>')
            break;
        if (c == '<')
            return std::nullopt;
        if (timestamp)
        {
            if (c == ':')
                colon = true;
            else if (std::isdigit(static_cast<unsigned char>(c)) == 0 && c != '.')
                return std::nullopt;
        }
    }
    if (i >= limit || text[i] != '>')
        return std::nullopt;
    if (named || (timestamp && colon))
        return i + 1;
    return std::nullopt;
}

CueAlign align_from_override(char digit, CueAlign current)
{
    if (digit >= '1' && digit <= '3')
        return CueAlign::bottom;
    if (digit >= '4' && digit <= '6')
        return CueAlign::middle;
    if (digit >= '7' && digit <= '9')
        return CueAlign::top;
    return current;
}

// &amp; and friends; returns the characters used up (0 when it is not an entity we know).
std::size_t decode_entity(std::string_view text, std::size_t at, std::string &out)
{
    const std::size_t end = text.find(';', at);
    if (end == std::string_view::npos || end - at > 10)
        return 0;
    const std::string_view name = text.substr(at + 1, end - at - 1);
    if (name == "amp")
        out += '&';
    else if (name == "lt")
        out += '<';
    else if (name == "gt")
        out += '>';
    else if (name == "quot")
        out += '"';
    else if (name == "apos")
        out += '\'';
    else if (name == "nbsp")
        out += ' ';
    else if (name == "lrm" || name == "rlm")
        ; // direction marks: nothing to show
    else if (name.size() >= 2 && name[0] == '#')
    {
        std::uint32_t code = 0;
        const bool hex = name[1] == 'x' || name[1] == 'X';
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty())
            return 0;
        for (const char c : digits)
        {
            const int value = hex && std::isxdigit(static_cast<unsigned char>(c)) != 0
                                  ? (std::isdigit(static_cast<unsigned char>(c)) != 0
                                         ? c - '0'
                                         : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10)
                              : std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0'
                                                                                 : -1;
            if (value < 0)
                return 0;
            code = code * (hex ? 16 : 10) + static_cast<std::uint32_t>(value);
            if (code > 0x10FFFF)
                return 0;
        }
        if (code == 0 || (code >= 0xD800 && code <= 0xDFFF))
            return 0;
        append_utf8(out, code);
    }
    else
        return 0;
    return end - at + 1;
}

} // namespace

std::string strip_markup(std::string_view text, CueAlign *align)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '{' && i + 1 < text.size() && text[i + 1] == '\\')
        {
            const std::size_t close = text.find('}', i);
            if (close != std::string_view::npos && close - i <= 512)
            {
                const std::string_view block = text.substr(i, close - i);
                const std::size_t an = block.find("\\an");
                if (align != nullptr && an != std::string_view::npos && an + 3 < block.size())
                    *align = align_from_override(block[an + 3], *align);
                i = close;
                continue;
            }
        }
        else if (c == '<')
        {
            if (const auto end = tag_end(text, i))
            {
                i = *end - 1;
                continue;
            }
        }
        else if (c == '&')
        {
            const std::size_t used = decode_entity(text, i, out);
            if (used > 0)
            {
                i += used - 1;
                continue;
            }
        }
        else if (c == '\\' && i + 1 < text.size())
        {
            const char next = text[i + 1];
            if (next == 'N' || next == 'n')
            {
                out += '\n';
                ++i;
                continue;
            }
            if (next == 'h')
            {
                out += ' ';
                ++i;
                continue;
            }
        }
        out += c;
    }

    // Trim the ends of every line and of the whole text.
    std::string result;
    result.reserve(out.size());
    std::size_t start = 0;
    while (start <= out.size())
    {
        std::size_t end = out.find('\n', start);
        if (end == std::string::npos)
            end = out.size();
        const std::string_view line = trim(std::string_view(out).substr(start, end - start));
        if (!result.empty() || !line.empty())
        {
            if (!result.empty())
                result += '\n';
            result += line;
        }
        start = end + 1;
    }
    while (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
}

// ---- the formats -----------------------------------------------------------------------------

SubtitleTrack parse_srt(std::string_view utf8)
{
    SubtitleTrack track;
    if (starts_with(utf8, "\xEF\xBB\xBF"))
        utf8.remove_prefix(3);
    const std::vector<std::string_view> lines = split_lines(utf8);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::size_t arrow = lines[i].find("-->");
        if (arrow == std::string_view::npos)
            continue;
        const auto start = parse_time(lines[i].substr(0, arrow), 3);
        std::string_view rest = trim(lines[i].substr(arrow + 3));
        rest = rest.substr(0, rest.find(' ')); // coordinates after the end time
        const auto end = parse_time(rest, 3);
        if (!start || !end)
            continue;
        std::string raw;
        std::size_t next = i + 1;
        for (; next < lines.size() && !trim(lines[next]).empty(); ++next)
        {
            if (!raw.empty())
                raw += '\n';
            raw += lines[next];
        }
        add_cue(track, *start, *end, raw, CueAlign::bottom);
        i = next;
    }
    return track;
}

SubtitleTrack parse_webvtt(std::string_view utf8)
{
    SubtitleTrack track;
    if (starts_with(utf8, "\xEF\xBB\xBF"))
        utf8.remove_prefix(3);
    if (!starts_with(utf8, "WEBVTT"))
        return track;
    const std::vector<std::string_view> lines = split_lines(utf8);
    std::size_t i = 0;
    while (i < lines.size() && !trim(lines[i]).empty()) // the header block
        ++i;
    while (i < lines.size())
    {
        while (i < lines.size() && trim(lines[i]).empty())
            ++i;
        const std::size_t block_start = i;
        while (i < lines.size() && !trim(lines[i]).empty())
            ++i;
        if (block_start >= i)
            continue;
        if (starts_with(trim(lines[block_start]), "NOTE") ||
            starts_with(trim(lines[block_start]), "STYLE") ||
            starts_with(trim(lines[block_start]), "REGION"))
            continue;
        // The timing line is the first or the second line (after an identifier).
        std::size_t timing = block_start;
        if (lines[timing].find("-->") == std::string_view::npos && timing + 1 < i)
            ++timing;
        const std::size_t arrow = lines[timing].find("-->");
        if (arrow == std::string_view::npos)
            continue;
        const auto start = parse_time(lines[timing].substr(0, arrow), 2);
        std::string_view rest = trim(lines[timing].substr(arrow + 3));
        const std::size_t space = rest.find(' ');
        const auto end = parse_time(rest.substr(0, space), 2);
        if (!start || !end)
            continue;
        CueAlign align = CueAlign::bottom;
        const std::size_t line_setting =
            space == std::string_view::npos ? space : rest.find("line:", space);
        if (line_setting != std::string_view::npos)
        {
            std::string_view value = rest.substr(line_setting + 5);
            value = value.substr(0, value.find(' '));
            const bool percent = !value.empty() && value.back() == '%';
            const int number = std::atoi(std::string(value).c_str());
            if (percent ? number < 50 : (number >= 0 && number <= 4))
                align = CueAlign::top;
        }
        std::string raw;
        for (std::size_t line = timing + 1; line < i; ++line)
        {
            if (!raw.empty())
                raw += '\n';
            raw += lines[line];
        }
        add_cue(track, *start, *end, raw, align);
    }
    return track;
}

SubtitleTrack parse_ass(std::string_view utf8)
{
    SubtitleTrack track;
    if (starts_with(utf8, "\xEF\xBB\xBF"))
        utf8.remove_prefix(3);
    bool in_events = false;
    std::size_t fields = 10;
    std::size_t start_index = 1;
    std::size_t end_index = 2;
    std::size_t text_index = 9;
    for (const std::string_view raw_line : split_lines(utf8))
    {
        const std::string_view line = trim(raw_line);
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            in_events = lower(line) == "[events]";
            continue;
        }
        if (!in_events)
            continue;
        if (starts_with(line, "Format:"))
        {
            std::size_t index = 0;
            std::size_t at = 7;
            fields = 0;
            while (at <= line.size())
            {
                std::size_t comma = line.find(',', at);
                if (comma == std::string_view::npos)
                    comma = line.size();
                const std::string name = lower(trim(line.substr(at, comma - at)));
                if (name == "start")
                    start_index = index;
                else if (name == "end")
                    end_index = index;
                else if (name == "text")
                    text_index = index;
                ++index;
                at = comma + 1;
            }
            fields = index;
            continue;
        }
        if (!starts_with(line, "Dialogue:"))
            continue;
        // Split into `fields` parts; the last (the text) keeps its commas.
        std::vector<std::string_view> parts;
        std::size_t at = 9;
        while (parts.size() + 1 < fields)
        {
            const std::size_t comma = line.find(',', at);
            if (comma == std::string_view::npos)
                break;
            parts.push_back(line.substr(at, comma - at));
            at = comma + 1;
        }
        parts.push_back(line.substr(std::min(at, line.size())));
        if (parts.size() != fields || start_index >= fields || end_index >= fields ||
            text_index >= fields)
            continue;
        const auto start = parse_time(parts[start_index], 3);
        const auto end = parse_time(parts[end_index], 3);
        if (!start || !end)
            continue;
        const std::string_view text = parts[text_index];
        const std::size_t drawing = text.find("\\p");
        if (drawing != std::string_view::npos && drawing + 2 < text.size() &&
            text[drawing + 2] >= '1' && text[drawing + 2] <= '9')
            continue; // vector drawing, not text
        add_cue(track, *start, *end, text, CueAlign::bottom);
    }
    return track;
}

bool load_subtitle_file(std::string_view bytes, SubtitleTrack &out, std::string *error)
{
    const auto fail = [error](const char *message) {
        if (error != nullptr)
            *error = message;
        return false;
    };
    if (bytes.empty())
        return fail("The subtitle file is empty");
    if (bytes.size() > kMaxSubtitleFileBytes)
        return fail("The subtitle file is too large");
    const std::string text = to_utf8(bytes);
    const std::string head = lower(std::string_view(text).substr(0, 4096));
    SubtitleTrack track;
    if (starts_with(trim(head), "webvtt"))
        track = parse_webvtt(text);
    else if (head.find("[events]") != std::string::npos)
        track = parse_ass(text);
    else
        track = parse_srt(text);
    if (track.size() == 0)
        return fail("No subtitles were found in this file");
    out = std::move(track);
    return true;
}

} // namespace hui::video
