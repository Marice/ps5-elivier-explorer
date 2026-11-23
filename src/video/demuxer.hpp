// EliVier Explorer - Opens a media file and hands out its compressed packets in file order.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libavformat does the work, reading through a ByteReader (a range read over a Source), so a
// film is never held in memory whole. FFmpeg's types stay out of this header; the decoders in
// this module include the FFmpeg headers themselves. A file is untrusted input: the number of
// streams, the amount probed and the size of one packet are capped, and no protocol is compiled
// in, so a file cannot make the player open another file or a network address.

#pragma once

#include "audio/decoder.hpp"
#include "video/av_sync.hpp"
#include "video/colour.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVCodecParameters;
struct AVPacket;

namespace hui::video
{

enum class StreamKind
{
    video,
    audio,
    subtitle,
    other, // attachments (fonts), data, thumbnails: kept so that indices match, never played
};

struct StreamInfo
{
    int index = -1;
    StreamKind kind = StreamKind::video;
    std::string codec;    // FFmpeg's short name: "h264", "hevc", "aac", "subrip"
    std::string profile;  // "High", "Main 10"; empty when the codec has none
    std::string language; // as the file says it ("eng"); empty when unknown
    std::string title;
    // Video
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    double frame_rate = 0.0;
    ColourInfo colour;
    // Audio
    int channels = 0;
    int sample_rate = 0;
};

struct MediaInfo
{
    std::string container; // "mov,mp4,m4a,3gp,3g2,mj2", "matroska,webm"
    double duration = 0.0; // seconds; 0 when the file does not say
    std::vector<StreamInfo> streams;
};

struct AVPacketDeleter
{
    void operator()(AVPacket *packet) const;
};
using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct Packet
{
    PacketPtr data;
    int stream = -1; // index into MediaInfo::streams
    double pts = kNoTimestamp; // seconds from the start of the file
    double duration = 0.0;
    bool keyframe = false;
};

enum class ReadResult
{
    packet,
    end,
    error,
};

class Demuxer
{
  public:
    ~Demuxer();
    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;

    // Null and `error` filled when the file is not media that this build can read.
    static std::unique_ptr<Demuxer> open(audio::ByteReader reader, std::uint64_t size,
                                         std::string *error);

    const MediaInfo &info() const;

    // The next packet of any stream, in file order. `end` stays `end` until a seek.
    ReadResult read(Packet &packet);

    // To the last keyframe at or before `seconds` (the picture is then decoded forward to the
    // wanted time). False when the file cannot seek there.
    bool seek(double seconds);

    // For the decoders. Valid for the life of the demuxer; null for an index out of range.
    const AVCodecParameters *parameters(int stream) const;

  private:
    struct Impl;
    explicit Demuxer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::video
