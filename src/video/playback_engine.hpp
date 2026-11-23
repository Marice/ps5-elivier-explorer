// EliVier Explorer - The playback engine: one file in, pictures, sound and subtitles out in time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A background thread demuxes the file, decodes the picture, the chosen sound track and the
// chosen subtitle track, converts pictures to RGBA and keeps all of it in bounded queues. The
// game thread never waits for it: it asks for "the picture for this clock" (take_picture), hands
// the sound to the mixer through audio::AudioPlayer (make_audio_source), and reads the subtitle
// lines for a time. Audio is the clock (src/video/av_sync.hpp); the viewer supplies it.
//
// pump() is one step of the thread's work and is public so that tests drive the engine without a
// thread, in a fixed order. Nothing here touches GL, the mixer or the console.

#pragma once

#include "audio/decoder.hpp"
#include "video/convert.hpp"
#include "video/demuxer.hpp"
#include "video/software_decoder.hpp"
#include "video/subtitle_text.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hui::video
{

struct VideoPicture
{
    double pts = kNoTimestamp;
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> rgba; // 16 bits a channel: width * height * 4, rows top to bottom
};

struct SubtitleLine
{
    std::string text;
    CueAlign align = CueAlign::bottom;
};

struct EngineOptions
{
    SoftwareLimits limits;
    int picture_queue = 4;             // converted pictures kept ready (8.3 MB each at 1080p)
    double audio_queue_seconds = 2.0;  // decoded sound kept ready
};

class PlaybackEngine
{
  public:
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine &) = delete;
    PlaybackEngine &operator=(const PlaybackEngine &) = delete;

    // Null and `error` filled when the file cannot be played at all. A file whose picture cannot
    // be decoded (too large, an unknown codec) but whose sound can is opened for sound; see
    // video_error(). The reader must stay valid for the life of the engine.
    static std::unique_ptr<PlaybackEngine> open(audio::ByteReader reader, std::uint64_t size,
                                                const EngineOptions &options, std::string *error);

    const MediaInfo &info() const;
    double duration() const;
    bool has_video() const;
    bool has_audio() const;
    std::string video_error() const; // empty when the picture decodes
    std::string audio_error() const;
    // The stream index in use (-1 for none). A change takes effect when the engine has caught up.
    int video_stream() const;
    int audio_stream() const;
    int subtitle_stream() const;

    // ---- the work ----
    // One step: handles a request, then moves one packet, picture or chunk of sound along. True
    // when it did something; false when there is nothing to do now (queues full, or the end).
    bool pump();
    void start(); // a thread that calls pump() until stop()
    void stop();

    // ---- requests (game thread) ----
    // To `seconds`: everything queued is dropped at once and decoding resumes from the keyframe
    // before it; pictures and sound before `seconds` are discarded, not shown.
    void seek(double seconds);
    // Switch tracks. `resume_at` is the current position: the demuxer is some way ahead of it, so
    // the switch restarts from there. -1 for no sound or no subtitles.
    void select_audio(int stream, double resume_at);
    void select_subtitle(int stream, double resume_at);

    // ---- consumption (game thread) ----
    // The newest picture whose time has come for `clock_seconds` (older ones are dropped); false
    // when there is none yet.
    bool take_picture(double clock_seconds, VideoPicture &picture);
    // The lines showing at `seconds` of the chosen subtitle track.
    std::vector<SubtitleLine> subtitles_at(double seconds) const;
    // Sound for audio::AudioPlayer::start(): stereo float at 48 kHz. An empty queue before the end
    // reads as audio::Decoder::kTryLater, so that the player writes nothing and its clock waits
    // for the sound (writing silence would make the sound trail the picture by that much); 0
    // only at the end. At most one at a time, and it must be gone before the engine is.
    std::unique_ptr<audio::Decoder> make_audio_source();

    // Everything is decoded and shown or heard.
    bool ended() const;

    struct Stats
    {
        int pictures_ready = 0;
        double audio_seconds_ready = 0.0;
        int pictures_late = 0;       // thrown away because their time had passed
        int damaged_video = 0;
        int damaged_audio = 0;
        std::uint64_t starved_reads = 0; // times the player asked for sound and the queue had none
    };
    Stats stats() const;

    struct Impl;

  private:
    explicit PlaybackEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::video
