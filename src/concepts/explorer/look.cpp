// EliVier Explorer - What the screens share: the theme, the header, hints and running text.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/look.hpp"

#include "explorer/format.hpp"

#include <cstdio>
#include <string>

namespace hui::concepts::explorer
{

using gfx::Color;
using gfx::Rect;

const ui::Theme &theme()
{
    static const ui::Theme kTheme = []
    {
        for (const ui::Theme &candidate : ui::themes())
        {
            if (std::string_view(candidate.id) == "utility")
                return candidate;
        }
        return ui::default_theme();
    }();
    return kTheme;
}

void begin_frame(app::Frame &frame, const ui::Theme &look)
{
    frame.backdrop = look.backdrop;
}

void draw_header(ui::Painter &paint, const char *title, const char *summary, std::string_view state)
{
    const ui::Theme &look = paint.theme();
    const Color ink = paint.page_text();
    const Color quiet = paint.page_text_muted();
    paint.label("ELIVIER EXPLORER", kMargin, 104.0f, 19.0f, quiet);
    paint.heading(title, kMargin - 2.0f, 172.0f, 58.0f, ink);
    paint.body(summary, kMargin, 214.0f, 23.0f, quiet);
    if (state.empty())
        return;
    const std::string text(state);
    const float width = paint.label_width(text, 22.0f) + 56.0f;
    const Rect pill{kRight - width, 92.0f, width, 58.0f};
    paint.surface(pill, look.radius, look.surface_high, look.outline, 1.0f);
    paint.label(text, pill.x + 28.0f, pill.cy() + 8.0f, 22.0f, look.text);
}

void draw_hints(gfx::DrawList &list, const ui::Fonts &fonts, const ui::Theme &look,
                std::span<const ui::Hint> hints)
{
    ui::Hint row[10];
    int count = 0;
    for (const ui::Hint &hint : hints)
    {
        if (count < 9)
            row[count++] = hint;
    }
    ui::GlyphStyle glyphs = look.dark ? ui::GlyphStyle::dark() : ui::GlyphStyle::light();
    glyphs.label = look.page_text.a > 0.0f ? look.page_text : look.text;
    ui::draw_hints(list, fonts, glyphs, row, count, kRight, true);
}

float paragraph(ui::Painter &paint, std::string_view text, float x, float y, float size,
                float width, Color color, int max_lines, float leading)
{
    const ui::FontRef &font = paint.font(ui::FontRole::regular);
    int lines = 0;
    for (const std::string &line : font.font->wrap(text, size, width))
    {
        if (max_lines > 0 && lines == max_lines)
            break;
        paint.body(line, x, y, size, color);
        y += size * leading;
        ++lines;
    }
    return y;
}

Rect side_panel(ui::Painter &paint, const Rect &r)
{
    const ui::Theme &look = paint.theme();
    return paint.surface(r, look.radius_card, look.surface, look.outline, 1.0f);
}

std::string size_text(std::uint64_t bytes)
{
    return hui::explorer::format_bytes(bytes);
}

std::string pane_error_text(int list_error, bool sandboxed)
{
    if (list_error != 0 && sandboxed)
        return "Needs the console unsandboxed: run etaHEN or OnionHEN, or launch the Lapy JB "
               "Daemon from the Homebrew Launcher, then reopen this app.";
    char line[96];
    std::snprintf(line, sizeof(line), "Could not open that folder (error %d)", list_error);
    return line;
}

} // namespace hui::concepts::explorer
