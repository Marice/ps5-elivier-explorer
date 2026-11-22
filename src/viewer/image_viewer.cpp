// EliVier Explorer - The image viewer: a picture fitted to the screen, with zoom and pan.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/image_viewer.hpp"

#include "explorer/format.hpp"
#include "explorer/worker.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::viewer
{

namespace
{

std::string file_name(const std::string &path)
{
    return path.substr(path.rfind('/') + 1);
}

// Reads the whole file, in pieces (a short read from a drive is not the end).
bool read_all(const hui::explorer::Source &source, const std::string &path, std::uint64_t size,
              std::string *out)
{
    out->assign(static_cast<std::size_t>(size), '\0');
    std::uint64_t done = 0;
    while (done < size)
    {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(size - done, 4u << 20));
        const std::int64_t got = source.read_range(path, done, out->data() + done, want);
        if (got < 0)
            return false;
        if (got == 0)
            break;
        done += static_cast<std::uint64_t>(got);
    }
    out->resize(static_cast<std::size_t>(done));
    return true;
}

} // namespace

ImageFileViewer::ImageFileViewer(std::string name, FileRef file, const ViewerContext &context)
    : name_(std::move(name)), textures_(context.textures)
{
    viewer_.style.filmstrip = false;
    viewer_.style.counter = false;
    viewer_.style.arrows = false;
    viewer_.style.max_zoom = 8.0f;
    viewer_.set_bounds(context.area);
    if (textures_ == nullptr)
    {
        error_ = "This build cannot show pictures";
        return;
    }
    if (file.source == nullptr || context.worker == nullptr)
    {
        error_ = "There is no way to read the picture";
        return;
    }
    if (file.entry.size > kMaxImageFileBytes)
    {
        error_ = "The file is too large to open as a picture";
        return;
    }
    job_ = std::make_shared<Job>();
    std::shared_ptr<Job> job = job_;
    hui::explorer::Source *source = file.source;
    const std::string path = file.path;
    const std::uint64_t size = file.entry.size;
    context.worker->queue(
        [job, source, path, size]
        {
            std::string bytes;
            gfx::ImageData image;
            std::string problem;
            bool cancelled;
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                cancelled = job->cancelled;
            }
            if (!cancelled)
            {
                if (!read_all(*source, path, size, &bytes))
                    problem = "The file cannot be read";
                else
                    gfx::decode_image(bytes, &image, &problem);
            }
            std::lock_guard<std::mutex> lock(job->mutex);
            job->image = std::move(image);
            job->error = std::move(problem);
            job->done = true;
        });
}

ImageFileViewer::~ImageFileViewer()
{
    close();
}

bool ImageFileViewer::loading() const
{
    return job_ != nullptr && texture_ == 0 && error_.empty();
}

void ImageFileViewer::close()
{
    if (job_)
    {
        std::lock_guard<std::mutex> lock(job_->mutex);
        job_->cancelled = true;
    }
    if (texture_ != 0 && textures_ != nullptr)
        textures_->destroy(texture_);
    texture_ = 0;
}

void ImageFileViewer::take_result()
{
    gfx::ImageData image;
    {
        std::lock_guard<std::mutex> lock(job_->mutex);
        if (!job_->done)
            return;
        image = std::move(job_->image);
        error_ = job_->error;
    }
    if (error_.empty())
    {
        texture_ = textures_->create(image.width, image.height, image.rgba.data());
        if (texture_ == 0)
            error_ = "The picture could not be put on the screen";
    }
    if (texture_ != 0)
    {
        width_ = image.width;
        height_ = image.height;
        char text[64];
        if (image.source_width != image.width)
            std::snprintf(text, sizeof(text), "%d x %d (shown at %d x %d)", image.source_width,
                          image.source_height, image.width, image.height);
        else
            std::snprintf(text, sizeof(text), "%d x %d", image.width, image.height);
        caption_ = text;
        ui::ViewerImage shown;
        shown.title = name_;
        shown.caption = caption_;
        shown.texture = texture_;
        shown.aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
        shown.pixel_width = static_cast<float>(image.width);
        viewer_.set_images({shown});
    }
}

void ImageFileViewer::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    if (texture_ == 0 && job_ && error_.empty())
        take_result();
    if (texture_ == 0)
    {
        if (input.is_pressed(Action::back))
            closing_ = true;
        return;
    }
    viewer_.set_active(true);
    if (viewer_.handle(input, feedback) == ui::Event::cancelled)
        closing_ = true;
    viewer_.update(dt);
}

void ImageFileViewer::draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const
{
    if (texture_ != 0)
    {
        viewer_.draw(canvas);
        return;
    }
    const char *text = loading() ? "Loading the picture..." : error_.c_str();
    paint.body(text, area.x, area.y + 40.0f, 26.0f, paint.page_text_muted());
}

std::span<const ui::Hint> ImageFileViewer::hints() const
{
    static constexpr ui::Hint kHints[] = {
        {ui::Button::cross, "Fit / fill / 1:1"},
        {ui::Button::l2, "Zoom", ui::Button::r2},
        {ui::Button::left_stick, "Pan"},
        {ui::Button::circle, "Back"},
    };
    return kHints;
}

Match ImageHandler::match(const FileProbe &probe) const
{
    const std::string &ext = probe.ext();
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || ext == "gif" ||
        ext == "tga" || ext == "dds")
        return Match::extension;
    if (probe.can_sniff() && gfx::looks_like_image(probe.head()))
        return Match::content;
    return Match::none;
}

std::unique_ptr<Viewer> ImageHandler::open(ViewerContext &context, const FileRef &file) const
{
    if (file.source == nullptr)
        return nullptr;
    return std::make_unique<ImageFileViewer>(file_name(file.path), file, context);
}

} // namespace hui::viewer
