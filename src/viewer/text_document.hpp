// EliVier Explorer - A text file as lines of columns: what the text viewer shows, without drawing.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bytes of a file (UTF-8, or UTF-16 with a byte order mark, which is converted) are split into
// lines once; a line is turned into what is shown only when it is asked for, so a file of a hundred
// thousand lines costs the lines in view. Columns are code points: the viewer's face is monospace.
//
// Shown as: a tab becomes spaces up to the next tab stop, a control character becomes a middle dot,
// and a byte that is not valid UTF-8 becomes U+FFFD (the replacement character).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::viewer
{

enum class TextEncoding : std::uint8_t
{
    utf8,
    utf8_bom,
    utf16_le,
    utf16_be,
};

// Whether the first bytes of a file look like text: a byte order mark, or no NUL and mostly
// printable bytes that are valid UTF-8 (a character cut off by the end of the head is allowed).
bool looks_like_text(std::string_view head);

class TextDocument
{
  public:
    // `bytes` is the start of the file; `truncated` says there is more of it than this.
    void load(std::string bytes, bool truncated, int tab_width = 4);

    int line_count() const
    {
        return static_cast<int>(start_.size());
    }
    // The widest line, in columns, tabs expanded.
    int max_columns() const
    {
        return max_columns_;
    }
    bool truncated() const
    {
        return truncated_;
    }
    TextEncoding encoding() const
    {
        return encoding_;
    }
    // The size of the text as UTF-8 (the byte order mark is not counted).
    std::size_t bytes() const
    {
        return data_.size();
    }

    // The whole line as shown; "" for a line that does not exist.
    std::string display_line(int index) const;
    // Columns [first, first + count) of the line as shown, as UTF-8; shorter at the end of the
    // line.
    std::string display_slice(int index, int first_column, int count) const;
    int columns_of(int index) const;

  private:
    // Calls emit(code_point) for each thing shown, in order, until it returns false.
    template <typename Emit> void walk(int index, Emit emit) const;

    std::string data_;
    std::vector<std::uint32_t> start_;
    std::vector<std::uint32_t> end_;
    int tab_width_ = 4;
    int max_columns_ = 0;
    bool truncated_ = false;
    TextEncoding encoding_ = TextEncoding::utf8;
};

} // namespace hui::viewer
