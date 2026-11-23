// EliVier Explorer - Text subtitle tracks inside a media file (Matroska, MP4): packets to cues.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/subtitle_decoder.hpp"

#include <cstdint>
#include <string_view>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace hui::video
{

namespace
{

std::string error_text(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

// libavcodec's ASS dialogue for a packet: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,
// Effect,Text". The text is what follows the eighth comma, and may hold commas itself.
std::string_view text_of_dialogue(std::string_view line)
{
    std::size_t at = 0;
    for (int commas = 0; commas < 8; ++commas)
    {
        at = line.find(',', at);
        if (at == std::string_view::npos)
            return {};
        ++at;
    }
    return line.substr(at);
}

class FfmpegSubtitleDecoder final : public SubtitleDecoder
{
  public:
    FfmpegSubtitleDecoder(AVCodecContext *context, std::string name)
        : context_(context), name_(std::move(name))
    {
    }

    ~FfmpegSubtitleDecoder() override
    {
        avcodec_free_context(&context_);
    }

    const std::string &name() const override
    {
        return name_;
    }

    void decode(const Packet &packet, SubtitleTrack &track) override
    {
        if (!packet.data || packet.pts == kNoTimestamp || packet.pts < 0.0)
            return;
        AVSubtitle subtitle{};
        int got = 0;
        const int result = avcodec_decode_subtitle2(context_, &subtitle, &got, packet.data.get());
        if (result < 0)
        {
            ++damaged_;
            return;
        }
        if (got != 0)
        {
            // Times: the packet's own, plus the offsets the decoder may add (in milliseconds).
            const double start =
                packet.pts + static_cast<double>(subtitle.start_display_time) / 1000.0;
            double end = packet.pts + packet.duration;
            if (subtitle.end_display_time != 0 && subtitle.end_display_time != UINT32_MAX)
                end = packet.pts + static_cast<double>(subtitle.end_display_time) / 1000.0;
            for (unsigned int i = 0; i < subtitle.num_rects; ++i)
            {
                const AVSubtitleRect *rect = subtitle.rects[i];
                if (rect == nullptr || rect->type != SUBTITLE_ASS || rect->ass == nullptr)
                    continue;
                const std::string_view text = text_of_dialogue(rect->ass);
                add_cue(track, start, end, text);
            }
        }
        avsubtitle_free(&subtitle);
    }

    void flush() override
    {
        avcodec_flush_buffers(context_);
    }

    int damaged_packets() const override
    {
        return damaged_;
    }

  private:
    AVCodecContext *context_;
    std::string name_;
    int damaged_ = 0;
};

} // namespace

std::unique_ptr<SubtitleDecoder> open_subtitle_decoder(const Demuxer &demuxer, int stream,
                                                       std::string *error)
{
    const auto fail = [error](const std::string &message) -> std::unique_ptr<SubtitleDecoder> {
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    const AVCodecParameters *parameters = demuxer.parameters(stream);
    if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_SUBTITLE)
        return fail("That is not a subtitle stream");
    const AVCodecDescriptor *descriptor = avcodec_descriptor_get(parameters->codec_id);
    if (descriptor == nullptr || (descriptor->props & AV_CODEC_PROP_TEXT_SUB) == 0)
        return fail("Picture subtitles (PGS, VobSub, DVB) are not supported yet");
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr)
        return fail(std::string("This build cannot read ") +
                    avcodec_get_name(parameters->codec_id));
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr)
        return fail("Out of memory");
    int result = avcodec_parameters_to_context(context, parameters);
    if (result >= 0)
        result = avcodec_open2(context, codec, nullptr);
    if (result < 0)
    {
        avcodec_free_context(&context);
        return fail("Cannot start the subtitle decoder (" + error_text(result) + ")");
    }
    return std::make_unique<FfmpegSubtitleDecoder>(context, codec->name);
}

} // namespace hui::video
