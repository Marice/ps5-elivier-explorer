// EliVier Explorer - A text file as lines of columns: what the text viewer shows, without drawing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/text_document.hpp"

#include <algorithm>

namespace hui::viewer
{

namespace
{

constexpr std::uint32_t kReplacement = 0xFFFD;
constexpr std::uint32_t kMiddleDot = 0x00B7;

void append_utf8(std::string *out, std::uint32_t cp)
{
    if (cp < 0x80)
    {
        out->push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes one code point at p; returns its length in bytes, or 0 when the bytes are not valid UTF-8
// (overlong forms, surrogates and values past U+10FFFF included). *incomplete is set when the bytes
// are a valid start cut off by the end.
int decode_utf8(const unsigned char *p, const unsigned char *end, std::uint32_t *cp,
                bool *incomplete)
{
    *incomplete = false;
    const unsigned char first = *p;
    if (first < 0x80)
    {
        *cp = first;
        return 1;
    }
    int length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF)
    {
        length = 2;
        value = first & 0x1F;
        minimum = 0x80;
    }
    else if (first >= 0xE0 && first <= 0xEF)
    {
        length = 3;
        value = first & 0x0F;
        minimum = 0x800;
    }
    else if (first >= 0xF0 && first <= 0xF4)
    {
        length = 4;
        value = first & 0x07;
        minimum = 0x10000;
    }
    else
    {
        return 0;
    }
    for (int i = 1; i < length; ++i)
    {
        if (p + i >= end)
        {
            *incomplete = true;
            return 0;
        }
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (p[i] & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return 0;
    *cp = value;
    return length;
}

bool is_control(std::uint32_t cp)
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

// UTF-16 (little or big endian) to UTF-8; a lone surrogate becomes the replacement character.
std::string utf16_to_utf8(const std::string &bytes, std::size_t from, bool little)
{
    std::string out;
    out.reserve(bytes.size());
    const auto unit = [&](std::size_t i)
    {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = static_cast<unsigned char>(bytes[i + 1]);
        return static_cast<std::uint32_t>(little ? (b << 8) | a : (a << 8) | b);
    };
    for (std::size_t i = from; i + 1 < bytes.size(); i += 2)
    {
        std::uint32_t cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (i + 3 < bytes.size() && unit(i + 2) >= 0xDC00 && unit(i + 2) <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (unit(i + 2) - 0xDC00);
                i += 2;
            }
            else
            {
                cp = kReplacement;
            }
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            cp = kReplacement;
        }
        append_utf8(&out, cp);
    }
    return out;
}

} // namespace

bool looks_like_text(std::string_view head)
{
    if (head.empty())
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(head.data());
    if (head.size() >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return true;
    if (head.size() >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)))
        return true;
    const unsigned char *end = p + head.size();
    std::size_t controls = 0;
    std::size_t points = 0;
    while (p < end)
    {
        std::uint32_t cp = 0;
        bool incomplete = false;
        const int length = decode_utf8(p, end, &cp, &incomplete);
        if (length == 0)
        {
            if (incomplete)
                break; // a character cut by the end of the head
            return false;
        }
        if (cp == 0)
            return false;
        ++points;
        if (is_control(cp) && cp != '\t' && cp != '\n' && cp != '\r' && cp != '\f')
            ++controls;
        p += length;
    }
    return points > 0 && controls * 20 <= points; // up to 5 % stray control characters
}

void TextDocument::load(std::string bytes, bool truncated, int tab_width)
{
    start_.clear();
    end_.clear();
    max_columns_ = 0;
    truncated_ = truncated;
    tab_width_ = tab_width < 1 ? 1 : tab_width;
    encoding_ = TextEncoding::utf8;
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 3 && b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF)
    {
        encoding_ = TextEncoding::utf8_bom;
        data_ = bytes.substr(3);
    }
    else if (bytes.size() >= 2 && b(0) == 0xFF && b(1) == 0xFE)
    {
        encoding_ = TextEncoding::utf16_le;
        data_ = utf16_to_utf8(bytes, 2, true);
    }
    else if (bytes.size() >= 2 && b(0) == 0xFE && b(1) == 0xFF)
    {
        encoding_ = TextEncoding::utf16_be;
        data_ = utf16_to_utf8(bytes, 2, false);
    }
    else
    {
        data_ = std::move(bytes);
    }

    // Lines end at \n, \r\n or a lone \r; the end of the last line needs no break.
    std::size_t begin = 0;
    const std::size_t size = data_.size();
    for (std::size_t i = 0; i < size; ++i)
    {
        const char c = data_[i];
        if (c != '\n' && c != '\r')
            continue;
        start_.push_back(static_cast<std::uint32_t>(begin));
        end_.push_back(static_cast<std::uint32_t>(i));
        if (c == '\r' && i + 1 < size && data_[i + 1] == '\n')
            ++i;
        begin = i + 1;
    }
    if (begin < size)
    {
        start_.push_back(static_cast<std::uint32_t>(begin));
        end_.push_back(static_cast<std::uint32_t>(size));
    }
    for (int i = 0; i < line_count(); ++i)
        max_columns_ = std::max(max_columns_, columns_of(i));
}

template <typename Emit> void TextDocument::walk(int index, Emit emit) const
{
    if (index < 0 || index >= line_count())
        return;
    const auto *p = reinterpret_cast<const unsigned char *>(data_.data()) +
                    start_[static_cast<std::size_t>(index)];
    const auto *end = reinterpret_cast<const unsigned char *>(data_.data()) +
                      end_[static_cast<std::size_t>(index)];
    int column = 0;
    while (p < end)
    {
        std::uint32_t cp = 0;
        bool incomplete = false;
        int length = decode_utf8(p, end, &cp, &incomplete);
        if (length == 0)
        {
            cp = kReplacement; // one bad byte at a time, so a stray one does not eat its neighbours
            length = 1;
        }
        p += length;
        if (cp == '\t')
        {
            const int stop = tab_width_ - column % tab_width_;
            for (int i = 0; i < stop; ++i)
            {
                if (!emit(static_cast<std::uint32_t>(' ')))
                    return;
                ++column;
            }
            continue;
        }
        if (is_control(cp))
            cp = kMiddleDot;
        if (!emit(cp))
            return;
        ++column;
    }
}

int TextDocument::columns_of(int index) const
{
    int columns = 0;
    walk(index,
         [&columns](std::uint32_t)
         {
             ++columns;
             return true;
         });
    return columns;
}

std::string TextDocument::display_line(int index) const
{
    std::string out;
    walk(index,
         [&out](std::uint32_t cp)
         {
             append_utf8(&out, cp);
             return true;
         });
    return out;
}

std::string TextDocument::display_slice(int index, int first_column, int count) const
{
    std::string out;
    if (count <= 0)
        return out;
    int column = 0;
    walk(index,
         [&](std::uint32_t cp)
         {
             if (column >= first_column + count)
                 return false;
             if (column >= first_column)
                 append_utf8(&out, cp);
             ++column;
             return true;
         });
    return out;
}

} // namespace hui::viewer
