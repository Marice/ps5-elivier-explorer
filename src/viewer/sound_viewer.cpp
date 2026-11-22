// EliVier Explorer - The sound viewer: plays a song, or the songs of a playlist file.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/sound_viewer.hpp"

#include "audio/mixer.hpp"
#include "explorer/source.hpp"
#include "gfx/decode_image.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::viewer
{

namespace
{

constexpr float kSeekStep = 10.0f;     // seconds per D-pad press
constexpr float kSeekInterval = 0.12f; // seconds between seeks while the D-pad is held
constexpr float kRowHeight = 36.0f;
constexpr float kCoverSize = 240.0f; // the album picture's box

std::string file_name(const std::string &path)
{
    return path.substr(path.rfind('/') + 1);
}

// `text` cut with "..." so it is at most `width` pixels wide.
std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    if (font.font == nullptr || font.measure(text, size) <= width)
        return text;
    std::string cut = text;
    while (!cut.empty() && font.measure(cut + "...", size) > width)
    {
        // Step back a whole UTF-8 character.
        do
            cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xc0) == 0x80);
    }
    return cut + "...";
}

} // namespace

std::string format_clock(double seconds)
{
    const long total = seconds > 0.0 ? static_cast<long>(seconds) : 0;
    char text[24];
    if (total >= 3600)
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60,
                      total % 60);
    else
        std::snprintf(text, sizeof(text), "%ld:%02ld", total / 60, total % 60);
    return text;
}

SoundViewer::SoundViewer(std::string name, hui::explorer::Source *source,
                         std::vector<PlaylistEntry> tracks, const ViewerContext &context)
    : name_(std::move(name)), source_(source), tracks_(std::move(tracks)),
      textures_(context.textures)
{
    if (context.mixer == nullptr)
    {
        error_ = "There is no audio output";
        return;
    }
    if (source_ == nullptr || tracks_.empty())
    {
        error_ = "There is nothing to play";
        return;
    }
    player_ = std::make_unique<hui::audio::AudioPlayer>(*context.mixer);
    play(0, 1);
}

SoundViewer::~SoundViewer()
{
    close();
}

void SoundViewer::close()
{
    if (player_)
        player_->stop();
    playing_ = false;
    drop_cover();
}

void SoundViewer::drop_cover()
{
    if (cover_ != 0 && textures_ != nullptr)
        textures_->destroy(cover_);
    cover_ = 0;
}

// The tag's picture as a texture; a picture that cannot be decoded is left out.
void SoundViewer::load_cover()
{
    drop_cover();
    if (textures_ == nullptr || !player_ || player_->decoder() == nullptr)
        return;
    const hui::audio::CoverArt &art = player_->decoder()->cover();
    if (art.data.empty())
        return;
    gfx::ImageData image;
    if (!gfx::decode_image(art.data, &image, nullptr))
        return;
    cover_ = textures_->create(image.width, image.height, image.rgba.data());
    cover_width_ = image.width;
    cover_height_ = image.height;
}

void SoundViewer::play(int index, int step)
{
    if (!player_)
        return;
    const int count = static_cast<int>(tracks_.size());
    for (int tried = 0; tried < count; ++tried)
    {
        const int at = ((index % count) + count) % count;
        const std::string &path = tracks_[static_cast<std::size_t>(at)].path;
        hui::explorer::Entry entry;
        std::string problem;
        std::unique_ptr<hui::audio::Decoder> decoder;
        if (!source_->stat(path, &entry))
            problem = "cannot be read";
        else
        {
            hui::explorer::Source *source = source_;
            decoder = hui::audio::open_mp3(
                [source, path](std::uint64_t offset, void *buffer, std::size_t size)
                { return source->read_range(path, offset, buffer, size); }, entry.size, &problem);
        }
        index_ = at;
        if (decoder)
        {
            player_->start(std::move(decoder));
            load_cover();
            playing_ = true;
            error_.clear();
            return;
        }
        error_ = file_name(path) + " " + problem;
        index_ = at;
        index += step;
    }
    player_->stop();
    playing_ = false;
}

void SoundViewer::update(const InputFrame &input, float dt, ui::Feedback & /*feedback*/)
{
    if (input.is_pressed(Action::back))
    {
        closing_ = true;
        close();
        return;
    }
    if (!player_)
        return;
    player_->pump(dt);

    if (input.is_pressed(Action::confirm))
    {
        if (!playing_)
            play(index_, 1);
        else if (player_->finished())
            player_->seek_to(0.0), play(index_, 1);
        else
            player_->set_paused(!player_->paused());
    }
    if (input.is_pressed(Action::page_next))
        play(index_ + 1, 1);
    else if (input.is_pressed(Action::page_prev))
        play(index_ - 1, -1);

    switch (input.nav)
    {
    case Direction::left:
        pending_seek_ -= kSeekStep;
        break;
    case Direction::right:
        pending_seek_ += kSeekStep;
        break;
    case Direction::up:
        player_->set_volume(player_->volume() + 1);
        break;
    case Direction::down:
        player_->set_volume(player_->volume() - 1);
        break;
    case Direction::none:
        break;
    }
    seek_cooldown_ -= dt;
    if (pending_seek_ != 0.0f && seek_cooldown_ <= 0.0f && playing_)
    {
        player_->seek_by(static_cast<double>(pending_seek_));
        pending_seek_ = 0.0f;
        seek_cooldown_ = kSeekInterval;
    }

    // On to the next song of a list when this one has ended.
    if (playing_ && player_->finished() && tracks_.size() > 1)
        play(index_ + 1, 1);
}

std::span<const ui::Hint> SoundViewer::hints() const
{
    static constexpr ui::Hint kSingle[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::circle, "Back"},
    };
    static constexpr ui::Hint kList[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::l1, "Song", ui::Button::r1},
        {ui::Button::circle, "Back"},
    };
    return tracks_.size() > 1 ? std::span<const ui::Hint>(kList)
                              : std::span<const ui::Hint>(kSingle);
}

void SoundViewer::draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const
{
    const ui::Theme &look = paint.theme();
    if (!player_ || !playing_)
    {
        paint.body(error_.empty() ? "Nothing is playing" : error_.c_str(), area.x, area.y + 40.0f,
                   26.0f, paint.page_text_muted());
        return;
    }
    const hui::audio::Decoder &song = *player_->decoder();
    const ui::FontRef &regular = canvas.fonts.regular;
    const ui::FontRef &mono = canvas.fonts.mono;

    // The picture on the left, when the song has one; everything else moves right of it.
    float left = 0.0f;
    if (cover_ != 0 && cover_width_ > 0 && cover_height_ > 0)
    {
        const float box = kCoverSize;
        const float scale = std::min(box / static_cast<float>(cover_width_),
                                     box / static_cast<float>(cover_height_));
        const float w = static_cast<float>(cover_width_) * scale;
        const float h = static_cast<float>(cover_height_) * scale;
        canvas.list.image(cover_, {area.x + (box - w) / 2.0f, area.y + (box - h) / 2.0f, w, h},
                          gfx::kFullUv, {1.0f, 1.0f, 1.0f, 1.0f}, 10.0f);
        left = box + 40.0f;
    }
    const float x = area.x + left;
    const float width = area.w - left;

    // What is playing: the tag's title and artist, or the file name.
    const PlaylistEntry &entry = tracks_[static_cast<std::size_t>(index_)];
    std::string heading = !song.title().empty()  ? song.title()
                          : !entry.title.empty() ? entry.title
                                                 : file_name(entry.path);
    ui::text(canvas.list, regular, fit(regular, heading, 46.0f, width), x, area.y + 56.0f, 46.0f,
             look.text);
    if (!song.artist().empty())
        ui::text(canvas.list, regular, fit(regular, song.artist(), 30.0f, width), x,
                 area.y + 104.0f, 30.0f, look.text_muted);
    ui::text(canvas.list, regular, song.description(), x, area.y + 144.0f, 22.0f, look.text_muted);

    // Time and a progress bar.
    const double length = player_->duration();
    const double at = player_->position();
    std::string clock = format_clock(at);
    if (length > 0.0)
        clock += " / " + format_clock(length);
    ui::text(canvas.list, mono, clock, x, area.y + 222.0f, 40.0f, look.text);
    const gfx::Rect bar{x, area.y + 246.0f, width, 14.0f};
    canvas.list.rounded_rect(bar, 7.0f, look.surface_high);
    if (length > 0.0)
    {
        const float fill = bar.w * static_cast<float>(std::clamp(at / length, 0.0, 1.0));
        if (fill > 1.0f)
            canvas.list.rounded_rect({bar.x, bar.y, std::max(14.0f, fill), bar.h}, 7.0f,
                                     look.primary);
    }

    const char *state = player_->finished() ? "Ended" : player_->paused() ? "Paused" : "Playing";
    char line[120];
    std::snprintf(line, sizeof(line), "%s     Volume %d of 10", state, player_->volume());
    ui::text(canvas.list, regular, line, x, area.y + 300.0f, 24.0f, paint.page_text_muted());
    if (!error_.empty())
        ui::text(canvas.list, regular, error_, x, area.y + 332.0f, 22.0f, look.text_muted);

    // The playlist: the songs around this one.
    if (tracks_.size() > 1)
    {
        const float top = area.y + 380.0f;
        const int rows = std::max(1, static_cast<int>((area.y + area.h - top) / kRowHeight));
        const int count = static_cast<int>(tracks_.size());
        const int first = std::clamp(index_ - rows / 2, 0, std::max(0, count - rows));
        for (int r = 0; r < rows && first + r < count; ++r)
        {
            const int i = first + r;
            const PlaylistEntry &item = tracks_[static_cast<std::size_t>(i)];
            const std::string label = std::to_string(i + 1) + ".  " +
                                      (item.title.empty() ? file_name(item.path) : item.title);
            const float y = top + static_cast<float>(r) * kRowHeight;
            if (i == index_)
                canvas.list.rounded_rect({area.x - 10.0f, y, area.w + 20.0f, kRowHeight - 2.0f},
                                         6.0f, look.surface_high);
            ui::text(canvas.list, regular, fit(regular, label, 24.0f, area.w - 20.0f), area.x,
                     y + 26.0f, 24.0f, i == index_ ? look.text : look.text_muted);
        }
    }
}

Match SoundHandler::match(const FileProbe &probe) const
{
    if (probe.ext() == "mp3" || probe.ext() == "m3u" || probe.ext() == "m3u8")
        return Match::extension;
    if (probe.can_sniff() && probe.head().compare(0, 3, "ID3") == 0)
        return Match::content;
    return Match::none;
}

std::unique_ptr<Viewer> SoundHandler::open(ViewerContext &context, const FileRef &file) const
{
    if (file.source == nullptr)
        return nullptr;
    const std::string name = file_name(file.path);
    const std::string ext = FileProbe(name, 0).ext();
    std::vector<PlaylistEntry> tracks;
    if (ext == "m3u" || ext == "m3u8")
    {
        std::string text(
            static_cast<std::size_t>(std::min<std::uint64_t>(file.entry.size, 1u << 20)), '\0');
        const std::int64_t got = file.source->read_range(file.path, 0, text.data(), text.size());
        if (got < 0)
            return nullptr;
        text.resize(static_cast<std::size_t>(got));
        const std::size_t slash = file.path.rfind('/');
        const std::string folder = slash == std::string::npos ? "." : slash == 0 ? "" : file.path.substr(0, slash);
        tracks = parse_m3u(text, folder);
    }
    else
        tracks.push_back({file.path, ""});
    return std::make_unique<SoundViewer>(name, file.source, std::move(tracks), context);
}

} // namespace hui::viewer
