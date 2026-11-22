// EliVier Explorer - The hex viewer: any file as offset, bytes and text, read a page at a time.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Only the page in view is read (through Source::read_range), so a file of any size opens at once.
// A cursor marks one byte; its value is shown as 8, 16 and 32 bits. View only: nothing is written.
//
//   D-pad left/right  a byte        L1 / R1  a page up or down
//   D-pad up/down     a row         L2 / R2  the start / the end
//   Square            go to an offset (digits: left/right pick, up/down change, Cross goes)

#pragma once

#include "core/held_step.hpp"
#include "viewer/handler.hpp"
#include "viewer/viewer.hpp"

#include <cstdint>
#include <string>

namespace hui::viewer
{

constexpr int kHexRowBytes = 16;
constexpr std::size_t kHexWindowBytes = 64u << 10; // what is held of the file at a time

// One row as text: "00000010  48 65 6c 6c ...  |Hello...|". Short last rows are padded.
std::string format_hex_row(std::uint64_t offset, const std::string &bytes, std::size_t first,
                           std::size_t count);

class HexViewer final : public Viewer
{
  public:
    HexViewer(std::string name, FileRef file, const ViewerContext &context);

    std::string title() const override
    {
        return name_;
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const override;
    std::span<const ui::Hint> hints() const override;
    bool closing() const override
    {
        return closing_;
    }

    std::uint64_t cursor() const
    {
        return cursor_;
    }
    std::uint64_t top_row() const
    {
        return top_row_;
    }
    int visible_rows() const
    {
        return rows_;
    }
    bool choosing_offset() const
    {
        return goto_;
    }
    std::uint64_t size() const
    {
        return size_;
    }

  private:
    std::uint64_t last_offset() const
    {
        return size_ == 0 ? 0 : size_ - 1;
    }
    void move_to(std::uint64_t offset);
    void move_by(std::int64_t delta);
    void ensure_window();
    void update_goto(const InputFrame &input);
    void open_goto();

    std::string name_;
    FileRef file_;
    std::uint64_t size_ = 0;
    gfx::Rect area_;
    float advance_ = 13.0f;
    int rows_ = 1;
    std::uint64_t cursor_ = 0;
    std::uint64_t top_row_ = 0;
    std::string window_; // bytes of the file from window_start_
    std::uint64_t window_start_ = 0;
    bool window_failed_ = false;
    HeldStep pager_{Action::page_prev, Action::page_next};
    bool closing_ = false;
    // The offset chooser.
    bool goto_ = false;
    int goto_digits_ = 4;
    int goto_digit_ = 0; // which digit, counted from the left
    std::uint64_t goto_value_ = 0;
};

class HexHandler final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "hex";
    }
    const char *label() const override
    {
        return "Hex";
    }
    const char *verb() const override
    {
        return "View hex";
    }
    IconKind icon() const override
    {
        return IconKind::other;
    }
    // Every file can be looked at as bytes, but nothing prefers it.
    Match match(const FileProbe &) const override
    {
        return Match::fallback;
    }
    std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const override;
};

} // namespace hui::viewer
