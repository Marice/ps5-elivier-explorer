// EliVier Explorer - The sound viewer: plays a song, or the songs of a playlist file.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MP3 for now (a decoder per format plugs into audio/decoder.hpp). A .m3u file opens as a list of
// songs played in order. The music stops when the viewer closes. Tags (title, artist) and the
// picture in the tag (JPEG or PNG) are shown.
//
//   Cross  play / pause (again from the start when it has ended)
//   D-pad left/right  10 seconds back / forward      D-pad up/down  volume
//   L1 / R1  the previous / next song of a playlist  Circle  back (stops the music)
//
// The sound plays on the music bus, so it also follows Music volume in the settings.

#pragma once

#include "audio/player.hpp"
#include "viewer/handler.hpp"
#include "viewer/playlist.hpp"
#include "viewer/viewer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hui::viewer
{

// "3:07", or "1:02:03" past an hour.
std::string format_clock(double seconds);

class SoundViewer final : public Viewer
{
  public:
    // `tracks` are absolute paths on `source`; `name` is what the title bar shows.
    SoundViewer(std::string name, hui::explorer::Source *source, std::vector<PlaylistEntry> tracks,
                const ViewerContext &context);
    ~SoundViewer() override;

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

    int track_count() const
    {
        return static_cast<int>(tracks_.size());
    }
    int track_index() const
    {
        return index_;
    }
    const std::string &error() const
    {
        return error_;
    }
    const hui::audio::AudioPlayer *player() const
    {
        return player_.get();
    }
    // Plays song `index` of the list; a song that cannot be played is skipped in `step` direction.
    void play(int index, int step);

  private:
    std::string name_;
    hui::explorer::Source *source_;
    std::vector<PlaylistEntry> tracks_;
    TextureHost *textures_ = nullptr;
    std::uint32_t cover_ = 0; // the picture of the song playing, 0 when it has none
    int cover_width_ = 0;
    int cover_height_ = 0;
    void load_cover();
    void drop_cover();
    std::unique_ptr<hui::audio::AudioPlayer> player_;
    int index_ = 0;
    std::string error_;
    float pending_seek_ = 0.0f; // seconds asked for, applied a few times a second
    float seek_cooldown_ = 0.0f;
    bool playing_ = false; // a song is loaded (even when it has ended)
    bool closing_ = false;
};

class SoundHandler final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "sound";
    }
    const char *label() const override
    {
        return "Sound";
    }
    const char *verb() const override
    {
        return "Play";
    }
    IconKind icon() const override
    {
        return IconKind::audio;
    }
    Match match(const FileProbe &probe) const override;
    std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const override;
};

} // namespace hui::viewer
