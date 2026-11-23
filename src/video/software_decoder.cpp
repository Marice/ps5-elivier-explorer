// EliVier Explorer - Video decoding on the CPU with libavcodec, and dav1d for AV1.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/software_decoder.hpp"

#include "video/ffmpeg_colour.hpp"

#include <algorithm>
#include <cerrno>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace hui::video
{

namespace
{

constexpr int kMaxDimension = 16384;
// A run of damaged pictures this long ends one receive() call, so a broken stream cannot keep the
// game thread inside it.
constexpr int kMaxConsecutiveDamage = 64;

std::string error_text(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

struct FormatInfo
{
    int bit_depth = 8;
    int chroma_shift_x = 1;
    int chroma_shift_y = 1;
    bool full_range = false; // the format itself says so (the deprecated yuvj formats)
};

// What the GPU path reads: three planes of planar YCbCr (an alpha plane is ignored), one
// component per plane, all of one depth, 8 bits per byte or low-aligned 16-bit little-endian words.
bool describe_format(int format, FormatInfo &info)
{
    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    if (descriptor == nullptr)
        return false;
    const std::uint64_t refused = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL |
                                  AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_BE |
                                  AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER |
                                  AV_PIX_FMT_FLAG_FLOAT;
    if ((descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) == 0 || (descriptor->flags & refused) != 0)
        return false;
    const bool has_alpha = (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    if (descriptor->nb_components != (has_alpha ? 4 : 3))
        return false;
    const int depth = descriptor->comp[0].depth;
    if (depth < 8 || depth > 16)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const AVComponentDescriptor &component = descriptor->comp[i];
        if (component.plane != i || component.depth != depth || component.shift != 0 ||
            component.offset != 0 || component.step != (depth > 8 ? 2 : 1))
            return false;
    }
    if (descriptor->log2_chroma_w > 2 || descriptor->log2_chroma_h > 2)
        return false;
    info.bit_depth = depth;
    info.chroma_shift_x = descriptor->log2_chroma_w;
    info.chroma_shift_y = descriptor->log2_chroma_h;
    info.full_range = format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
                      format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P;
    return true;
}

void free_frame(AVFrame *frame)
{
    av_frame_free(&frame);
}

class SoftwareDecoder final : public VideoDecoder
{
  public:
    SoftwareDecoder(AVCodecContext *context, std::string name, StreamTiming timing,
                    const SoftwareLimits &limits)
        : context_(context), name_(std::move(name)), timing_(timing), limits_(limits),
          scratch_(av_frame_alloc())
    {
    }

    ~SoftwareDecoder() override
    {
        av_frame_free(&scratch_);
        avcodec_free_context(&context_);
    }

    const std::string &name() const override
    {
        return name_;
    }

    SendResult send(const Packet &packet) override
    {
        if (!packet.data)
            return SendResult::error;
        return interpret(avcodec_send_packet(context_, packet.data.get()));
    }

    SendResult send_end() override
    {
        if (draining_)
            return SendResult::accepted;
        draining_ = true;
        return interpret(avcodec_send_packet(context_, nullptr));
    }

    ReceiveResult receive(Frame &frame) override
    {
        if (scratch_ == nullptr)
            return ReceiveResult::error;
        for (int attempt = 0; attempt < kMaxConsecutiveDamage; ++attempt)
        {
            const int result = avcodec_receive_frame(context_, scratch_);
            if (result == AVERROR(EAGAIN))
                return ReceiveResult::need_input;
            if (result == AVERROR_EOF)
                return ReceiveResult::end;
            if (result == AVERROR_INVALIDDATA)
            {
                ++damaged_;
                continue;
            }
            if (result < 0)
                return ReceiveResult::error;
            return fill(frame) ? ReceiveResult::frame : ReceiveResult::error;
        }
        return ReceiveResult::need_input;
    }

    void flush() override
    {
        avcodec_flush_buffers(context_);
        draining_ = false;
    }

    int damaged_packets() const override
    {
        return damaged_;
    }

  private:
    SendResult interpret(int result)
    {
        if (result >= 0)
            return SendResult::accepted;
        if (result == AVERROR(EAGAIN))
            return SendResult::full;
        if (result == AVERROR_INVALIDDATA)
        {
            ++damaged_;
            return SendResult::accepted;
        }
        if (result == AVERROR_EOF && draining_)
            return SendResult::accepted;
        return SendResult::error;
    }

    bool fill(Frame &out)
    {
        FormatInfo info;
        if (!describe_format(scratch_->format, info) || scratch_->width <= 0 ||
            scratch_->height <= 0 || scratch_->width > kMaxDimension ||
            scratch_->height > kMaxDimension ||
            static_cast<long long>(scratch_->width) * scratch_->height > limits_.max_pixels ||
            scratch_->linesize[0] <= 0 || scratch_->linesize[1] <= 0 || scratch_->linesize[2] <= 0)
        {
            av_frame_unref(scratch_);
            return false;
        }

        // The frame keeps its buffers alive; the scratch frame is free for the next picture.
        std::shared_ptr<AVFrame> kept(av_frame_alloc(), &free_frame);
        if (!kept)
        {
            av_frame_unref(scratch_);
            return false;
        }
        av_frame_move_ref(kept.get(), scratch_);
        const AVFrame &source = *kept;

        out = Frame{};
        for (int i = 0; i < 3; ++i)
        {
            out.planes[i] = source.data[i];
            out.pitches[i] = source.linesize[i];
        }
        out.plane_count = 3;
        out.semi_planar = false;
        out.width = source.width;
        out.height = source.height;
        out.bit_depth = info.bit_depth;
        out.chroma_shift_x = info.chroma_shift_x;
        out.chroma_shift_y = info.chroma_shift_y;

        // Tags on the picture win; a picture that says nothing takes what the stream said.
        const auto pick = [](auto frame_value, auto stream_value, auto unspecified) {
            return frame_value != unspecified ? frame_value : stream_value;
        };
        const AVColorRange range =
            info.full_range
                ? AVCOL_RANGE_JPEG
                : pick(source.color_range, context_->color_range, AVCOL_RANGE_UNSPECIFIED);
        out.colour = colour_info(
            pick(source.color_trc, context_->color_trc, AVCOL_TRC_UNSPECIFIED),
            pick(source.colorspace, context_->colorspace, AVCOL_SPC_UNSPECIFIED),
            pick(source.color_primaries, context_->color_primaries, AVCOL_PRI_UNSPECIFIED), range,
            source.height);

        const std::int64_t stamp = source.best_effort_timestamp != AV_NOPTS_VALUE
                                       ? source.best_effort_timestamp
                                       : source.pts;
        out.pts = stamp != AV_NOPTS_VALUE
                      ? static_cast<double>(stamp) * timing_.seconds_per_tick - timing_.start_offset
                      : kNoTimestamp;
        out.owner = std::move(kept);
        return true;
    }

    AVCodecContext *context_;
    std::string name_;
    StreamTiming timing_;
    SoftwareLimits limits_;
    AVFrame *scratch_;
    bool draining_ = false;
    int damaged_ = 0;
};

} // namespace

std::unique_ptr<VideoDecoder> open_software_decoder(const Demuxer &demuxer, int stream,
                                                    const SoftwareLimits &limits,
                                                    std::string *error)
{
    const auto fail = [error](const std::string &message) -> std::unique_ptr<VideoDecoder> {
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    const AVCodecParameters *parameters = demuxer.parameters(stream);
    if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_VIDEO)
        return fail("That is not a video stream");
    if (parameters->width <= 0 || parameters->height <= 0 || parameters->width > kMaxDimension ||
        parameters->height > kMaxDimension)
        return fail("The video has no usable picture size");
    if (static_cast<long long>(parameters->width) * parameters->height > limits.max_pixels)
        return fail("The picture is too large for software decoding (" +
                    std::to_string(parameters->width) + " x " + std::to_string(parameters->height) +
                    ")");

    // FFmpeg's own AV1 decoder only wraps hardware; dav1d is the software one.
    const AVCodec *codec = parameters->codec_id == AV_CODEC_ID_AV1
                               ? avcodec_find_decoder_by_name("libdav1d")
                               : avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr)
        return fail(std::string("This build cannot decode ") +
                    avcodec_get_name(parameters->codec_id));

    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr)
        return fail("Out of memory");
    int result = avcodec_parameters_to_context(context, parameters);
    if (result < 0)
    {
        avcodec_free_context(&context);
        return fail("Cannot set up the decoder (" + error_text(result) + ")");
    }
    context->thread_count = std::max(limits.max_threads, 1);
    context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    result = avcodec_open2(context, codec, nullptr);
    if (result < 0)
    {
        avcodec_free_context(&context);
        return fail("Cannot start the decoder (" + error_text(result) + ")");
    }
    auto decoder = std::make_unique<SoftwareDecoder>(context, codec->name,
                                                     demuxer.timing(stream), limits);
    return decoder;
}

} // namespace hui::video
