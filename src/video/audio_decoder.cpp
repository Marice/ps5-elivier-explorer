// EliVier Explorer - Audio decoding for video files: any codec in, stereo or 7.1 float at 48 kHz out.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/audio_decoder.hpp"

#include <cerrno>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace hui::video
{

namespace
{

constexpr int kMaxChannels = 8;
constexpr int kMinRate = 8000;
constexpr int kMaxRate = 192000;
constexpr int kMaxConsecutiveDamage = 64;

std::string error_text(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

class FfmpegAudioDecoder final : public AudioDecoder
{
  public:
    FfmpegAudioDecoder(AVCodecContext *context, std::string name, StreamTiming timing, int channels)
        : context_(context), name_(std::move(name)), timing_(timing), channels_(channels),
          scratch_(av_frame_alloc())
    {
    }

    ~FfmpegAudioDecoder() override
    {
        swr_free(&resampler_);
        av_channel_layout_uninit(&input_layout_);
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

    ReceiveResult receive(AudioChunk &chunk) override
    {
        if (scratch_ == nullptr)
            return ReceiveResult::error;
        // A frame that resamples to nothing (the filter is still filling) is skipped, so one call
        // returns sound or a reason why not.
        for (int attempt = 0; attempt < kMaxConsecutiveDamage; ++attempt)
        {
            const int result = avcodec_receive_frame(context_, scratch_);
            if (result == AVERROR(EAGAIN))
                return ReceiveResult::need_input;
            if (result == AVERROR_EOF)
                return finish(chunk);
            if (result == AVERROR_INVALIDDATA)
            {
                ++damaged_;
                continue;
            }
            if (result < 0)
                return ReceiveResult::error;
            const int converted = convert(chunk);
            if (converted < 0)
                return ReceiveResult::error;
            if (converted > 0)
                return ReceiveResult::frame;
        }
        return ReceiveResult::need_input;
    }

    void flush() override
    {
        avcodec_flush_buffers(context_);
        swr_free(&resampler_); // its history belongs to the old position; it is rebuilt on demand
        draining_ = false;
        tail_done_ = false;
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

    // The decoder has no more frames: what the resampler still holds comes out once.
    ReceiveResult finish(AudioChunk &chunk)
    {
        if (tail_done_ || resampler_ == nullptr)
            return ReceiveResult::end;
        tail_done_ = true;
        const int room = swr_get_out_samples(resampler_, 0);
        if (room <= 0)
            return ReceiveResult::end;
        chunk.channels = channels_;
        chunk.samples.assign(static_cast<std::size_t>(room) * channels_, 0.0f);
        std::uint8_t *out = reinterpret_cast<std::uint8_t *>(chunk.samples.data());
        const int count = swr_convert(resampler_, &out, room, nullptr, 0);
        if (count <= 0)
            return ReceiveResult::end;
        chunk.samples.resize(static_cast<std::size_t>(count) * channels_);
        reorder(chunk);
        limit(chunk);
        chunk.pts = kNoTimestamp;
        return ReceiveResult::frame;
    }

    // (Re)builds the resampler when the decoder's output differs from what it was made for.
    bool prepare_resampler(const AVFrame &frame)
    {
        if (frame.nb_samples <= 0 || frame.sample_rate < kMinRate || frame.sample_rate > kMaxRate ||
            frame.ch_layout.nb_channels < 1 || frame.ch_layout.nb_channels > kMaxChannels)
            return false;
        AVChannelLayout layout = {};
        if (av_channel_layout_copy(&layout, &frame.ch_layout) < 0)
            return false;
        if (layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&layout, frame.ch_layout.nb_channels);
        // On eight channels a 5.1 film's surrounds belong on the surround speakers of a 5.1 set,
        // whether the file calls them side or back (both players put them there, D65).
        const AVChannelLayout back = AV_CHANNEL_LAYOUT_5POINT1_BACK;
        if (channels_ == kSurroundChannels && av_channel_layout_compare(&layout, &back) == 0)
        {
            av_channel_layout_uninit(&layout);
            layout = AV_CHANNEL_LAYOUT_5POINT1;
        }

        const bool same = resampler_ != nullptr && input_format_ == frame.format &&
                          input_rate_ == frame.sample_rate &&
                          av_channel_layout_compare(&input_layout_, &layout) == 0;
        if (same)
        {
            av_channel_layout_uninit(&layout);
            return true;
        }

        swr_free(&resampler_);
        // Eight channels come out in FFmpeg's 7.1 order (FL FR FC LFE BL BR SL SR) and are put in
        // the console's order afterwards (reorder()).
        AVChannelLayout output = channels_ == kSurroundChannels
                                     ? AVChannelLayout(AV_CHANNEL_LAYOUT_7POINT1)
                                     : AVChannelLayout(AV_CHANNEL_LAYOUT_STEREO);
        int result = swr_alloc_set_opts2(&resampler_, &output, AV_SAMPLE_FMT_FLT, kAudioOutputRate,
                                         &layout, static_cast<AVSampleFormat>(frame.format),
                                         frame.sample_rate, 0, nullptr);
        if (result >= 0 && channels_ == kSurroundChannels)
        {
            // Nothing is mixed down: the channels pass at their own level, and a missing one is
            // silent. Mono goes to the centre, as FFmpeg places it.
            mixing_down_ = layout.nb_channels > kSurroundChannels;
            av_opt_set_double(resampler_, "rematrix_maxval", 8.0, 0);
            result = swr_init(resampler_);
        }
        else if (result >= 0)
        {
            // A mix-down at the usual weights (centre and surrounds at -3 dB), not scaled so that
            // the loudest possible sum fits: that made 5.1 films 7.7 dB quieter than stereo ones
            // (docs/decision-log.md, D56). The rare peak above full scale goes through
            // limit_sample(). Stereo and mono are not touched by this.
            mixing_down_ = layout.nb_channels > 2;
            av_opt_set_double(resampler_, "rematrix_maxval", mixing_down_ ? 8.0 : 1.0, 0);
            if (layout.nb_channels == 1)
            {
                // FFmpeg's own mono to stereo puts the signal on both sides at -3 dB, which makes
                // a mono film 3 dB quieter than a stereo one. Both sides at full level instead.
                const double mono_to_stereo[2] = {1.0, 1.0};
                result = swr_set_matrix(resampler_, mono_to_stereo, 1);
            }
            if (result >= 0)
                result = swr_init(resampler_);
        }
        if (result < 0)
        {
            swr_free(&resampler_);
            av_channel_layout_uninit(&layout);
            return false;
        }
        av_channel_layout_uninit(&input_layout_);
        input_layout_ = layout; // ownership moves
        input_format_ = frame.format;
        input_rate_ = frame.sample_rate;
        return true;
    }

    // Returns the frames in `chunk`, 0 when the resampler gave nothing yet, -1 on an error.
    int convert(AudioChunk &chunk)
    {
        if (!prepare_resampler(*scratch_))
        {
            av_frame_unref(scratch_);
            return -1;
        }
        const int room = swr_get_out_samples(resampler_, scratch_->nb_samples);
        if (room < 0)
        {
            av_frame_unref(scratch_);
            return -1;
        }
        chunk.channels = channels_;
        chunk.samples.assign(static_cast<std::size_t>(room) * channels_, 0.0f);
        std::uint8_t *out = reinterpret_cast<std::uint8_t *>(chunk.samples.data());
        const int count =
            swr_convert(resampler_, &out, room,
                        const_cast<const std::uint8_t **>(scratch_->extended_data),
                        scratch_->nb_samples);
        const std::int64_t stamp = scratch_->best_effort_timestamp != AV_NOPTS_VALUE
                                       ? scratch_->best_effort_timestamp
                                       : scratch_->pts;
        chunk.pts = kNoTimestamp;
        if (stamp != AV_NOPTS_VALUE)
            chunk.pts =
                static_cast<double>(stamp) * timing_.seconds_per_tick - timing_.start_offset;
        av_frame_unref(scratch_);
        if (count < 0)
            return -1;
        chunk.samples.resize(static_cast<std::size_t>(count) * channels_);
        reorder(chunk);
        limit(chunk);
        return count;
    }

    // FFmpeg's 7.1 order to the console's: the side pair (FFmpeg's 6 and 7) before the back pair
    // (FFmpeg's 4 and 5).
    void reorder(AudioChunk &chunk) const
    {
        if (channels_ != kSurroundChannels)
            return;
        for (std::size_t i = 0; i + 8 <= chunk.samples.size(); i += 8)
        {
            float *frame = chunk.samples.data() + i;
            std::swap(frame[4], frame[6]);
            std::swap(frame[5], frame[7]);
        }
    }

    // Only a mix-down can go over full scale (its matrix is not scaled down, D56).
    void limit(AudioChunk &chunk) const
    {
        if (!mixing_down_)
            return;
        for (float &sample : chunk.samples)
            sample = limit_sample(sample);
    }

    AVCodecContext *context_;
    std::string name_;
    StreamTiming timing_;
    int channels_ = 2; // 2 or kSurroundChannels
    AVFrame *scratch_;
    SwrContext *resampler_ = nullptr;
    bool mixing_down_ = false;
    int input_format_ = -1;
    int input_rate_ = 0;
    AVChannelLayout input_layout_ = {};
    bool draining_ = false;
    bool tail_done_ = false;
    int damaged_ = 0;
};

} // namespace

float limit_sample(float sample)
{
    constexpr float kKnee = 0.8f;
    const float size = sample < 0.0f ? -sample : sample;
    if (size <= kKnee)
        return sample;
    const float over = (size - kKnee) / (1.0f - kKnee);
    const float bent = kKnee + (1.0f - kKnee) * over / (1.0f + over);
    return sample < 0.0f ? -bent : bent;
}

std::unique_ptr<AudioDecoder> open_audio_decoder(const Demuxer &demuxer, int stream,
                                                 std::string *error, int channels)
{
    const auto fail = [error](const std::string &message) -> std::unique_ptr<AudioDecoder> {
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    if (channels != 2 && channels != kSurroundChannels)
        return fail("Sound comes out as stereo or as eight channels");
    const AVCodecParameters *parameters = demuxer.parameters(stream);
    if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_AUDIO)
        return fail("That is not an audio stream");
    if (parameters->ch_layout.nb_channels > kMaxChannels)
        return fail("More than eight audio channels are not supported");
    if (parameters->sample_rate != 0 &&
        (parameters->sample_rate < kMinRate || parameters->sample_rate > kMaxRate))
        return fail("The sample rate of this audio is not supported");

    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
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
        return fail("Cannot set up the audio decoder (" + error_text(result) + ")");
    }
    context->thread_count = 1; // a frame of sound is cheap; threads would only add delay
    result = avcodec_open2(context, codec, nullptr);
    if (result < 0)
    {
        avcodec_free_context(&context);
        return fail("Cannot start the audio decoder (" + error_text(result) + ")");
    }
    return std::make_unique<FfmpegAudioDecoder>(context, codec->name, demuxer.timing(stream),
                                                channels);
}

} // namespace hui::video
