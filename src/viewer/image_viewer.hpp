// EliVier Explorer - The image viewer: a picture fitted to the screen, with zoom and pan.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JPEG, PNG, BMP, GIF (first frame) and TGA. The file is read and decoded on the worker so the
// frame never waits; the texture is made on the frame thread through the TextureHost and given back
// when the viewer closes. The zoom and pan are the kit's ui::ImageViewer.
//
//   Cross  fit / fill / 1:1          L2 / R2 (triggers)  zoom
//   left stick  pan                  D-pad  pan, where there is more picture
//   Circle  back to fit, then close

#pragma once

#include "gfx/decode_image.hpp"
#include "ui/components/image_viewer.hpp"
#include "viewer/handler.hpp"
#include "viewer/viewer.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hui::viewer
{

constexpr std::uint64_t kMaxImageFileBytes = 128ull << 20; // a larger file is not read

class ImageFileViewer final : public Viewer
{
  public:
    ImageFileViewer(std::string name, FileRef file, const ViewerContext &context);
    ~ImageFileViewer() override;

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
    void close() override;

    // For tests and the status line.
    bool loading() const;
    bool ready() const
    {
        return texture_ != 0;
    }
    const std::string &error() const
    {
        return error_;
    }
    int picture_width() const
    {
        return width_;
    }
    int picture_height() const
    {
        return height_;
    }

  private:
    // What the worker hands over; shared, so a job that outlives the viewer has somewhere to write.
    struct Job
    {
        std::mutex mutex;
        bool done = false;
        bool cancelled = false;
        gfx::ImageData image;
        std::string error;
    };
    void take_result();

    std::string name_;
    TextureHost *textures_;
    std::shared_ptr<Job> job_;
    ui::ImageViewer viewer_;
    std::uint32_t texture_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::string error_;
    std::string caption_;
    bool closing_ = false;
};

class ImageHandler final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "image";
    }
    const char *label() const override
    {
        return "Image";
    }
    const char *verb() const override
    {
        return "View image";
    }
    IconKind icon() const override
    {
        return IconKind::image;
    }
    Match match(const FileProbe &probe) const override;
    std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const override;
};

} // namespace hui::viewer
