// EliVier Explorer - Every kind of file has an icon, and it draws something sane.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/icons.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace
{

using hui::concepts::explorer::draw_file_icon;
using hui::concepts::explorer::icon_accent;
using hui::viewer::IconKind;

constexpr IconKind kAllKinds[] = {IconKind::folder,  IconKind::archive, IconKind::image,
                                  IconKind::audio,   IconKind::video,   IconKind::text,
                                  IconKind::program, IconKind::disc,    IconKind::other};

TEST(Icons, EveryKindDrawsFiniteShapesInsideItsBoxAndMutedOrNot)
{
    const hui::gfx::Rect box{100.0f, 200.0f, 28.0f, 28.0f};
    for (const IconKind kind : kAllKinds)
    {
        for (const bool supported : {false, true})
        {
            hui::gfx::DrawList list;
            draw_file_icon(list, box, kind, supported, hui::gfx::Color::rgb(0xffffff));
            const size_t shapes = list.instances().size() + list.mesh_vertices().size();
            EXPECT_GT(shapes, 0u) << static_cast<int>(kind);
            for (const hui::gfx::Instance &instance : list.instances())
            {
                for (const float value : instance.rect)
                    ASSERT_TRUE(std::isfinite(value));
                for (const float value : instance.color_top)
                    ASSERT_TRUE(std::isfinite(value));
                // Nothing strays far from the box: an icon must not spill into the next column.
                EXPECT_GE(instance.rect[0], box.x - 4.0f);
                EXPECT_LE(instance.rect[0] + instance.rect[2], box.x + box.w + 4.0f);
            }
        }
    }
}

TEST(Icons, AFolderIsDrawnInItsColourEvenWhenNoHandlerIsAsked)
{
    hui::gfx::DrawList vivid;
    hui::gfx::DrawList muted;
    const hui::gfx::Rect box{0.0f, 0.0f, 28.0f, 28.0f};
    draw_file_icon(vivid, box, IconKind::folder, true, hui::gfx::Color::rgb(0xffffff));
    draw_file_icon(muted, box, IconKind::folder, false, hui::gfx::Color::rgb(0xffffff));
    ASSERT_EQ(vivid.instances().size(), muted.instances().size());
    for (size_t i = 0; i < vivid.instances().size(); ++i)
        for (int c = 0; c < 4; ++c)
            EXPECT_FLOAT_EQ(vivid.instances()[i].color_top[c], muted.instances()[i].color_top[c]);
}

TEST(Icons, AFileWithoutAHandlerIsPaler)
{
    hui::gfx::DrawList vivid;
    hui::gfx::DrawList muted;
    const hui::gfx::Rect box{0.0f, 0.0f, 28.0f, 28.0f};
    draw_file_icon(vivid, box, IconKind::image, true, hui::gfx::Color::rgb(0xffffff));
    draw_file_icon(muted, box, IconKind::image, false, hui::gfx::Color::rgb(0xffffff));
    ASSERT_FALSE(vivid.instances().empty());
    // The body (the first shape) is closer to the white background when muted.
    EXPECT_GT(muted.instances()[0].color_top[1], vivid.instances()[0].color_top[1]);
}

TEST(Icons, EveryKindHasItsOwnColourExceptOther)
{
    for (const IconKind a : kAllKinds)
        for (const IconKind b : kAllKinds)
        {
            if (a == b)
                continue;
            const auto ca = icon_accent(a);
            const auto cb = icon_accent(b);
            EXPECT_TRUE(ca.r != cb.r || ca.g != cb.g || ca.b != cb.b)
                << static_cast<int>(a) << " and " << static_cast<int>(b);
        }
}

} // namespace
