// EliVier Explorer - The video viewer: plays a film with its sound and subtitles.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/video_viewer.hpp"

#include "audio/mixer.hpp"
#include "audio/player.hpp"
#include "audio/surround.hpp"
#include "explorer/source.hpp"
#include "explorer/trace.hpp"
#include "video/colour.hpp"
#include "viewer/media_memory.hpp"

// The console's heap size (src/runtime/app_heap.c); absent on a PC.
extern "C" std::size_t hui_heap_capacity(int *direct) __attribute__((weak));
#include "ui/glyphs.hpp"
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
constexpr float kHintBand = 90.0f; // the hint row the bar carries in full screen
// The Options menu's items (D73).
constexpr int kTagRestart = 1;
constexpr int kTagSubtitleDelay = 2;
constexpr int kTagSoundDelay = 3;
constexpr int kTagFill = 4;
constexpr double kDelayStep = 0.1;
constexpr double kMaxDelay = 10.0;

std::string signed_seconds(double seconds)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%+.1f s", seconds);
    return text;
}
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
    : name_(std::move(name)), textures_(context.textures), memory_(context.memory), path_(path)
{
    window_area_ = context.area;
    if (source == nullptr)
    {
        error_ = "There is nothing to play";
        return;
    }
    const auto reader = [source, path](std::uint64_t offset, void *buffer, std::size_t size)
    { return source->read_range(path, offset, buffer, size); };
    std::string reason;
    hui::video::EngineOptions options;
    // With the 1 GiB heap (D54) films up to 4K are decoded: a 4K picture at 16 bits a channel is
    // 66 MB, so the budget holds three of them, the conversion is split over four threads and
    // the decoder gets six (assumptions to tune on the console, D59). The old 128 MiB heap keeps
    // the 1080p limits and the engine's default budget, which fit it.
    if (hui_heap_capacity == nullptr || hui_heap_capacity(nullptr) >= (512u << 20))
    {
        options.limits.max_pixels = 3840 * 2176;
        options.limits.max_threads = 6;
        options.picture_budget_bytes = 240u << 20;
        options.convert_threads = 4;
        options.max_buffered_bytes = 32u << 20;
    }
    engine_ = hui::video::PlaybackEngine::open(reader, entry.size, options,
                                               &reason);
    if (!engine_)
    {
        error_ = reason;
        return;
    }

    subtitle_sources_.push_back({"Off", -1, -1, std::string()});
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
    {
        if (stream.kind == hui::video::StreamKind::audio)
            sound_streams_.push_back(stream.index);
        else if (stream.kind == hui::video::StreamKind::subtitle &&
                 !is_picture_subtitle(stream.codec))
            subtitle_sources_.push_back(
                {describe_track(stream, static_cast<int>(subtitle_sources_.size())), stream.index,
                 -1, stream.language});
    }
    find_subtitles(source, path);

    // The languages last picked, when this film has them (D73).
    int chosen_audio = engine_->audio_stream();
    if (memory_ != nullptr && !memory_->audio_language().empty())
        for (const hui::video::StreamInfo &stream : engine_->info().streams)
            if (stream.kind == hui::video::StreamKind::audio &&
                same_language(stream.language, memory_->audio_language()))
            {
                if (stream.index != chosen_audio)
                    engine_->select_audio(stream.index, 0.0);
                chosen_audio = stream.index;
                break;
            }
    if (memory_ != nullptr && memory_->subtitle_choice() == MediaMemory::Subtitles::language)
        for (std::size_t i = 1; i < subtitle_sources_.size(); ++i)
            if (same_language(subtitle_sources_[i].language, memory_->subtitle_language()))
            {
                subtitle_choice_ = static_cast<int>(i);
                if (subtitle_sources_[i].stream >= 0)
                    engine_->select_subtitle(subtitle_sources_[i].stream, 0.0);
                break;
            }
    hdr_ = context.hdr;
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
        if (stream.index == engine_->video_stream() &&
            stream.colour.transfer == hui::video::Transfer::pq)
            hdr_film_ = true;

    // The sound, when there is somewhere to play it. Without it the engine must not keep
    // decoding sound nobody reads: its queue would fill and hold the picture back.
    // More than two channels and an eight-channel output: the film's sound goes there, as it is
    // (D65); the console mixes it down itself for a stereo TV.
    int channels = 0;
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
        if (stream.index == chosen_audio)
            channels = stream.channels;
    if (engine_->has_audio() && channels > 2 && context.surround != nullptr &&
        context.surround->open())
    {
        if (engine_->set_sound_channels(hui::audio::kSurroundChannels))
        {
            hui::video::PlaybackEngine *engine = engine_.get();
            auto surround = std::make_unique<hui::audio::SurroundPlayer>(
                *context.surround,
                [engine](float *out, int frames) { return engine->read_sound(out, frames); },
                [engine](double seconds) { engine->seek(seconds); });
            if (surround->start())
            {
                player_ = std::move(surround);
                surround_ = true;
            }
        }
        if (!surround_)
        {
            context.surround->close();
            engine_->set_sound_channels(2);
        }
        hui::explorer::trace("film: %d channels, %s", channels,
                             surround_ ? "on the eight-channel output" : "mixed down to stereo");
    }
    if (surround_)
    {
        for (std::size_t i = 0; i < sound_streams_.size(); ++i)
            if (sound_streams_[i] == chosen_audio)
                sound_choice_ = static_cast<int>(i);
    }
    else if (engine_->has_audio() && context.mixer != nullptr)
    {
        auto stereo = std::make_unique<hui::audio::AudioPlayer>(*context.mixer);
        if (auto source_of_sound = engine_->make_audio_source())
            stereo->start(std::move(source_of_sound));
        player_ = std::move(stereo);
        for (std::size_t i = 0; i < sound_streams_.size(); ++i)
            if (sound_streams_[i] == chosen_audio)
                sound_choice_ = static_cast<int>(i);
    }
    else if (engine_->has_audio())
    {
        engine_->select_audio(-1, 0.0);
    }
    // Where this film was left, unless that was barely started or the end (D73).
    if (memory_ != nullptr)
    {
        const double resume = memory_->resume_at(path_, engine_->duration());
        if (resume > 0.0)
        {
            restart_at(resume);
            show_note("Resumed at " + format_clock(resume));
        }
    }
    engine_->start();
}

void VideoViewer::show_note(std::string text)
{
    note_ = std::move(text);
    note_timer_ = 4.0f;
}

void VideoViewer::remember_position()
{
    if (memory_ == nullptr || !engine_)
        return;
    memory_->remember_position(path_, position(), engine_->duration());
    memory_->save();
    memory_timer_ = 0.0f;
}

VideoViewer::~VideoViewer()
{
    close();
}

void VideoViewer::close()
{
    remember_position();
    if (hdr_on_ && hdr_ != nullptr)
    {
        hdr_->leave();
        hdr_on_ = false;
        hui::explorer::trace("film: HDR10 output off");
    }
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
    // "Film.nl.srt", "Film.eng.forced.srt": the first tag after the film's name, when it looks
    // like a language (two or three letters).
    std::string language;
    std::string inner = file.substr(0, file.rfind('.'));
    const std::size_t film_end = inner.find('.');
    if (film_end != std::string::npos)
    {
        std::string tag = inner.substr(film_end + 1);
        tag = tag.substr(0, tag.find('.'));
        const bool letters = std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
            return std::isalpha(c) != 0;
        });
        if ((tag.size() == 2 || tag.size() == 3) && letters)
            language = lower(tag);
    }
    subtitle_sources_.push_back({file, -1, static_cast<int>(external_.size()) - 1, language});
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
    at -= subtitle_delay_; // later subtitles show the cue of that much earlier
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
    // Picked by hand: the next film starts with the same (off, or this language when known).
    if (memory_ != nullptr && (subtitle_choice_ == 0 || !source.language.empty()))
        memory_->remember_subtitles(subtitle_choice_ == 0 ? std::string() : source.language);
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
    const int stream = sound_streams_[static_cast<std::size_t>(sound_choice_)];
    if (memory_ != nullptr)
        for (const hui::video::StreamInfo &info : engine_->info().streams)
            if (info.index == stream)
                memory_->remember_audio_language(info.language);
    engine_->select_audio(stream, at);
    restart_at(at);
}

// Takes the picture that belongs to the clock and puts it in the texture.
void VideoViewer::show_picture()
{
    // Pictures are taken even with nowhere to show them: the engine's queue must keep moving, or
    // it stops reading and the sound (the clock) starves.
    // A sound delay shows the picture that belongs that much later than what is heard.
    if (!engine_->take_picture(position() + sound_delay_, picture_) || textures_ == nullptr)
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
    if (pictures_shown_ == 0)
        hui::explorer::trace("film: first picture uploaded");
    ++pictures_shown_;
}

void VideoViewer::open_menu(ui::Feedback &feedback)
{
    std::vector<ui::MenuItem> items;
    const auto item = [](std::string label, int tag, bool disabled = false) {
        ui::MenuItem made;
        made.label = std::move(label);
        made.tag = tag;
        made.disabled = disabled;
        return made;
    };
    items.push_back(item("Start from the beginning", kTagRestart));
    ui::MenuItem subtitles = item("Subtitle delay", kTagSubtitleDelay, subtitle_choice_ == 0);
    subtitles.shortcut = signed_seconds(subtitle_delay_);
    items.push_back(subtitles);
    ui::MenuItem sound = item("Sound delay", kTagSoundDelay, !player_);
    sound.shortcut = signed_seconds(sound_delay_);
    items.push_back(sound);
    ui::MenuItem fill = item("Fill the screen", kTagFill, !fullscreen_);
    fill.checkable = true;
    fill.checked = fill_;
    items.push_back(fill);
    menu_.style.theme = ui::default_theme();
    const gfx::Rect area =
        fullscreen_ ? gfx::Rect{0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight} : window_area_;
    menu_.set_bounds(area);
    menu_.set_items(std::move(items));
    // Above the bottom right corner, where the bar's state text is.
    menu_.open({area.x + area.w - 140.0f, area.y + area.h - kBarHeight, 1.0f, 1.0f}, feedback);
}

void VideoViewer::menu_chosen(int tag)
{
    switch (tag)
    {
    case kTagRestart:
        menu_.dismiss();
        if (memory_ != nullptr)
            memory_->forget(path_);
        if (ended_ || paused_)
        {
            paused_ = false;
            if (player_)
                player_->set_paused(false);
        }
        restart_at(0.0);
        break;
    case kTagSubtitleDelay:
        menu_.dismiss();
        adjust_ = Adjust::subtitles;
        break;
    case kTagSoundDelay:
        menu_.dismiss();
        adjust_ = Adjust::sound;
        break;
    case kTagFill:
        fill_ = !fill_;
        break;
    default:
        break;
    }
}

// Left and right change the delay being set; Cross or Circle leave. False once it is left.
bool VideoViewer::handle_adjust(const InputFrame &input)
{
    if (input.is_pressed(Action::confirm) || input.is_pressed(Action::back) ||
        input.is_pressed(Action::menu))
        return false;
    double &delay = adjust_ == Adjust::subtitles ? subtitle_delay_ : sound_delay_;
    if (input.nav == Direction::left)
        delay = std::max(-kMaxDelay, delay - kDelayStep);
    else if (input.nav == Direction::right)
        delay = std::min(kMaxDelay, delay + kDelayStep);
    delay = std::round(delay * 10.0) / 10.0; // no drift from adding tenths
    return true;
}

void VideoViewer::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    menu_.update(dt);
    // The menu has the pad while it is open; Circle closes it, not the film.
    if (menu_.is_open() && engine_)
    {
        // A plain item confirms as `activated`; a checkable one (Fill the screen) as `changed`,
        // with the box already flipped by the menu itself. Either way our own state follows.
        const ui::Event chosen = menu_.handle(input, feedback);
        if (chosen == ui::Event::activated || chosen == ui::Event::changed)
            menu_chosen(menu_.items()[static_cast<std::size_t>(menu_.focus())].tag);
        if (player_)
            player_->pump(dt);
        sync_hdr();
        show_picture();
        return;
    }
    if (adjust_ != Adjust::none && engine_)
    {
        overlay_ = kOverlaySeconds;
        if (!handle_adjust(input))
            adjust_ = Adjust::none;
        if (player_)
            player_->pump(dt);
        sync_hdr();
        show_picture();
        return;
    }
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


    overlay_ = std::max(0.0f, overlay_ - dt);
    note_timer_ = std::max(0.0f, note_timer_ - dt);
    // The position is written now and then while playing, so a crash or a power cut loses little.
    if (!paused_ && !ended_ && (memory_timer_ += dt) >= 30.0f)
        remember_position();
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
    if (input.is_pressed(Action::north))
    {
        touched = true; // the bar comes up, so the new hints are seen
        fullscreen_ = !fullscreen_;
    }
    if (input.is_pressed(Action::menu))
    {
        touched = true;
        open_menu(feedback);
    }
    // L2/R2: a minute at a time; held, two and then five minutes a step.
    if (const int jump = jump_.step(input, dt); jump != 0)
    {
        jump_repeats_ = input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next)
                            ? 0
                            : jump_repeats_ + 1;
        const float size = jump_repeats_ < 4 ? 60.0f : jump_repeats_ < 10 ? 120.0f : 300.0f;
        pending_seek_ += static_cast<float>(jump) * size;
        touched = true;
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
        double target = std::max(0.0, now + static_cast<double>(pending_seek_));
        if (length > 1.0)
            target = std::min(target, length - 0.5);
        restart_at(target);
        pending_seek_ = 0.0f;
        seek_cooldown_ = kSeekInterval;
    }
    if (touched)
        overlay_ = kOverlaySeconds;

    sync_hdr();
    show_picture();

    if (!ended_ && engine_->ended() && (!player_ || player_->finished()))
        ended_ = true;
}

std::span<const ui::Hint> VideoViewer::hints() const
{
    static constexpr ui::Hint kBasic[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::l2, "1 min", ui::Button::r2},
        {ui::Button::triangle, "Full screen"},
        {ui::Button::options, "Menu"},
        {ui::Button::circle, "Back"},
    };
    static constexpr ui::Hint kTracks[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::l1, "Sound", ui::Button::r1},
        {ui::Button::square, "Subtitles"},
        {ui::Button::triangle, "Full screen"},
        {ui::Button::options, "Menu"},
        {ui::Button::circle, "Back"},
    };
    static constexpr ui::Hint kSubtitles[] = {
        {ui::Button::cross, "Play / pause"},
        {ui::Button::dpad, "Seek, volume"},
        {ui::Button::square, "Subtitles"},
        {ui::Button::triangle, "Full screen"},
        {ui::Button::options, "Menu"},
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

// In HDR10 mode the display reads PQ BT.2020 codes, so what is drawn over the film is drawn in
// those: white at 203 nits (D68). The film itself is not passed through here.
gfx::Color VideoViewer::ink(gfx::Color colour) const
{
    if (!hdr_on_)
        return colour;
    const hui::video::Rgb mapped = hui::video::sdr_to_hdr10({colour.r, colour.g, colour.b});
    return {mapped.r, mapped.g, mapped.b, colour.a};
}

// HDR10 while an HDR film plays in full screen, SDR otherwise (D68).
void VideoViewer::sync_hdr()
{
    const bool want = fullscreen_ && hdr_film_ && hdr_ != nullptr && !closing_ && engine_ &&
                      !menu_.is_open();
    if (want == hdr_on_)
        return;
    if (want)
    {
        hdr_on_ = hdr_->enter();
        hui::explorer::trace("film: HDR10 output %s", hdr_on_ ? "on" : "refused");
    }
    else
    {
        hdr_->leave();
        hdr_on_ = false;
        hui::explorer::trace("film: HDR10 output off");
    }
    if (engine_)
        engine_->set_hdr_passthrough(hdr_on_);
}

void VideoViewer::draw(ui::Painter &paint, ui::Canvas &canvas, const gfx::Rect &area) const
{
    canvas.list.rounded_rect(area, 0.0f, kBlack);
    const ui::FontRef &regular = canvas.fonts.regular;
    const ui::FontRef &mono = canvas.fonts.mono;
    if (!engine_)
    {
        paint.body(error_.empty() ? "Nothing is playing" : error_.c_str(), area.x + 24.0f,
                   area.y + 64.0f, 28.0f, ink(kWhite));
        return;
    }

    // The picture, as large as fits, centred.
    gfx::Rect shown = area;
    if (texture_ != 0 && texture_width_ > 0 && texture_height_ > 0)
    {
        const float fit = std::min(area.w / static_cast<float>(texture_width_),
                                   area.h / static_cast<float>(texture_height_));
        const float cover = std::max(area.w / static_cast<float>(texture_width_),
                                     area.h / static_cast<float>(texture_height_));
        const bool filling = fill_ && fullscreen_;
        const float scale = filling ? cover : fit;
        const float w = static_cast<float>(texture_width_) * scale;
        const float h = static_cast<float>(texture_height_) * scale;
        shown = {area.x + (area.w - w) / 2.0f, area.y + (area.h - h) / 2.0f, w, h};
        gfx::Rect uv = gfx::kFullUv;
        if (filling)
        {
            // Only the part inside the screen is drawn: the texture rectangle shrinks to it.
            const float keep_u = std::min(1.0f, area.w / w);
            const float keep_v = std::min(1.0f, area.h / h);
            uv = {(1.0f - keep_u) / 2.0f, (1.0f - keep_v) / 2.0f, keep_u, keep_v};
            shown = area;
        }
        canvas.list.image(texture_, shown, uv, {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f);
    }
    else if (!engine_->has_video())
    {
        const std::string note = engine_->video_error().empty()
                                     ? std::string("Sound only")
                                     : "No picture: " + engine_->video_error();
        ui::text(canvas.list, regular, note, area.x + area.w / 2.0f, area.y + area.h / 2.0f,
                 30.0f, ink(kMuted), gfx::Align::center);
    }

    // The bar stays while the menu or a delay is being set: the menu and the panel are drawn with it.
    const bool bar = overlay_ > 0.0f || paused_ || ended_ || menu_.visible() || adjust_ != Adjust::none;
    const double at = position();

    // Subtitles, with a dark edge so that they read on any picture.
    const std::vector<hui::video::SubtitleLine> cues = subtitle_lines(at);
    // Full screen has no hint row below the viewer: the bar carries its own under the controls.
    const float bar_height = kBarHeight + (fullscreen_ ? kHintBand : 0.0f);
    float bottom_y = std::min(shown.y + shown.h, area.y + area.h - (bar ? bar_height : 0.0f)) - 40.0f;
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
                         ink({0.0f, 0.0f, 0.0f, 0.85f}), gfx::Align::center);
            ui::text(canvas.list, regular, line, cx, y, size, ink(kWhite), gfx::Align::center);
            y += step;
        }
        if (cue->align == hui::video::CueAlign::top)
            top_y = y;
        else if (cue->align == hui::video::CueAlign::bottom)
            bottom_y -= static_cast<float>(lines.size()) * step;
    }

    if (note_timer_ > 0.0f && !note_.empty())
    {
        const float alpha = std::min(1.0f, note_timer_ / 0.5f);
        const float inset = fullscreen_ ? 96.0f : 32.0f;
        const float width = regular.font != nullptr ? regular.measure(note_, 28.0f) : 300.0f;
        canvas.list.rounded_rect({area.x + inset, area.y + 40.0f, width + 40.0f, 52.0f}, 10.0f,
                                 ink({0.0f, 0.0f, 0.0f, 0.62f * alpha}));
        ui::text(canvas.list, regular, note_, area.x + inset + 20.0f, area.y + 76.0f, 28.0f,
                 ink({1.0f, 1.0f, 1.0f, alpha}));
    }
    if (!bar)
        return;
    // The control bar: what is playing, the time, the progress and the tracks in use.
    const gfx::Rect panel{area.x, area.y + area.h - bar_height, area.w, kBarHeight};
    canvas.list.rounded_rect({panel.x, panel.y, panel.w, bar_height}, 0.0f,
                             ink({0.0f, 0.0f, 0.0f, 0.62f}));
    if (fullscreen_)
    {
        // Light glyphs and white labels on the dark bar, whatever the theme.
        ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        glyphs.label = ink(kWhite);
        glyphs.body = ink(glyphs.body);
        glyphs.edge = ink(glyphs.edge);
        glyphs.ink = ink(glyphs.ink);
        ui::HintLayout layout;
        layout.cy = area.y + area.h - kHintBand / 2.0f - 8.0f;
        const std::span<const ui::Hint> row = hints();
        ui::draw_hints(canvas.list, canvas.fonts, glyphs, row.data(), static_cast<int>(row.size()),
                       area.x + area.w - 96.0f, true, layout);
    }
    const float inset = fullscreen_ ? 96.0f : 32.0f; // full screen keeps to the safe area
    const float x = panel.x + inset;
    const float width = panel.w - 2.0f * inset;
    ui::text(canvas.list, regular, fit(regular, name_, 30.0f, width), x, panel.y + 40.0f, 30.0f,
             ink(kWhite));

    std::string info;
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
    {
        if (stream.index == engine_->video_stream())
        {
            info = stream.codec + " " + std::to_string(stream.width) + "x" +
                   std::to_string(stream.height);
            if (stream.bit_depth > 8)
                info += " " + std::to_string(stream.bit_depth) + "-bit";
            if (engine_->video_decoder_name().rfind("hardware", 0) == 0)
                info += " (hardware)";
            if (stream.colour.transfer == hui::video::Transfer::pq)
                info += hdr_on_ ? " HDR10" : " HDR (shown as SDR)";
        }
    }
    const int sound = engine_->audio_stream();
    for (const hui::video::StreamInfo &stream : engine_->info().streams)
        if (stream.index == sound)
            info += std::string(info.empty() ? "" : "     ") + "Sound: " +
                    describe_track(stream, sound_choice_ + 1) +
                    (surround_ ? ", surround out" : "");
    if (subtitle_sources_.size() > 1)
    {
        const SubtitleSource &chosen =
            subtitle_sources_[static_cast<std::size_t>(subtitle_choice_)];
        info += std::string(info.empty() ? "" : "     ") + "Subtitles: " + chosen.label;
    }
    ui::text(canvas.list, regular, fit(regular, info, 22.0f, width), x, panel.y + 74.0f, 22.0f,
             ink(kMuted));

    const double length = engine_->duration();
    std::string clock = format_clock(at);
    if (length > 0.0)
        clock += " / " + format_clock(length);
    const char *state = ended_ ? "Ended" : paused_ ? "Paused" : "Playing";
    ui::text(canvas.list, mono, clock, x, panel.y + 122.0f, 30.0f, ink(kWhite));
    std::string right = state;
    if (player_)
        right += "     Volume " + std::to_string(player_->volume()) + " of 10";
    ui::text(canvas.list, regular, right, x + width, panel.y + 122.0f, 24.0f, ink(kMuted),
             gfx::Align::right);
    const gfx::Rect track{x, panel.y + 90.0f, width, 8.0f};
    canvas.list.rounded_rect(track, 4.0f, ink({1.0f, 1.0f, 1.0f, 0.25f}));
    if (length > 0.0)
    {
        const float fill = track.w * static_cast<float>(std::clamp(at / length, 0.0, 1.0));
        if (fill > 1.0f)
            canvas.list.rounded_rect({track.x, track.y, std::max(8.0f, fill), track.h}, 4.0f,
                                     ink(paint.theme().primary));
    }
    if (adjust_ != Adjust::none)
    {
        const std::string label = std::string(adjust_ == Adjust::subtitles ? "Subtitle delay "
                                                                          : "Sound delay ") +
                                  signed_seconds(adjust_ == Adjust::subtitles ? subtitle_delay_
                                                                              : sound_delay_);
        const float panel_w = 560.0f;
        const gfx::Rect box{area.x + (area.w - panel_w) / 2.0f, area.y + area.h / 2.0f - 70.0f,
                            panel_w, 140.0f};
        canvas.list.rounded_rect(box, 16.0f, ink({0.0f, 0.0f, 0.0f, 0.78f}));
        ui::text(canvas.list, regular, label, box.x + box.w / 2.0f, box.y + 62.0f, 36.0f,
                 ink(kWhite), gfx::Align::center);
        ui::text(canvas.list, regular, "Left / right: 0.1 s     Cross: done", box.x + box.w / 2.0f,
                 box.y + 108.0f, 22.0f, ink(kMuted), gfx::Align::center);
    }
    menu_.draw(canvas);
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
