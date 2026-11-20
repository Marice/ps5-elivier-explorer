// EliVier Explorer - What the screens share: the theme, the header, hints and running text.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"
#include "ui/components/component.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hui::concepts::explorer
{

constexpr float kMargin = 96.0f;
constexpr float kRight = gfx::kVirtualWidth - kMargin;
constexpr float kTop = 290.0f;    // where a screen's content starts
constexpr float kBottom = 940.0f; // ... and ends, above the hints

// The app's one look: the kit's "Utility" theme (27 · after Foundation).
const ui::Theme &theme();

// Paints the page; everything else draws on frame.scene.
void begin_frame(app::Frame &frame, const ui::Theme &theme);

// "ELIVIER EXPLORER", the screen's title and summary, and an optional state on the right.
void draw_header(ui::Painter &paint, const char *title, const char *summary,
                 std::string_view state = {});

// The hint row at the bottom right; L1/R1 is added.
void draw_hints(gfx::DrawList &list, const ui::Fonts &fonts, const ui::Theme &theme,
                std::span<const ui::Hint> hints);

// Running text wrapped to a width; returns the baseline after the last line.
float paragraph(ui::Painter &paint, std::string_view text, float x, float y, float size,
                float width, gfx::Color color, int max_lines = 0, float leading = 1.4f);

// A side panel's surface; returns its inside.
gfx::Rect side_panel(ui::Painter &paint, const gfx::Rect &r);

// "512 B", "4.2 KB", "512 MB", "1.3 GB", "2.0 TB".
std::string size_text(std::uint64_t bytes);

} // namespace hui::concepts::explorer
