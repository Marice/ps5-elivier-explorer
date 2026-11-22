// EliVier Explorer - The text viewer: a file as lines in a monospace face, scrolled by the pad.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the first 8 MiB of a text file, splits it into lines once (TextDocument) and draws only the
// lines in view, so a file of any length costs the screen's worth. Line numbers on the left,
// scrolling up and down by line or page, left and right by columns; no wrapping. View only.
//
//   D-pad up/down  a line          L1 / R1  a page up or down
//   D-pad left/right  four columns  L2 / R2  the top / the bottom
//   right stick    scrolls steadily     Circle  back

#pragma once

#include "core/held_step.hpp"
#include "viewer/handler.hpp"
#include "viewer/text_document.hpp"
#include "viewer/viewer.hpp"

#include <cstdint>

namespace hui::viewer
{

constexpr std::size_t kTextViewMaxBytes = 8u << 20; // what is read of a text file

class TextViewer final : public Viewer
{
  public:
    // `file_size` is the whole file (the document may hold only the start of it).
    TextViewer(std::string name, TextDocument document, std::uint64_t file_size,
               const ViewerContext &context);

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

    // For tests and the status line.
    int top_line() const
    {
        return top_;
    }
    int left_column() const
    {
        return left_;
    }
    int visible_rows() const
    {
        return rows_;
    }
    int visible_columns() const
    {
        return columns_;
    }
    const TextDocument &document() const
    {
        return document_;
    }

  private:
    struct Geometry
    {
        float gutter = 0.0f; // the line numbers' width
        float text_x = 0.0f;
        float text_width = 0.0f;
        int rows = 1;
        int columns = 1;
    };
    Geometry geometry(const gfx::Rect &area, float advance) const;
    void clamp();
    void scroll_lines(int lines);
    void scroll_columns(int columns);

    std::string name_;
    TextDocument document_;
    std::uint64_t file_size_;
    gfx::Rect area_;
    float advance_ = 13.0f; // one column, in pixels
    int rows_ = 1;
    int columns_ = 1;
    int top_ = 0;
    int left_ = 0;
    float accumulated_lines_ = 0.0f;
    float accumulated_columns_ = 0.0f;
    HeldStep pager_{Action::page_prev, Action::page_next};
    bool closing_ = false;
};

class TextHandler final : public FileHandler
{
  public:
    explicit TextHandler(std::size_t max_bytes = kTextViewMaxBytes) : max_bytes_(max_bytes)
    {
    }
    const char *id() const override
    {
        return "text";
    }
    const char *label() const override
    {
        return "Text";
    }
    const char *verb() const override
    {
        return "View text";
    }
    IconKind icon() const override
    {
        return IconKind::text;
    }
    Match match(const FileProbe &probe) const override;
    std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const override;

  private:
    std::size_t max_bytes_;
};

} // namespace hui::viewer
