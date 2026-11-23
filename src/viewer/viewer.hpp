// EliVier Explorer - A viewer is what a handler opens: a full-screen view of one file.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The explorer screen holds one viewer at a time, hands it the pad and the frame, and closes it
// when it says so. A viewer never touches GL: pictures go through the TextureHost the app injects,
// sound through the mixer, and blocking work (decoding) runs on the JobWorker.

#pragma once

#include "core/input.hpp"
#include "explorer/source.hpp"
#include "ui/components/component.hpp"
#include "ui/glyphs.hpp"
#include "ui/widgets.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace hui::explorer
{
class JobWorker;
}
namespace hui::audio
{
class Mixer;
}

namespace hui::viewer
{

// Where textures come from. GL lives in the app (and the snapshot tool); tests pass a fake.
class TextureHost
{
  public:
    virtual ~TextureHost() = default;
    // 8-bit RGBA, rows top to bottom; 0 when it could not be made.
    virtual std::uint32_t create(int width, int height, const std::uint8_t *rgba) = 0;
    virtual void destroy(std::uint32_t texture) = 0;
    // A texture for a moving picture, 16 bits a channel (the console uploads those about 15 times
    // faster than 8-bit ones, D52), made empty; fill it with update(). 0 when it could not be made.
    virtual std::uint32_t create_streaming(int width, int height) = 0;
    // New pixels (RGBA, 16 bits a channel, rows top to bottom) for a texture made by
    // create_streaming(), of the same width and height. False when the texture or the size is
    // not right.
    virtual bool update(std::uint32_t texture, int width, int height,
                        const std::uint16_t *rgba) = 0;
};

// What a viewer may use. Anything may be null (a test, or a build without it): a viewer that
// needs one it does not have says so on screen instead of failing.
struct ViewerContext
{
    // Where the viewer is drawn: the same rectangle the screen passes to draw(), so a viewer can
    // size what it shows (rows, columns, pages) in update().
    gfx::Rect area{0.0f, 0.0f, 1728.0f, 900.0f};
    const ui::Fonts *fonts = nullptr;
    hui::explorer::JobWorker *worker = nullptr;
    TextureHost *textures = nullptr;
    hui::audio::Mixer *mixer = nullptr;
};

// The file to open: where it is and what the listing knew of it.
struct FileRef
{
    hui::explorer::Source *source = nullptr;
    std::string path;
    hui::explorer::Entry entry;
};

class Viewer
{
  public:
    virtual ~Viewer() = default;

    virtual std::string title() const = 0;
    virtual void update(const InputFrame &input, float dt, ui::Feedback &feedback) = 0;
    // Must not change state: it may be drawn more than once a frame.
    virtual void draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const = 0;
    virtual std::span<const ui::Hint> hints() const = 0;
    // True once the player asked to leave (Circle); the screen then calls close() and drops it.
    virtual bool closing() const = 0;
    // Gives back what the viewer holds that is not freed by its destructor alone: a playing
    // stream, textures. Called once, before the viewer is destroyed.
    virtual void close()
    {
    }
};

} // namespace hui::viewer
