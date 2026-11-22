// EliVier Explorer - Screen "About": what the name means, and a Ko-fi QR code for those who like
// what EliVier Explorer does. SPDX-License-Identifier: GPL-3.0-or-later
//
// The code is a PNG in assets/about. The app hands this screen a loader for it (GL work, which
// screens leave to the app; see set_about_qr), used once when the screen is first drawn.

#include "concepts/concepts.hpp"

#include "concepts/explorer/look.hpp"
#include "core/tween.hpp"

#include <string>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;
using namespace explorer;

constexpr const char *kLink = "ko-fi.com/H5L629ZHNX";

constexpr const char *kTechniques[] = {
    "The QR code is a PNG in the app's assets, decoded once with stb_image into a texture",
    "It leads to ko-fi.com/H5L629ZHNX; nothing is sent from the console",
    "Without the PNG the screen still shows the link as text",
};

constexpr app::TourStep kTour[] = {
    {1.2f, 0, Direction::none, "about"},
};

// Set by the app: how to turn the PNG into a texture, and where the PNG is.
std::uint32_t (*g_load_png)(const std::string &path) = nullptr;
std::string g_qr_path;

class AboutScreen final : public app::Concept
{
  public:
    explicit AboutScreen(app::Context &context) : context_(context)
    {
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "about",
            "About",
            "What EliVier means, and a coffee or a beer",
            "src/concepts/about.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x1779ba),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const InputFrame & /*input*/, float dt, app::Feedback & /*feedback*/) override
    {
        age_ += dt;
    }

    void draw(app::Frame &frame) const override
    {
        const ui::Theme &look = theme();
        begin_frame(frame, look);
        ui::Painter paint(frame.scene, context_.fonts, look);
        frame.scene.push_opacity(tween::cubic_out(age_ / 0.3f));
        draw_header(paint, "About", "EliVier Explorer");
        if (!tried_)
        {
            tried_ = true;
            texture_ = g_load_png != nullptr ? g_load_png(g_qr_path) : 0;
        }

        constexpr float kQrSize = 560.0f;
        const Rect qr{kMargin, kTop + 20.0f, kQrSize, kQrSize};
        if (texture_ != 0)
            frame.scene.image(texture_, qr, gfx::kFullUv, Color::rgb(0xffffff), look.radius_card);
        else
            paint.fill(qr, look.radius_card, look.surface_high);

        const float x = kMargin + kQrSize + 80.0f;
        const float width = kRight - x;
        // What the name means comes first: it is the reason the program exists.
        float y = kTop + 50.0f;
        paint.label("THE NAME", x, y, 20.0f, paint.page_text_muted());
        y += 52.0f;
        y = paragraph(paint,
                      "EliVier stands for Elise and Olivier, my children. This program is a "
                      "dedication to them.",
                      x, y, 30.0f, width, paint.page_text());
        y += 56.0f;
        paint.heading("Enjoying EliVier Explorer?", x, y, 56.0f, paint.page_text());
        y += 80.0f;
        y = paragraph(paint,
                      "If you like what I make, you can buy me a coffee or a beer. Scan the code "
                      "with your phone. Thank you!",
                      x, y, 30.0f, width, paint.page_text_muted());
        y += 40.0f;
        paint.label("KO-FI", x, y, 20.0f, paint.page_text_muted());
        y += 52.0f;
        paint.body(kLink, x, y, 34.0f, look.primary);
        if (texture_ == 0)
        {
            y += 70.0f;
            paint.body("The code could not be loaded; the link above leads to the same page.", x, y,
                       22.0f, paint.page_text_muted());
        }
        static constexpr ui::Hint kHints[] = {
            {ui::Button::circle, "Close"},
        };
        draw_hints(frame.scene, context_.fonts, look, kHints);
        frame.scene.pop_opacity();
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    app::Context &context_;
    float age_ = 0.0f;
    mutable bool tried_ = false;
    mutable std::uint32_t texture_ = 0;
};

} // namespace

void set_about_qr(std::uint32_t (*load_png)(const std::string &path), std::string path)
{
    g_load_png = load_png;
    g_qr_path = std::move(path);
}

std::unique_ptr<app::Concept> make_about(app::Context &context)
{
    return std::make_unique<AboutScreen>(context);
}

} // namespace hui::concepts
