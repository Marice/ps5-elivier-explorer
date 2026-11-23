// EliVier Explorer - The video viewer: plays a film with its sound and subtitles.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/video_viewer.hpp"

#include "audio/mixer.hpp"
#include "explorer/source.hpp"
#include "explorer/trace.hpp"
#include "viewer/sound_viewer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace hui::viewer
{

namespace
{

constexpr float kSeekStep = 10.0f;     // seconds per D-pad press
constexpr float kSeekInterval = 0.12f; // seconds between seeks while the D-pad is held
constexpr float kOverlaySeconds = 4.0f;
constexpr float kBarHeight = 150.0f;
constexpr std::size_t kMaxExternalSubtitles = 8;
constexpr float kSubtitleSize = 38.0f;

const gfx::Color kWhite{0.96f, 0.96f, 0.96f, 1.0f};
const gfx::Color kMuted{0.72f, 0.72f, 0.74f, 1.0f};
const gfx::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};

std::string file_name(const std::string &path)
{
    return path.substr(path.rfind('/') + 1);
}

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string extension_of(const std::string &name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}

// Picture subtitles (PGS, VobSub, DVB) are not read yet: they are left out of the choices.
bool is_picture_subtitle(const std::string &codec)
{
    return codec == "hdmv_pgs_subtitle" || codec == "dvd_subtitle" || codec == "dvb_subtitle" ||
           codec == "xsub" || codec == "dvb_teletext";
}

std::string describe_track(const hui::video::StreamInfo &stream, int number)
{
    std::string label = !stream.title.empty()      ? stream.title
                        : !stream.language.empty() ? stream.language
                                                   : "Track " + std::to_string(number);
    label += " (" + stream.codec;
    if (stream.kind == hui::video::StreamKind::audio && stream.channels > 0)
        label += ", " + std::to_string(stream.channels) + " ch";
    return label + ")";
}

// `text` cut with "..." so it is at most `width` pixels wide.
std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    if (font.font == nullptr || font.measure(text, size) <= width)
        return text;
    std::string cut = text;
    while (!cut.empty() && font.measure(cut + "...", size) > width)
    {
        do
            cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xc0) == 0x80);
    }
    return cut + "...";
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

} // namespace

VideoViewer::VideoViewer(std::string name, hui::explorer::Source *source, std::string path,
                         const hui::explorer::Entry &entry, const ViewerContext &context)
    : name_(std::move(name)), textures_(context.textures)
{
    if (source == nullptr)
    {
        error_ = "There is nothing to play";
        return;
    }
    const auto reader = [source, path](std::uint64_t offset, void *buffer, std::size_t size)
    { return source->read_range(path, offset, buffer, size); };
    std::string reason;
    engine_ = hui::video::PlaybackEngine::open(reader, entry.size, hui::video::EngineOptions{},
                                               &reason);
    if (!engine_)
    {
        error_ = reason;
        return;
    }

    subtitle_sources_.push_back({"Off", -1, -1});
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
    {
        if (stream.kind == hui::video::StreamKind::audio)
            sound_streams_.push_back(stream.index);
        else if (stream.kind == hui::video::StreamKind::subtitle &&
                 !is_picture_subtitle(stream.codec))
            subtitle_sources_.push_back(
                {describe_track(stream, static_cast<int>(subtitle_sources_.size())), stream.index,
                 -1});
    }
    find_subtitles(source, path);

    // The sound, when there is somewhere to play it. Without it the engine must not keep
    // decoding sound nobody reads: its queue would fill and hold the picture back.
    if (engine_->has_audio() && context.mixer != nullptr)
    {
        player_ = std::make_unique<hui::audio::AudioPlayer>(*context.mixer);
        if (auto source_of_sound = engine_->make_audio_source())
            player_->start(std::move(source_of_sound));
        for (std::size_t i = 0; i < sound_streams_.size(); ++i)
            if (sound_streams_[i] == engine_->audio_stream())
                sound_choice_ = static_cast<int>(i);
    }
    else if (engine_->has_audio())
    {
        engine_->select_audio(-1, 0.0);
    }
    engine_->start();
}

VideoViewer::~VideoViewer()
{
    close();
}

void VideoViewer::close()
{
    if (player_)
        player_->stop(); // before the engine: the player holds its sound source
    player_.reset();
    if (engine_)
        engine_->stop();
    if (texture_ != 0 && textures_ != nullptr)
        textures_->destroy(texture_);
    texture_ = 0;
    engine_.reset();
}

double VideoViewer::position() const
{
    if (player_)
        return player_->position();
    return free_clock_;
}

// Text files beside the film that begin with its name: "Film.srt", "Film.nl.srt".
void VideoViewer::find_subtitles(hui::explorer::Source *source, const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    const std::string folder = slash == std::string::npos ? "."
                               : slash == 0               ? "/"
                                                          : path.substr(0, slash);
    std::string stem = file_name(path);
    const std::size_t dot = stem.rfind('.');
    if (dot != std::string::npos)
        stem.resize(dot);
    stem = lower(stem);
    const hui::explorer::ListResult listing = source->list(folder);
    if (!listing.ok)
        return;
    for (const hui::explorer::Entry &entry : listing.entries)
    {
        if (external_.size() >= kMaxExternalSubtitles)
            break;
        if (entry.kind != hui::explorer::EntryKind::file)
            continue;
        const std::string ext = extension_of(entry.name);
        if (ext != "srt" && ext != "vtt" && ext != "ass" && ext != "ssa")
            continue;
        if (lower(entry.name).compare(0, stem.size(), stem) != 0)
            continue;
        load_external(source, folder, entry.name);
    }
}

void VideoViewer::load_external(hui::explorer::Source *source, const std::string &folder,
                                const std::string &file)
{
    hui::explorer::Entry entry;
    const std::string path = (folder == "/" ? "" : folder) + "/" + file;
    if (!source->stat(path, &entry) || entry.size == 0 ||
        entry.size > hui::video::kMaxSubtitleFileBytes)
        return;
    std::string bytes(static_cast<std::size_t>(entry.size), '\0');
    const std::int64_t got = source->read_range(path, 0, bytes.data(), bytes.size());
    if (got <= 0)
        return;
    bytes.resize(static_cast<std::size_t>(got));
    hui::video::SubtitleTrack track;
    std::string problem;
    if (!hui::video::load_subtitle_file(bytes, track, &problem))
        return;
    external_.push_back(std::move(track));
    subtitle_sources_.push_back({file, -1, static_cast<int>(external_.size()) - 1});
}

std::vector<std::string> VideoViewer::subtitle_choices() const
{
    std::vector<std::string> labels;
    for (const SubtitleSource &source : subtitle_sources_)
        labels.push_back(source.label);
    return labels;
}

std::vector<hui::video::SubtitleLine> VideoViewer::subtitle_lines(double at) const
{
    if (!engine_ || subtitle_choice_ <= 0 ||
        subtitle_choice_ >= static_cast<int>(subtitle_sources_.size()))
        return {};
    const SubtitleSource &source = subtitle_sources_[static_cast<std::size_t>(subtitle_choice_)];
    if (source.stream >= 0)
        return engine_->subtitles_at(at);
    std::vector<hui::video::SubtitleLine> lines;
    for (const hui::video::Cue *cue :
         external_[static_cast<std::size_t>(source.external)].active_at(at))
        lines.push_back({cue->text, cue->align});
    return lines;
}

// Everything restarts from a time: the sound player (its clock), the engine's queues.
void VideoViewer::restart_at(double seconds)
{
    const double target = std::max(seconds, 0.0);
    if (player_)
        player_->seek_to(target);
    else
    {
        engine_->seek(target);
        free_clock_ = target;
    }
    ended_ = false;
}

void VideoViewer::cycle_subtitles(double at)
{
    subtitle_choice_ = (subtitle_choice_ + 1) % static_cast<int>(subtitle_sources_.size());
    const SubtitleSource &source = subtitle_sources_[static_cast<std::size_t>(subtitle_choice_)];
    if (source.stream >= 0)
    {
        engine_->select_subtitle(source.stream, at);
        restart_at(at);
    }
    else if (engine_->subtitle_stream() >= 0)
    {
        engine_->select_subtitle(-1, at); // an outside file or nothing: the file's track stops
        restart_at(at);
    }
}

void VideoViewer::cycle_sound(int step, double at)
{
    if (sound_streams_.size() < 2)
        return;
    const int count = static_cast<int>(sound_streams_.size());
    sound_choice_ = ((sound_choice_ + step) % count + count) % count;
    engine_->select_audio(sound_streams_[static_cast<std::size_t>(sound_choice_)], at);
    restart_at(at);
}

// Takes the picture that belongs to the clock and puts it in the texture.
void VideoViewer::show_picture()
{
    // Pictures are taken even with nowhere to show them: the engine's queue must keep moving, or
    // it stops reading and the sound (the clock) starves.
    if (!engine_->take_picture(position(), picture_) || textures_ == nullptr)
        return;
    if (texture_ != 0 && (picture_.width != texture_width_ || picture_.height != texture_height_))
    {
        textures_->destroy(texture_);
        texture_ = 0;
    }
    if (texture_ == 0)
    {
        texture_ = textures_->create_streaming(picture_.width, picture_.height);
        texture_width_ = picture_.width;
        texture_height_ = picture_.height;
        if (texture_ == 0)
        {
            error_ = "The picture cannot be shown";
            return;
        }
    }
    if (!textures_->update(texture_, picture_.width, picture_.height, picture_.rgba.data()))
    {
        error_ = "The picture cannot be shown";
        return;
    }
    ++pictures_shown_;
}

void VideoViewer::update(const InputFrame &input, float dt, ui::Feedback & /*feedback*/)
{
    if (input.is_pressed(Action::back))
    {
        closing_ = true;
        close();
        return;
    }
    if (!engine_)
        return;
    if (player_)
        player_->pump(dt);
    else if (!paused_ && !ended_)
        free_clock_ += static_cast<double>(dt);

    // One line a second while it plays, so a console run shows whether it keeps up (D53): pictures
    // shown, thrown away late and ready, and times the sound had to wait.
    if (!paused_ && !ended_)
    {
        stats_clock_ += dt;
        if (stats_clock_ >= 1.0f)
        {
            const hui::video::PlaybackEngine::Stats stats = engine_->stats();
            hui::explorer::trace("video t=%.1f shown=%d late=%d ready=%d starved=%llu",
                                 position(), pictures_shown_ - stats_shown_,
                                 stats.pictures_late - stats_late_, stats.pictures_ready,
                                 static_cast<unsigned long long>(stats.starved_reads));
            stats_shown_ = pictures_shown_;
            stats_late_ = stats.pictures_late;
            stats_clock_ = 0.0f;
        }
    }

    overlay_ = std::max(0.0f, overlay_ - dt);
    const double now = position();
    bool touched = false;

    if (input.is_pressed(Action::confirm))
    {
        touched = true;
        if (ended_)
        {
            paused_ = false;
            if (player_)
                player_->set_paused(false);
            restart_at(0.0);
        }
        else
        {
            paused_ = !paused_;
            if (player_)
                player_->set_paused(paused_);
        }
    }
    if (input.is_pressed(Action::page_next))
    {
        touched = true;
        cycle_sound(1, now);
    }
    else if (input.is_pressed(Action::page_prev))
    {
        touched = true;
        cycle_sound(-1, now);
    }
    if (input.is_pressed(Action::west))
    {
        touched = true;
        cycle_subtitles(now);
    }
    switch (input.nav)
    {
    case Direction::left:
        pending_seek_ -= kSeekStep;
        touched = true;
        break;
    case Direction::right:
        pending_seek_ += kSeekStep;
        touched = true;
        break;
    case Direction::up:
        if (player_)
            player_->set_volume(player_->volume() + 1);
        touched = true;
        break;
    case Direction::down:
        if (player_)
            player_->set_volume(player_->volume() - 1);
        touched = true;
        break;
    case Direction::none:
        break;
    }
    seek_cooldown_ -= dt;
    if (pending_seek_ != 0.0f && seek_cooldown_ <= 0.0f)
    {
        const double length = engine_->duration();
        double target = now + static_cast<double>(pending_seek_);
        if (length > 1.0)
            target = std::min(target, length - 0.5);
        restart_at(target);
        pending_seek_ = 0.0f;
        seek_cooldown_ = kSeekInterval;
    }
    if (touched)
        overlay_ = kOverlaySeconds;

    show_picture();

    if (!ended_ && engine_->ended() && (!player_ || player_->finished()))
        ended_ = true;
}

std::span<const ui::Hint> VideoViewer::hints() const
{
    static constexpr ui::Hint kBasic[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::circle, "Back"},
    };
    static constexpr ui::Hint kTracks[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::l1, "Sound", ui::Button::r1},
        {ui::Button::square, "Subtitles"},
        {ui::Button::circle, "Back"},
    };
    static constexpr ui::Hint kSubtitles[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::square, "Subtitles"},
        {ui::Button::circle, "Back"},
    };
    const bool sounds = sound_streams_.size() > 1;
    const bool subtitles = subtitle_sources_.size() > 1;
    if (sounds && subtitles)
        return kTracks;
    if (subtitles)
        return kSubtitles;
    return kBasic;
}

void VideoViewer::draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const
{
    canvas.list.rounded_rect(area, 0.0f, kBlack);
    const ui::FontRef &regular = canvas.fonts.regular;
    const ui::FontRef &mono = canvas.fonts.mono;
    if (!engine_)
    {
        paint.body(error_.empty() ? "Nothing is playing" : error_.c_str(), area.x + 24.0f,
                   area.y + 64.0f, 28.0f, kWhite);
        return;
    }

    // The picture, as large as fits, centred.
    gfx::Rect shown = area;
    if (texture_ != 0 && texture_width_ > 0 && texture_height_ > 0)
    {
        const float scale = std::min(area.w / static_cast<float>(texture_width_),
                                     area.h / static_cast<float>(texture_height_));
        const float w = static_cast<float>(texture_width_) * scale;
        const float h = static_cast<float>(texture_height_) * scale;
        shown = {area.x + (area.w - w) / 2.0f, area.y + (area.h - h) / 2.0f, w, h};
        canvas.list.image(texture_, shown, gfx::kFullUv, {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f);
    }
    else if (!engine_->has_video())
    {
        const std::string note = engine_->video_error().empty()
                                     ? std::string("Sound only")
                                     : "No picture: " + engine_->video_error();
        ui::text(canvas.list, regular, note, area.x + area.w / 2.0f, area.y + area.h / 2.0f,
                 30.0f, kMuted, gfx::Align::center);
    }

    const bool bar = overlay_ > 0.0f || paused_ || ended_;
    const double at = position();

    // Subtitles, with a dark edge so that they read on any picture.
    const std::vector<hui::video::SubtitleLine> cues = subtitle_lines(at);
    float bottom_y = shown.y + shown.h - 40.0f - (bar ? kBarHeight : 0.0f);
    float top_y = shown.y + 64.0f;
    for (auto cue = cues.rbegin(); cue != cues.rend(); ++cue)
    {
        const std::vector<std::string> lines = split_lines(cue->text);
        const float step = kSubtitleSize * 1.25f;
        float y = cue->align == hui::video::CueAlign::top ? top_y
                  : cue->align == hui::video::CueAlign::middle
                      ? shown.y + shown.h / 2.0f
                      : bottom_y - static_cast<float>(lines.size() - 1) * step;
        for (const std::string &line : lines)
        {
            float size = kSubtitleSize;
            const float limit = shown.w - 80.0f;
            const float width = regular.font != nullptr ? regular.measure(line, size) : 0.0f;
            if (width > limit && width > 0.0f)
                size = std::max(kSubtitleSize * 0.6f, kSubtitleSize * limit / width);
            const float cx = shown.x + shown.w / 2.0f;
            static const float kEdge[4][2] = {
                {-2.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, -2.0f}, {0.0f, 2.0f}};
            for (const auto &edge : kEdge)
                ui::text(canvas.list, regular, line, cx + edge[0], y + edge[1], size,
                         {0.0f, 0.0f, 0.0f, 0.85f}, gfx::Align::center);
            ui::text(canvas.list, regular, line, cx, y, size, kWhite, gfx::Align::center);
            y += step;
        }
        if (cue->align == hui::video::CueAlign::top)
            top_y = y;
        else if (cue->align == hui::video::CueAlign::bottom)
            bottom_y -= static_cast<float>(lines.size()) * step;
    }

    if (!bar)
        return;
    // The control bar: what is playing, the time, the progress and the tracks in use.
    const gfx::Rect panel{area.x, area.y + area.h - kBarHeight, area.w, kBarHeight};
    canvas.list.rounded_rect(panel, 0.0f, {0.0f, 0.0f, 0.0f, 0.62f});
    const float x = panel.x + 32.0f;
    const float width = panel.w - 64.0f;
    ui::text(canvas.list, regular, fit(regular, name_, 30.0f, width), x, panel.y + 40.0f, 30.0f,
             kWhite);

    std::string info;
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
    {
        if (stream.index == engine_->video_stream())
        {
            info = stream.codec + " " + std::to_string(stream.width) + "x" +
                   std::to_string(stream.height);
            if (stream.bit_depth > 8)
                info += " " + std::to_string(stream.bit_depth) + "-bit";
            if (stream.colour.transfer == hui::video::Transfer::pq)
                info += " HDR (shown as SDR)";
        }
    }
    const int sound = engine_->audio_stream();
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
        if (stream.index == sound)
            info += std::string(info.empty() ? "" : "     ") + "Sound: " +
                    describe_track(stream, sound_choice_ + 1);
    if (subtitle_sources_.size() > 1)
    {
        const SubtitleSource &chosen =
            subtitle_sources_[static_cast<std::size_t>(subtitle_choice_)];
        info += std::string(info.empty() ? "" : "     ") + "Subtitles: " + chosen.label;
    }
    ui::text(canvas.list, regular, fit(regular, info, 22.0f, width), x, panel.y + 74.0f, 22.0f,
             kMuted);

    const double length = engine_->duration();
    std::string clock = format_clock(at);
    if (length > 0.0)
        clock += " / " + format_clock(length);
    const char *state = ended_ ? "Ended" : paused_ ? "Paused" : "Playing";
    ui::text(canvas.list, mono, clock, x, panel.y + 122.0f, 30.0f, kWhite);
    std::string right = state;
    if (player_)
        right += "     Volume " + std::to_string(player_->volume()) + " of 10";
    ui::text(canvas.list, regular, right, x + width, panel.y + 122.0f, 24.0f, kMuted,
             gfx::Align::right);
    const gfx::Rect track{x, panel.y + 90.0f, width, 8.0f};
    canvas.list.rounded_rect(track, 4.0f, {1.0f, 1.0f, 1.0f, 0.25f});
    if (length > 0.0)
    {
        const float fill = track.w * static_cast<float>(std::clamp(at / length, 0.0, 1.0));
        if (fill > 1.0f)
            canvas.list.rounded_rect({track.x, track.y, std::max(8.0f, fill), track.h}, 4.0f,
                                     paint.theme().primary);
    }
}

Match VideoHandler::match(const FileProbe &probe) const
{
    static const char *const kExtensions[] = {"mp4", "m4v", "mkv", "webm", "avi", "mov", "ts",
                                              "m2ts", "mts", "wmv", "flv", "mpg", "mpeg", "ogv",
                                              "3gp"};
    for (const char *ext : kExtensions)
        if (probe.ext() == ext)
            return Match::extension;
    if (probe.can_sniff())
    {
        const std::string &head = probe.head();
        if (head.size() >= 4 && head.compare(0, 4, "\x1A\x45\xDF\xA3") == 0)
            return Match::content; // Matroska and WebM
        if (head.size() >= 12 && head.compare(4, 4, "ftyp") == 0)
            return Match::content; // MP4 and QuickTime
        if (head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "AVI ") == 0)
            return Match::content;
    }
    return Match::none;
}

std::unique_ptr<Viewer> VideoHandler::open(ViewerContext &context, const FileRef &file) const
{
    if (file.source == nullptr)
        return nullptr;
    return std::make_unique<VideoViewer>(file_name(file.path), file.source, file.path, file.entry,
                                         context);
}

} // namespace hui::viewer
