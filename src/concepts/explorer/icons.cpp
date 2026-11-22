// EliVier Explorer - A small picture for every kind of file, drawn from the kit's shapes.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/icons.hpp"

namespace hui::concepts::explorer
{

namespace
{

using gfx::Color;
using gfx::Rect;
using hui::viewer::IconKind;

// A rectangle given in the box's own units (0 to 1 each way).
Rect part(const Rect &box, float x, float y, float w, float h)
{
    return {box.x + x * box.w, box.y + y * box.h, w * box.w, h * box.h};
}

} // namespace

Color icon_accent(IconKind kind)
{
    switch (kind)
    {
    case IconKind::folder:
        return Color::rgb(0xf0a21b);
    case IconKind::archive:
        return Color::rgb(0xa5764a);
    case IconKind::image:
        return Color::rgb(0x2fa36b);
    case IconKind::audio:
        return Color::rgb(0x8e4ec6);
    case IconKind::video:
        return Color::rgb(0xdb4b50);
    case IconKind::text:
        return Color::rgb(0x3b82c4);
    case IconKind::program:
        return Color::rgb(0x12a594);
    case IconKind::disc:
        return Color::rgb(0x4a5fd0);
    case IconKind::other:
        break;
    }
    return Color::rgb(0x8a8f98);
}

void draw_file_icon(gfx::DrawList &list, const Rect &box, IconKind kind, bool supported,
                    Color background)
{
    // A folder is always a folder: it opens. For a file the colour says whether a viewer knows it.
    const bool vivid = supported || kind == IconKind::folder;
    const Color body = vivid ? icon_accent(kind) : gfx::mix(icon_accent(kind), background, 0.62f);
    const Color dark = gfx::mix(body, Color::rgb(0x000000), 0.28f);
    const Color light = gfx::mix(body, Color::rgb(0xffffff), 0.55f);
    const Color white = Color::rgb(0xffffff, vivid ? 1.0f : 0.8f);
    const float r = box.w * 0.1f;

    switch (kind)
    {
    case IconKind::folder:
        list.rounded_rect(part(box, 0.06f, 0.16f, 0.44f, 0.30f), r * 0.8f, dark);
        list.rounded_rect(part(box, 0.06f, 0.30f, 0.88f, 0.58f), r, body);
        list.rounded_rect(part(box, 0.06f, 0.30f, 0.88f, 0.12f), r * 0.6f, light);
        break;
    case IconKind::text:
    case IconKind::other:
    {
        list.rounded_rect(part(box, 0.18f, 0.06f, 0.64f, 0.88f), r,
                          kind == IconKind::text ? light : body);
        const float corner[6] = {box.x + 0.82f * box.w, box.y + 0.06f * box.h,
                                 box.x + 0.82f * box.w, box.y + 0.30f * box.h,
                                 box.x + 0.58f * box.w, box.y + 0.30f * box.h};
        list.polygon(corner, 3, dark);
        if (kind == IconKind::text)
        {
            for (int i = 0; i < 3; ++i)
                list.rounded_rect(part(box, 0.28f, 0.44f + 0.15f * static_cast<float>(i),
                                       i == 2 ? 0.28f : 0.44f, 0.06f),
                                  r * 0.3f, body);
        }
        break;
    }
    case IconKind::image:
        list.rounded_rect(part(box, 0.06f, 0.14f, 0.88f, 0.72f), r, body);
        list.circle(box.x + 0.70f * box.w, box.y + 0.36f * box.h, box.w * 0.09f, white);
        {
            const float hill[6] = {box.x + 0.06f * box.w, box.y + 0.86f * box.h,
                                   box.x + 0.40f * box.w, box.y + 0.46f * box.h,
                                   box.x + 0.72f * box.w, box.y + 0.86f * box.h};
            list.polygon(hill, 3, dark);
        }
        break;
    case IconKind::audio:
        list.circle(box.x + 0.36f * box.w, box.y + 0.74f * box.h, box.w * 0.15f, body);
        list.rounded_rect(part(box, 0.45f, 0.16f, 0.08f, 0.60f), r * 0.3f, body);
        list.line(box.x + 0.49f * box.w, box.y + 0.18f * box.h, box.x + 0.82f * box.w,
                  box.y + 0.34f * box.h, box.w * 0.09f, body);
        break;
    case IconKind::video:
        list.rounded_rect(part(box, 0.06f, 0.18f, 0.88f, 0.64f), r, body);
        {
            const float play[6] = {box.x + 0.40f * box.w, box.y + 0.34f * box.h,
                                   box.x + 0.40f * box.w, box.y + 0.66f * box.h,
                                   box.x + 0.68f * box.w, box.y + 0.50f * box.h};
            list.polygon(play, 3, white);
        }
        break;
    case IconKind::archive:
        list.rounded_rect(part(box, 0.14f, 0.08f, 0.72f, 0.84f), r, body);
        for (int i = 0; i < 4; ++i)
            list.rounded_rect(part(box, i % 2 == 0 ? 0.44f : 0.50f,
                                   0.14f + 0.14f * static_cast<float>(i), 0.12f, 0.08f),
                              r * 0.2f, white);
        list.rounded_rect(part(box, 0.42f, 0.70f, 0.16f, 0.14f), r * 0.4f, dark);
        break;
    case IconKind::program:
        for (int i = 0; i < 3; ++i)
        {
            const float at = 0.30f + 0.20f * static_cast<float>(i);
            list.rounded_rect(part(box, at - 0.03f, 0.08f, 0.06f, 0.84f), r * 0.2f, dark);
            list.rounded_rect(part(box, 0.08f, at - 0.03f, 0.84f, 0.06f), r * 0.2f, dark);
        }
        list.rounded_rect(part(box, 0.20f, 0.20f, 0.60f, 0.60f), r * 0.8f, body);
        list.rounded_rect(part(box, 0.36f, 0.36f, 0.28f, 0.28f), r * 0.4f, white);
        break;
    case IconKind::disc:
        list.circle(box.cx(), box.cy(), box.w * 0.44f, body);
        list.ring(box.cx(), box.cy(), box.w * 0.30f, box.w * 0.04f, white);
        list.circle(box.cx(), box.cy(), box.w * 0.11f, background);
        break;
    }
}

} // namespace hui::concepts::explorer
