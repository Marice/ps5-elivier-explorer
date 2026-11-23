// EliVier Explorer - The video viewer: plays a film with its sound and subtitles.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A background thread (video/playback_engine.hpp) decodes; this viewer only asks for the picture
// that belongs to the clock, uploads it, and draws it with the subtitle lines and a control bar.
// The sound plays through the same AudioPlayer as the song viewer and is the clock; a file with
// no sound runs on the frame time. Subtitles are the file's own text tracks and any .srt, .vtt,
// .ass or .ssa beside it with the same name.
//
//   Cross  play / pause (from the start when it has ended)
//   D-pad left/right  10 seconds back / forward     D-pad up/down  volume
//   L1 / R1  the previous / next sound track        Square  subtitles: off, then each track
//   Circle  back
//
// Pictures are converted on the CPU (docs/decision-log.md, D51); how that performs on the console
// has not been measured.

#pragma once

#include "audio/player.hpp"
#include "video/playback_engine.hpp"
#include "viewer/handler.hpp"
#include "viewer/viewer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hui::viewer
{

class VideoViewer final : public Viewer
{
  public:
    VideoViewer(std::string name, hui::explorer::Source *source, std::string path,
                const hui::explorer::Entry &entry, const ViewerContext &context);
    ~VideoViewer() override;

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
    bool fullscreen() const override
    {
        return fullscreen_;
    }
    bool hdr_output() const
    {
        return hdr_on_;
    }
    void close() override;

    const std::string &error() const
    {
        return error_;
    }
    bool playing() const
    {
        return engine_ != nullptr && !paused_ && !ended_;
    }
    bool ended() const
    {
        return ended_;
    }
    double position() const;
    int pictures_shown() const
    {
        return pictures_shown_;
    }
    // "Off" first, then the file's own tracks, then the files beside it.
    std::vector<std::string> subtitle_choices() const;
    int sound_choice() const
    {
        return sound_choice_;
    }
    int subtitle_choice() const
    {
        return subtitle_choice_;
    }
    const hui::video::PlaybackEngine *engine() const
    {
        return engine_.get();
    }

  private:
    struct SubtitleSource
    {
        std::string label;
        int stream = -1;    // an embedded track, or -1
        int external = -1;  // an index into external_, or -1
        std::string language; // "eng", "nl"; empty when unknown
    };

    void find_subtitles(hui::explorer::Source *source, const std::string &path);
    void load_external(hui::explorer::Source *source, const std::string &folder,
                       const std::string &file);
    void show_picture();
    void restart_at(double seconds);
    void cycle_subtitles(double at);
    void cycle_sound(int step, double at);
    std::vector<hui::video::SubtitleLine> subtitle_lines(double at) const;
    gfx::Color ink(gfx::Color colour) const; // a colour drawn over the film, as the display reads it
    void sync_hdr();
    void remember_position();
    void show_note(std::string text);

    std::string name_;
    TextureHost *textures_ = nullptr;
    std::unique_ptr<hui::video::PlaybackEngine> engine_;
    std::unique_ptr<hui::audio::SoundPlayer> player_; // the mixer's (stereo) or a surround one
    bool surround_ = false;
    std::uint32_t texture_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    hui::video::VideoPicture picture_; // the last one taken, a buffer to reuse
    int pictures_shown_ = 0;
    int traced_updates_ = 0;     // trace lines for the first updates after the first picture
    float stats_clock_ = 0.0f;  // seconds since the last trace line
    int stats_shown_ = 0;       // pictures_shown_ at the last trace line
    int stats_late_ = 0;

    std::string error_;
    std::vector<SubtitleSource> subtitle_sources_; // [0] is "Off"
    std::vector<hui::video::SubtitleTrack> external_;
    int subtitle_choice_ = 0;
    std::vector<int> sound_streams_;
    int sound_choice_ = 0;

    double free_clock_ = 0.0; // used when the file has no sound
    bool paused_ = false;
    bool ended_ = false;
    bool closing_ = false;
    bool fullscreen_ = false;
    HdrOutput *hdr_ = nullptr; // the display's HDR10 mode, when there is one
    MediaMemory *memory_ = nullptr; // resume positions and languages, when there is one
    std::string path_;
    float memory_timer_ = 0.0f; // seconds since the position was last written
    std::string note_;          // a short line over the film ("Resumed at 1:23:45")
    float note_timer_ = 0.0f;
    bool hdr_film_ = false;    // the picture is PQ (HDR10)
    bool hdr_on_ = false;      // the display is in HDR10 for this film now
    float overlay_ = 4.0f; // seconds the control bar stays up after the last input
    float pending_seek_ = 0.0f;
    float seek_cooldown_ = 0.0f;
};

class VideoHandler final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "video";
    }
    const char *label() const override
    {
        return "Video";
    }
    const char *verb() const override
    {
        return "Play";
    }
    IconKind icon() const override
    {
        return IconKind::video;
    }
    Match match(const FileProbe &probe) const override;
    std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const override;
};

} // namespace hui::viewer
