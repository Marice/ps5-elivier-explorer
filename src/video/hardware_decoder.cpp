// EliVier Explorer - Video decoded by the console's hardware, behind the same seam as software.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/hardware_decoder.hpp"

#include <algorithm>
#include <deque>
#include <set>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

namespace hui::video
{

namespace
{

HardwareProvider &provider()
{
    static HardwareProvider instance;
    return instance;
}

// Refusals in a row after which the stream is given up: one damaged packet is not an error.
constexpr int kMaxRefusals = 30;

struct BsfDeleter
{
    void operator()(AVBSFContext *context) const
    {
        av_bsf_free(&context);
    }
};
using BsfPtr = std::unique_ptr<AVBSFContext, BsfDeleter>;

class HardwareDecoder final : public VideoDecoder
{
  public:
    HardwareDecoder(std::unique_ptr<HardwareBackend> backend, const AVCodecParameters &parameters,
                    HardwareStream stream, ColourInfo colour)
        : backend_(std::move(backend)), stream_(stream), colour_(colour),
          parameters_(avcodec_parameters_alloc()), scratch_(av_packet_alloc())
    {
        if (parameters_ != nullptr)
            avcodec_parameters_copy(parameters_, &parameters);
        filter_ = make_filter();
    }

    ~HardwareDecoder() override
    {
        av_packet_free(&scratch_);
        avcodec_parameters_free(&parameters_);
    }

    bool ready() const
    {
        return filter_ != nullptr && scratch_ != nullptr;
    }

    const std::string &name() const override
    {
        return backend_->name();
    }

    SendResult send(const Packet &packet) override
    {
        if (!packet.data || !ready())
            return SendResult::error;
        if (!ready_.empty())
            return SendResult::full; // the pictures that came out hold the backend's buffers
        // After a seek the hardware needs a keyframe first; anything before it cannot be decoded.
        if (waiting_for_keyframe_)
        {
            if (!packet.keyframe)
                return SendResult::accepted;
            waiting_for_keyframe_ = false;
        }
        if (av_packet_ref(scratch_, packet.data.get()) < 0)
            return SendResult::error;
        if (av_bsf_send_packet(filter_.get(), scratch_) < 0)
        {
            av_packet_unref(scratch_);
            return count_refusal();
        }
        if (packet.pts != kNoTimestamp)
            pending_pts_.insert(packet.pts);
        // One packet in gives one access unit out for these filters; take all there is.
        while (av_bsf_receive_packet(filter_.get(), scratch_) == 0)
        {
            std::vector<HardwarePicture> pictures;
            const bool decoded = backend_->decode(scratch_->data,
                                                  static_cast<std::size_t>(scratch_->size),
                                                  pictures);
            av_packet_unref(scratch_);
            take(pictures);
            if (!decoded && count_refusal() == SendResult::error)
                return SendResult::error;
            if (decoded)
                refusals_ = 0;
        }
        return SendResult::accepted;
    }

    SendResult send_end() override
    {
        if (draining_)
            return SendResult::accepted;
        draining_ = true;
        std::vector<HardwarePicture> pictures;
        backend_->flush(pictures);
        take(pictures);
        return SendResult::accepted;
    }

    ReceiveResult receive(Frame &frame) override
    {
        if (ready_.empty())
            return draining_ ? ReceiveResult::end : ReceiveResult::need_input;
        HardwarePicture picture = std::move(ready_.front().first);
        const double pts = ready_.front().second;
        ready_.pop_front();
        frame = Frame{};
        frame.planes[0] = picture.luma;
        frame.planes[1] = picture.luma + static_cast<std::ptrdiff_t>(picture.pitch_bytes) *
                                             picture.coded_height;
        frame.pitches[0] = picture.pitch_bytes;
        frame.pitches[1] = picture.pitch_bytes;
        frame.plane_count = 2;
        frame.semi_planar = true;
        frame.width = stream_.width;
        frame.height = stream_.height;
        frame.bit_depth = stream_.bit_depth;
        frame.sample_bytes = picture.wide ? 2 : 1;
        frame.chroma_shift_x = 1;
        frame.chroma_shift_y = 1;
        frame.colour = colour_;
        frame.pts = pts;
        frame.owner = std::move(picture.owner);
        return ReceiveResult::frame;
    }

    void flush() override
    {
        backend_->reset();
        ready_.clear();
        pending_pts_.clear();
        draining_ = false;
        refusals_ = 0;
        // A flushed filter does not send the parameter sets again; a new one does.
        filter_ = make_filter();
        waiting_for_keyframe_ = true;
    }

    int damaged_packets() const override
    {
        return damaged_;
    }

  private:
    BsfPtr make_filter() const
    {
        if (parameters_ == nullptr)
            return nullptr;
        const AVBitStreamFilter *kind = av_bsf_get_by_name(
            stream_.codec == HardwareCodec::avc ? "h264_mp4toannexb" : "hevc_mp4toannexb");
        AVBSFContext *context = nullptr;
        if (kind == nullptr || av_bsf_alloc(kind, &context) < 0)
            return nullptr;
        BsfPtr filter(context);
        if (avcodec_parameters_copy(filter->par_in, parameters_) < 0 || av_bsf_init(filter.get()) < 0)
            return nullptr;
        return filter;
    }

    // Pictures come out in display order with no timestamp: each takes the earliest one sent
    // that no picture has taken yet (decode order differs from display order only by reordering).
    void take(std::vector<HardwarePicture> &pictures)
    {
        for (HardwarePicture &picture : pictures)
        {
            double pts = kNoTimestamp;
            if (!pending_pts_.empty())
            {
                pts = *pending_pts_.begin();
                pending_pts_.erase(pending_pts_.begin());
            }
            ready_.emplace_back(std::move(picture), pts);
        }
    }

    SendResult count_refusal()
    {
        ++damaged_;
        return ++refusals_ >= kMaxRefusals ? SendResult::error : SendResult::accepted;
    }

    std::unique_ptr<HardwareBackend> backend_;
    HardwareStream stream_;
    ColourInfo colour_;
    AVCodecParameters *parameters_;
    AVPacket *scratch_;
    BsfPtr filter_;
    std::deque<std::pair<HardwarePicture, double>> ready_;
    std::multiset<double> pending_pts_;
    bool waiting_for_keyframe_ = false;
    bool draining_ = false;
    int refusals_ = 0;
    int damaged_ = 0;
};

} // namespace

void set_hardware_provider(HardwareProvider value)
{
    provider() = std::move(value);
}

bool hardware_stream_for(const AVCodecParameters &parameters, HardwareStream &stream)
{
    if (parameters.codec_type != AVMEDIA_TYPE_VIDEO || parameters.width <= 0 ||
        parameters.height <= 0)
        return false;
    const auto format = static_cast<AVPixelFormat>(parameters.format);
    stream = HardwareStream{};
    stream.width = parameters.width;
    stream.height = parameters.height;
    if (parameters.codec_id == AV_CODEC_ID_H264)
    {
        // 8-bit 4:2:0 only: High 10 and the 4:2:2 and 4:4:4 profiles stay in software.
        const bool profile_ok = parameters.profile == AV_PROFILE_UNKNOWN ||
                                parameters.profile == AV_PROFILE_H264_BASELINE ||
                                parameters.profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
                                parameters.profile == AV_PROFILE_H264_MAIN ||
                                parameters.profile == AV_PROFILE_H264_HIGH;
        if (!profile_ok || (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P &&
                            format != AV_PIX_FMT_NONE))
            return false;
        if (stream.width > 1920 || stream.height > 1088)
            return false;
        stream.codec = HardwareCodec::avc;
        stream.bit_depth = 8;
        return true;
    }
    if (parameters.codec_id == AV_CODEC_ID_HEVC)
    {
        if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P)
            stream.bit_depth = 8;
        else if (format == AV_PIX_FMT_YUV420P10LE)
            stream.bit_depth = 10;
        else
            return false; // 4:2:2, 4:4:4, 12-bit: not Main or Main 10
        if (stream.width > 3840 || stream.height > 2176)
            return false;
        stream.codec = HardwareCodec::hevc;
        return true;
    }
    return false;
}

std::unique_ptr<VideoDecoder> open_hardware_decoder(const Demuxer &demuxer, int stream,
                                                    std::string *error)
{
    const auto fail = [error](const char *message) -> std::unique_ptr<VideoDecoder> {
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    if (!provider())
        return fail("There is no hardware decoder");
    const AVCodecParameters *parameters = demuxer.parameters(stream);
    HardwareStream wanted;
    if (parameters == nullptr || !hardware_stream_for(*parameters, wanted))
        return fail("The hardware decoder does not take this stream");
    std::unique_ptr<HardwareBackend> backend = provider()(wanted);
    if (!backend)
        return fail("The hardware decoder is not available for this stream");
    ColourInfo colour;
    for (const StreamInfo &info : demuxer.info().streams)
        if (info.index == stream)
            colour = info.colour;
    auto decoder = std::make_unique<HardwareDecoder>(std::move(backend), *parameters, wanted,
                                                     colour);
    if (!decoder->ready())
        return fail("The hardware decoder could not be set up");
    return decoder;
}

} // namespace hui::video
