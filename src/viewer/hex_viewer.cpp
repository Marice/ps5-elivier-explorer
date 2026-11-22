// EliVier Explorer - The hex viewer: any file as offset, bytes and text, read a page at a time.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/hex_viewer.hpp"

#include "explorer/format.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::viewer
{

namespace
{

constexpr float kTextSize = 22.0f;
constexpr float kRowHeight = 30.0f;
constexpr float kStatusHeight = 76.0f;
constexpr float kBaseline = 22.0f;
constexpr int kOffsetDigits = 8; // columns of the offset, more for a file over 4 GiB

int offset_digits(std::uint64_t size)
{
    int digits = kOffsetDigits;
    while (digits < 16 && (size >> (4 * digits)) != 0)
        ++digits;
    return digits;
}

char printable(unsigned char c)
{
    return c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.';
}

} // namespace

std::string format_hex_row(std::uint64_t offset, const std::string &bytes, std::size_t first,
                           std::size_t count)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string row;
    char head[24];
    std::snprintf(head, sizeof(head), "%08llx  ", static_cast<unsigned long long>(offset));
    row = head;
    for (int i = 0; i < kHexRowBytes; ++i)
    {
        if (static_cast<std::size_t>(i) < count && first + i < bytes.size())
        {
            const auto c = static_cast<unsigned char>(bytes[first + i]);
            row += kDigits[c >> 4];
            row += kDigits[c & 15];
        }
        else
            row += "  ";
        row += i == 7 ? "  " : " ";
    }
    row += " |";
    for (std::size_t i = 0; i < count && first + i < bytes.size(); ++i)
        row += printable(static_cast<unsigned char>(bytes[first + i]));
    row += '|';
    return row;
}

HexViewer::HexViewer(std::string name, FileRef file, const ViewerContext &context)
    : name_(std::move(name)), file_(std::move(file)), size_(file_.entry.size), area_(context.area)
{
    if (context.fonts != nullptr && context.fonts->mono.font != nullptr)
        advance_ = context.fonts->mono.font->measure("0", kTextSize);
    rows_ = std::max(1, static_cast<int>((area_.h - kStatusHeight) / kRowHeight));
    goto_digits_ = std::max(4, offset_digits(size_));
    ensure_window();
}

void HexViewer::ensure_window()
{
    if (size_ == 0 || file_.source == nullptr)
        return;
    const std::uint64_t first = top_row_ * kHexRowBytes;
    const std::uint64_t want_end =
        std::min(size_, first + static_cast<std::uint64_t>(rows_) * kHexRowBytes);
    if (!window_failed_ && first >= window_start_ && want_end <= window_start_ + window_.size())
        return;
    window_.assign(kHexWindowBytes, '\0');
    const std::int64_t got =
        file_.source->read_range(file_.path, first, window_.data(), window_.size());
    window_start_ = first;
    window_failed_ = got < 0;
    window_.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
}

void HexViewer::move_to(std::uint64_t offset)
{
    cursor_ = std::min(offset, last_offset());
    const std::uint64_t row = cursor_ / kHexRowBytes;
    if (row < top_row_)
        top_row_ = row;
    else if (row >= top_row_ + static_cast<std::uint64_t>(rows_))
        top_row_ = row - static_cast<std::uint64_t>(rows_) + 1;
    ensure_window();
}

void HexViewer::move_by(std::int64_t delta)
{
    if (delta < 0 && cursor_ < static_cast<std::uint64_t>(-delta))
        move_to(0);
    else
        move_to(static_cast<std::uint64_t>(static_cast<std::int64_t>(cursor_) + delta));
}

void HexViewer::open_goto()
{
    goto_ = true;
    goto_value_ = cursor_;
    goto_digit_ = goto_digits_ - 1;
}

void HexViewer::update_goto(const InputFrame &input)
{
    if (input.is_pressed(Action::back))
    {
        goto_ = false;
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        goto_ = false;
        move_to(goto_value_);
        return;
    }
    const int shift = 4 * (goto_digits_ - 1 - goto_digit_);
    const std::uint64_t digit = (goto_value_ >> shift) & 15;
    switch (input.nav)
    {
    case Direction::left:
        goto_digit_ = std::max(0, goto_digit_ - 1);
        break;
    case Direction::right:
        goto_digit_ = std::min(goto_digits_ - 1, goto_digit_ + 1);
        break;
    case Direction::up:
        goto_value_ = (goto_value_ & ~(std::uint64_t{15} << shift)) | (((digit + 1) & 15) << shift);
        break;
    case Direction::down:
        goto_value_ =
            (goto_value_ & ~(std::uint64_t{15} << shift)) | (((digit + 15) & 15) << shift);
        break;
    case Direction::none:
        break;
    }
}

void HexViewer::update(const InputFrame &input, float dt, ui::Feedback & /*feedback*/)
{
    if (goto_)
    {
        update_goto(input);
        return;
    }
    if (input.is_pressed(Action::back))
    {
        closing_ = true;
        return;
    }
    if (input.is_pressed(Action::west))
    {
        open_goto();
        return;
    }
    switch (input.nav)
    {
    case Direction::left:
        move_by(-1);
        break;
    case Direction::right:
        move_by(1);
        break;
    case Direction::up:
        move_by(-kHexRowBytes);
        break;
    case Direction::down:
        move_by(kHexRowBytes);
        break;
    case Direction::none:
        break;
    }
    if (const int step = pager_.step(input, dt))
        move_by(static_cast<std::int64_t>(step) * std::max(1, rows_ - 1) * kHexRowBytes);
    if (input.is_pressed(Action::jump_prev))
        move_to(0);
    else if (input.is_pressed(Action::jump_next))
        move_to(last_offset());
}

std::span<const ui::Hint> HexViewer::hints() const
{
    static constexpr ui::Hint kHints[] = {
        {ui::Button::dpad, "Move"},
        {ui::Button::l1, "Page", ui::Button::r1},
        {ui::Button::l2, "Start / end", ui::Button::r2},
        {ui::Button::square, "Go to"},
        {ui::Button::circle, "Back"},
    };
    static constexpr ui::Hint kGoto[] = {
        {ui::Button::dpad, "Digit"},
        {ui::Button::cross, "Go"},
        {ui::Button::circle, "Cancel"},
    };
    return goto_ ? std::span<const ui::Hint>(kGoto) : std::span<const ui::Hint>(kHints);
}

void HexViewer::draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const
{
    const ui::Theme &look = paint.theme();
    const ui::FontRef &mono = canvas.fonts.mono;
    const float advance = mono.font != nullptr ? mono.font->measure("0", kTextSize) : advance_;
    if (size_ == 0)
    {
        paint.body("This file is empty", area.x, area.y + 40.0f, 26.0f, paint.page_text_muted());
        return;
    }
    if (window_failed_)
    {
        paint.body("This file cannot be read", area.x, area.y + 40.0f, 26.0f,
                   paint.page_text_muted());
        return;
    }
    const float rows_height = static_cast<float>(rows_) * kRowHeight;
    // Character columns inside a row: offset 8, two spaces, then 3 per byte (one more after 8).
    const auto byte_x = [&](int i)
    { return area.x + advance * (10.0f + 3.0f * i + (i >= 8 ? 1.0f : 0.0f)); };
    const auto ascii_x = [&](int i) { return area.x + advance * (10.0f + 49.0f + 2.0f + i); };

    for (int r = 0; r < rows_; ++r)
    {
        const std::uint64_t row = top_row_ + static_cast<std::uint64_t>(r);
        const std::uint64_t offset = row * kHexRowBytes;
        if (offset >= size_)
            break;
        const std::size_t count =
            static_cast<std::size_t>(std::min<std::uint64_t>(kHexRowBytes, size_ - offset));
        const float y = area.y + static_cast<float>(r) * kRowHeight + kBaseline;
        if (offset >= window_start_ && offset - window_start_ < window_.size())
        {
            const std::string line = format_hex_row(
                offset, window_, static_cast<std::size_t>(offset - window_start_), count);
            ui::text(canvas.list, mono, line, area.x, y, kTextSize, look.text);
        }
        if (cursor_ / kHexRowBytes == row)
        {
            const int i = static_cast<int>(cursor_ % kHexRowBytes);
            gfx::Color mark = look.primary;
            mark.a = 0.35f;
            const float top = area.y + static_cast<float>(r) * kRowHeight;
            canvas.list.rounded_rect(
                {byte_x(i) - 4.0f, top, advance * 2.0f + 8.0f, kRowHeight - 2.0f}, 4.0f, mark);
            canvas.list.rounded_rect({ascii_x(i) - 1.0f, top, advance + 2.0f, kRowHeight - 2.0f},
                                     4.0f, mark);
        }
    }

    // A thumb on the right: where this page is in the file.
    const std::uint64_t total_rows = (size_ + kHexRowBytes - 1) / kHexRowBytes;
    if (total_rows > static_cast<std::uint64_t>(rows_))
    {
        constexpr float kThumbWidth = 6.0f;
        const float track_x = area.x + area.w - kThumbWidth;
        const float thumb =
            std::max(28.0f, rows_height * static_cast<float>(static_cast<double>(rows_) /
                                                             static_cast<double>(total_rows)));
        const double max_top = static_cast<double>(total_rows - static_cast<std::uint64_t>(rows_));
        const float y = area.y + (rows_height - thumb) *
                                     static_cast<float>(static_cast<double>(top_row_) / max_top);
        canvas.list.rounded_rect({track_x, area.y, kThumbWidth, rows_height}, 3.0f,
                                 look.surface_high);
        canvas.list.rounded_rect({track_x, y, kThumbWidth, thumb}, 3.0f, look.text_muted);
    }

    // The status line: the offset in both bases, and the value under the cursor.
    char status[240];
    const std::uint64_t at = cursor_;
    char value[120] = "";
    if (at >= window_start_ && at - window_start_ < window_.size())
    {
        const std::size_t i = static_cast<std::size_t>(at - window_start_);
        const auto b = [&](std::size_t k)
        { return static_cast<std::uint32_t>(static_cast<unsigned char>(window_[i + k])); };
        const std::size_t left = window_.size() - i;
        int used = std::snprintf(value, sizeof(value), "u8 %u", b(0));
        if (left >= 2)
            used +=
                std::snprintf(value + used, sizeof(value) - used, "   u16 %u", b(0) | b(1) << 8);
        if (left >= 4)
            std::snprintf(value + used, sizeof(value) - used, "   u32 %u",
                          b(0) | b(1) << 8 | b(2) << 16 | b(3) << 24);
    }
    std::snprintf(status, sizeof(status), "Offset 0x%llx (%llu) of %s   %s   (little endian)",
                  static_cast<unsigned long long>(at), static_cast<unsigned long long>(at),
                  hui::explorer::format_bytes(size_).c_str(), value);
    paint.body(status, area.x, area.y + rows_height + 32.0f, 20.0f, paint.page_text_muted());

    if (goto_)
    {
        const float w = advance * static_cast<float>(goto_digits_) * 1.6f + 120.0f;
        const gfx::Rect panel{area.x + (area.w - w) / 2.0f, area.y + 120.0f, w, 190.0f};
        canvas.list.rounded_rect(panel, 14.0f, look.surface_high);
        ui::text(canvas.list, canvas.fonts.regular, "Go to offset (hex)", panel.x + 36.0f,
                 panel.y + 56.0f, 30.0f, look.text);
        const float cell = advance * 1.6f;
        for (int d = 0; d < goto_digits_; ++d)
        {
            const int shift = 4 * (goto_digits_ - 1 - d);
            char c[2] = {"0123456789abcdef"[(goto_value_ >> shift) & 15], 0};
            const float x = panel.x + 36.0f + cell * static_cast<float>(d);
            if (d == goto_digit_)
                canvas.list.rounded_rect({x - 4.0f, panel.y + 84.0f, cell, 62.0f}, 6.0f,
                                         look.primary);
            ui::text(canvas.list, mono, c, x + cell / 2.0f - 4.0f - advance / 2.0f,
                     panel.y + 128.0f, 34.0f, d == goto_digit_ ? look.on_primary : look.text);
        }
    }
}

std::unique_ptr<Viewer> HexHandler::open(ViewerContext &context, const FileRef &file) const
{
    if (file.source == nullptr)
        return nullptr;
    const std::string name = file.path.substr(file.path.rfind('/') + 1);
    return std::make_unique<HexViewer>(name, file, context);
}

} // namespace hui::viewer
