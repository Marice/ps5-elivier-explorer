// EliVier Explorer - A small picture for every kind of file, drawn from the kit's shapes.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No icon font and no image files: each icon is a few rounded rectangles, circles and lines in a
// square box, so it stays sharp at any size and takes its colour from the kind. A kind that has a
// handler (the file can be opened) is drawn in its colour; one without is drawn muted. That is how
// the list says "this one opens".

#pragma once

#include "gfx/draw_list.hpp"
#include "viewer/file_probe.hpp"

namespace hui::concepts::explorer
{

// The colour a kind is drawn in when it can be opened.
gfx::Color icon_accent(hui::viewer::IconKind kind);

// Draws the icon filling `box` (a square is expected). `background` is the colour behind it, used
// for the holes (a disc's centre) and the muted tone; `supported` picks the vivid or the muted
// look.
void draw_file_icon(gfx::DrawList &list, const gfx::Rect &box, hui::viewer::IconKind kind,
                    bool supported, gfx::Color background);

} // namespace hui::concepts::explorer
