// EliVier Explorer - Text subtitle tracks inside a media file (Matroska, MP4): packets to cues.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libavcodec turns every text format (SubRip, WebVTT, mov_text, ASS and SSA, MicroDVD ...) into
// ASS dialogue lines; the cue is that line's text with the markup removed (subtitle_text.hpp).
// Styled ASS is drawn as plain text until libass is added; picture subtitles (PGS, VobSub, DVB)
// are not read yet and are refused when the track is opened.

#pragma once

#include "video/demuxer.hpp"
#include "video/subtitle_text.hpp"

#include <memory>
#include <string>

namespace hui::video
{

class SubtitleDecoder
{
  public:
    virtual ~SubtitleDecoder() = default;

    // "subrip", "ass", "webvtt", "mov_text".
    virtual const std::string &name() const = 0;

    // Adds the cue(s) of one packet to the track. Seeking makes the demuxer deliver a packet
    // again; the track drops a cue it already has. A damaged packet is counted, not fatal.
    virtual void decode(const Packet &packet, SubtitleTrack &track) = 0;
    // After a seek: forget what is in flight. The cues already in the track stay.
    virtual void flush() = 0;

    virtual int damaged_packets() const = 0;
};

// Null and `error` filled when the stream is not a text subtitle this build can read.
std::unique_ptr<SubtitleDecoder> open_subtitle_decoder(const Demuxer &demuxer, int stream,
                                                       std::string *error);

} // namespace hui::video
