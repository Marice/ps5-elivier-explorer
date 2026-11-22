// EliVier Explorer - The text viewer: a file as lines in a monospace face, scrolled by the pad.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/text_viewer.hpp"

#include "explorer/format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hui::viewer
{

namespace
{

constexpr float kTextSize = 22.0f;
constexpr float kRowHeight = 30.0f;
constexpr float kStatusHeight = 46.0f;
constexpr float kBaseline = 22.0f; // from the top of a row
constexpr float kThumbWidth = 6.0f;
constexpr int kColumnStep = 4;
constexpr float kStickLines = 30.0f;   // lines a second at full tilt
constexpr float kStickColumns = 40.0f; // columns a second

int digits_of(int value)
{
    int digits = 1;
    while (value >= 10)
    {
        value /= 10;
        ++digits;
    }
    return digits;
}

} // namespace

TextViewer::TextViewer(std::string name, TextDocument document, std::uint64_t file_size,
                       const ViewerContext &context)
    : name_(std::move(name)), document_(std::move(document)), file_size_(file_size),
      area_(context.area)
{
    if (context.fonts != nullptr && context.fonts->mono.font != nullptr)
        advance_ = context.fonts->mono.font->measure("0", kTextSize);
    const Geometry g = geometry(area_, advance_);
    rows_ = g.rows;
    columns_ = g.columns;
}

TextViewer::Geometry TextViewer::geometry(const gfx::Rect &area, float advance) const
{
    Geometry g;
    g.gutter = static_cast<float>(digits_of(std::max(1, document_.line_count()))) * advance + 32.0f;
    g.text_x = area.x + g.gutter + 16.0f;
    g.text_width = area.w - g.gutter - 16.0f - 24.0f;
    g.rows = std::max(1, static_cast<int>((area.h - kStatusHeight) / kRowHeight));
    g.columns = std::max(1, static_cast<int>(g.text_width / advance));
    return g;
}

void TextViewer::clamp()
{
    const int max_top = std::max(0, document_.line_count() - rows_);
    const int max_left = std::max(0, document_.max_columns() - columns_);
    top_ = std::clamp(top_, 0, max_top);
    left_ = std::clamp(left_, 0, max_left);
}

void TextViewer::scroll_lines(int lines)
{
    top_ += lines;
    clamp();
}

void TextViewer::scroll_columns(int columns)
{
    left_ += columns;
    clamp();
}

void TextViewer::update(const InputFrame &input, float dt, ui::Feedback & /*feedback*/)
{
    if (input.is_pressed(Action::back))
    {
        closing_ = true;
        return;
    }
    switch (input.nav)
    {
    case Direction::up:
        scroll_lines(-1);
        break;
    case Direction::down:
        scroll_lines(1);
        break;
    case Direction::left:
        scroll_columns(-kColumnStep);
        break;
    case Direction::right:
        scroll_columns(kColumnStep);
        break;
    case Direction::none:
        break;
    }
    if (const int step = pager_.step(input, dt))
        scroll_lines(step * std::max(1, rows_ - 1));
    if (input.is_pressed(Action::jump_prev))
    {
        top_ = 0;
        left_ = 0;
        clamp();
    }
    else if (input.is_pressed(Action::jump_next))
    {
        top_ = document_.line_count();
        clamp();
    }
    // The right stick scrolls steadily: whole lines and columns are taken from what has built up.
    accumulated_lines_ += input.stick2_y * kStickLines * dt;
    accumulated_columns_ += input.stick2_x * kStickColumns * dt;
    const int lines = static_cast<int>(accumulated_lines_);
    const int columns = static_cast<int>(accumulated_columns_);
    accumulated_lines_ -= static_cast<float>(lines);
    accumulated_columns_ -= static_cast<float>(columns);
    if (lines != 0)
        scroll_lines(lines);
    if (columns != 0)
        scroll_columns(columns);
}

std::span<const ui::Hint> TextViewer::hints() const
{
    static constexpr ui::Hint kHints[] = {
        {ui::Button::dpad, "Scroll"},
        {ui::Button::l1, "Page", ui::Button::r1},
        {ui::Button::l2, "Top / end", ui::Button::r2},
        {ui::Button::circle, "Back"},
    };
    return kHints;
}

void TextViewer::draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const
{
    using gfx::Rect;
    const ui::Theme &look = paint.theme();
    const ui::FontRef &mono = canvas.fonts.mono;
    const float advance = mono.font != nullptr ? mono.font->measure("0", kTextSize) : advance_;
    const Geometry g = geometry(area, advance);
    const int lines = document_.line_count();

    if (lines == 0)
    {
        paint.body("This file is empty", area.x, area.y + 40.0f, 26.0f, paint.page_text_muted());
        return;
    }

    // The gutter: a quiet plate behind the line numbers.
    const float rows_height = static_cast<float>(g.rows) * kRowHeight;
    canvas.list.rounded_rect({area.x, area.y, g.gutter, rows_height}, 10.0f, look.surface_high);
    const int last = std::min(lines, top_ + g.rows);
    canvas.list.push_clip({g.text_x, area.y, g.text_width, rows_height});
    for (int line = top_; line < last; ++line)
    {
        const float y = area.y + static_cast<float>(line - top_) * kRowHeight + kBaseline;
        const std::string slice = document_.display_slice(line, left_, g.columns + 1);
        ui::text(canvas.list, mono, slice, g.text_x, y, kTextSize, look.text);
    }
    canvas.list.pop_clip();
    for (int line = top_; line < last; ++line)
    {
        const float y = area.y + static_cast<float>(line - top_) * kRowHeight + kBaseline;
        char number[16];
        std::snprintf(number, sizeof(number), "%d", line + 1);
        ui::text(canvas.list, mono, number, area.x + g.gutter - 14.0f, y, kTextSize,
                 look.text_muted, gfx::Align::right);
    }

    // A thumb on the right: where this page is in the file.
    if (lines > g.rows)
    {
        const float track_x = area.x + area.w - kThumbWidth;
        const float thumb =
            std::max(28.0f, rows_height * static_cast<float>(g.rows) / static_cast<float>(lines));
        const float max_top = static_cast<float>(lines - g.rows);
        const float y = area.y + (rows_height - thumb) * (static_cast<float>(top_) / max_top);
        canvas.list.rounded_rect({track_x, area.y, kThumbWidth, rows_height}, 3.0f,
                                 look.surface_high);
        canvas.list.rounded_rect({track_x, y, kThumbWidth, thumb}, 3.0f, look.text_muted);
    }

    // The status line: where you are, and what was read.
    char status[200];
    std::snprintf(status, sizeof(status), "Line %d of %d   Column %d   %s   %s", top_ + 1, lines,
                  left_ + 1,
                  document_.encoding() == TextEncoding::utf16_le   ? "UTF-16 LE"
                  : document_.encoding() == TextEncoding::utf16_be ? "UTF-16 BE"
                  : document_.encoding() == TextEncoding::utf8_bom ? "UTF-8 (BOM)"
                                                                   : "UTF-8",
                  hui::explorer::format_bytes(file_size_).c_str());
    std::string line = status;
    if (document_.truncated())
        line += "   -   only the first " +
                hui::explorer::format_bytes(static_cast<std::uint64_t>(document_.bytes())) +
                " are shown (Open as Hex for the rest)";
    paint.body(line, area.x, area.y + rows_height + 32.0f, 20.0f, paint.page_text_muted());
}

Match TextHandler::match(const FileProbe &probe) const
{
    if (is_text_extension(probe.ext()))
        return Match::extension;
    // An unknown extension: text if the first bytes look like it. As weak as an extension, so a
    // handler that knows the content (an image) still wins over a file that merely has no suffix.
    if (probe.can_sniff() && looks_like_text(probe.head()))
        return Match::extension;
    return Match::none;
}

std::unique_ptr<Viewer> TextHandler::open(ViewerContext &context, const FileRef &file) const
{
    if (file.source == nullptr)
        return nullptr;
    std::string bytes;
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<char> buffer(kChunk);
    std::uint64_t offset = 0;
    while (bytes.size() < max_bytes_)
    {
        const std::size_t want = std::min(kChunk, max_bytes_ - bytes.size());
        const std::int64_t got = file.source->read_range(file.path, offset, buffer.data(), want);
        if (got < 0)
            return nullptr;
        if (got == 0)
            break;
        bytes.append(buffer.data(), static_cast<std::size_t>(got));
        offset += static_cast<std::uint64_t>(got);
    }
    const bool truncated = file.entry.size > bytes.size();
    TextDocument document;
    document.load(std::move(bytes), truncated);
    const std::string name = file.path.substr(file.path.rfind('/') + 1);
    return std::make_unique<TextViewer>(name, std::move(document), file.entry.size, context);
}

} // namespace hui::viewer
