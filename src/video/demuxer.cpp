// EliVier Explorer - Opens a media file and hands out its compressed packets in file order.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/demuxer.hpp"

#include "video/ffmpeg_colour.hpp"

#include <algorithm>
#include <cerrno>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

namespace hui::video
{

void AVPacketDeleter::operator()(AVPacket *packet) const
{
    av_packet_free(&packet);
}

namespace
{

// Limits for a file we did not write.
constexpr int kMaxStreams = 64;
constexpr std::int64_t kProbeBytes = 4 * 1024 * 1024;
constexpr std::int64_t kAnalyseMicroseconds = 3 * AV_TIME_BASE;
constexpr int kMaxPacketBytes = 32 * 1024 * 1024;
constexpr int kIoBufferBytes = 64 * 1024;

std::string describe(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

std::string dictionary_value(const AVDictionary *dictionary, const char *key)
{
    const AVDictionaryEntry *entry = av_dict_get(dictionary, key, nullptr, 0);
    return entry != nullptr && entry->value != nullptr ? entry->value : std::string();
}

int bit_depth_of(const AVCodecParameters &parameters)
{
    if (parameters.bits_per_raw_sample > 0)
        return parameters.bits_per_raw_sample;
    const AVPixFmtDescriptor *descriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters.format));
    return descriptor != nullptr ? descriptor->comp[0].depth : 0;
}

} // namespace

struct Demuxer::Impl
{
    audio::ByteReader reader;
    std::uint64_t size = 0;
    std::uint64_t position = 0;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    MediaInfo info;
    double start_offset = 0.0;
    bool ended = false;

    ~Impl()
    {
        // avformat_close_input frees the context; the custom IO buffer and context are ours.
        avformat_close_input(&format);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    static int read_callback(void *opaque, std::uint8_t *buffer, int size)
    {
        Impl *self = static_cast<Impl *>(opaque);
        if (size <= 0)
            return 0;
        const std::int64_t count =
            self->reader(self->position, buffer, static_cast<std::size_t>(size));
        if (count < 0)
            return static_cast<int>(std::max<std::int64_t>(count, -4095)); // an AVERROR(errno)
        if (count == 0)
            return AVERROR_EOF;
        self->position += static_cast<std::uint64_t>(count);
        return static_cast<int>(count);
    }

    static std::int64_t seek_callback(void *opaque, std::int64_t offset, int whence)
    {
        Impl *self = static_cast<Impl *>(opaque);
        if ((whence & AVSEEK_SIZE) != 0)
            return static_cast<std::int64_t>(self->size);
        std::int64_t target = 0;
        switch (whence & ~AVSEEK_FORCE)
        {
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = static_cast<std::int64_t>(self->position) + offset;
            break;
        case SEEK_END:
            target = static_cast<std::int64_t>(self->size) + offset;
            break;
        default:
            return AVERROR(EINVAL);
        }
        if (target < 0)
            return AVERROR(EINVAL);
        self->position = static_cast<std::uint64_t>(target);
        return target;
    }

    void collect_streams()
    {
        info.container = format->iformat != nullptr && format->iformat->name != nullptr
                             ? format->iformat->name
                             : "";
        info.duration = format->duration != AV_NOPTS_VALUE
                            ? static_cast<double>(format->duration) / AV_TIME_BASE
                            : 0.0;
        start_offset = format->start_time != AV_NOPTS_VALUE
                           ? static_cast<double>(format->start_time) / AV_TIME_BASE
                           : 0.0;
        for (unsigned int i = 0; i < format->nb_streams; ++i)
        {
            const AVStream &stream = *format->streams[i];
            const AVCodecParameters &parameters = *stream.codecpar;
            StreamInfo item;
            item.index = static_cast<int>(i);
            switch (parameters.codec_type)
            {
            case AVMEDIA_TYPE_VIDEO:
                item.kind = StreamKind::video;
                break;
            case AVMEDIA_TYPE_AUDIO:
                item.kind = StreamKind::audio;
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                item.kind = StreamKind::subtitle;
                break;
            default:
                item.kind = StreamKind::other;
                info.streams.push_back(item);
                continue;
            }
            item.codec = avcodec_get_name(parameters.codec_id);
            if (const char *profile = avcodec_profile_name(parameters.codec_id, parameters.profile))
                item.profile = profile;
            item.language = dictionary_value(stream.metadata, "language");
            item.title = dictionary_value(stream.metadata, "title");
            if (item.kind == StreamKind::video)
            {
                item.width = parameters.width;
                item.height = parameters.height;
                item.bit_depth = bit_depth_of(parameters);
                const AVRational rate = av_guess_frame_rate(format, format->streams[i], nullptr);
                item.frame_rate = rate.den > 0 ? av_q2d(rate) : 0.0;
                item.colour = colour_info(parameters.color_trc, parameters.color_space,
                                          parameters.color_primaries, parameters.color_range,
                                          parameters.height);
            }
            else if (item.kind == StreamKind::audio)
            {
                item.channels = parameters.ch_layout.nb_channels;
                item.sample_rate = parameters.sample_rate;
            }
            info.streams.push_back(item);
        }
    }
};

Demuxer::Demuxer(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

Demuxer::~Demuxer() = default;

std::unique_ptr<Demuxer> Demuxer::open(audio::ByteReader reader, std::uint64_t size,
                                       std::string *error)
{
    const auto fail = [error](const std::string &message) -> std::unique_ptr<Demuxer> {
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    if (!reader || size == 0)
        return fail("The file is empty");

    av_log_set_level(AV_LOG_QUIET); // reasons come back through `error`, not through stderr

    auto impl = std::make_unique<Impl>();
    impl->reader = std::move(reader);
    impl->size = size;
    unsigned char *buffer = static_cast<unsigned char *>(av_malloc(kIoBufferBytes));
    if (buffer == nullptr)
        return fail("Out of memory");
    impl->io = avio_alloc_context(buffer, kIoBufferBytes, 0, impl.get(), &Impl::read_callback,
                                  nullptr, &Impl::seek_callback);
    if (impl->io == nullptr)
    {
        av_free(buffer);
        return fail("Out of memory");
    }
    impl->format = avformat_alloc_context();
    if (impl->format == nullptr)
        return fail("Out of memory");
    impl->format->pb = impl->io;
    impl->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    impl->format->max_streams = kMaxStreams;
    impl->format->probesize = kProbeBytes;
    impl->format->max_analyze_duration = kAnalyseMicroseconds;

    int result = avformat_open_input(&impl->format, "", nullptr, nullptr);
    if (result < 0)
        return fail("Not a video this player can open (" + describe(result) + ")");
    result = avformat_find_stream_info(impl->format, nullptr);
    if (result < 0)
        return fail("Cannot read the streams of this file (" + describe(result) + ")");
    impl->collect_streams();
    const bool playable =
        std::any_of(impl->info.streams.begin(), impl->info.streams.end(), [](const StreamInfo &s) {
            return s.kind == StreamKind::video || s.kind == StreamKind::audio;
        });
    if (!playable)
        return fail("The file has no picture or sound this player can play");
    return std::unique_ptr<Demuxer>(new Demuxer(std::move(impl)));
}

const MediaInfo &Demuxer::info() const
{
    return impl_->info;
}

ReadResult Demuxer::read(Packet &packet)
{
    Impl &self = *impl_;
    if (self.ended)
        return ReadResult::end;
    PacketPtr data(av_packet_alloc());
    if (!data)
        return ReadResult::error;
    const int result = av_read_frame(self.format, data.get());
    if (result == AVERROR_EOF)
    {
        self.ended = true;
        return ReadResult::end;
    }
    if (result < 0)
        return ReadResult::error;
    if (data->size > kMaxPacketBytes || data->stream_index < 0 ||
        static_cast<std::size_t>(data->stream_index) >= self.info.streams.size())
        return ReadResult::error;

    const AVStream &stream = *self.format->streams[data->stream_index];
    const double seconds_per_tick = av_q2d(stream.time_base);
    const std::int64_t stamp = data->pts != AV_NOPTS_VALUE ? data->pts : data->dts;
    packet.pts =
        stamp != AV_NOPTS_VALUE ? static_cast<double>(stamp) * seconds_per_tick - self.start_offset
                                : kNoTimestamp;
    packet.duration = static_cast<double>(data->duration) * seconds_per_tick;
    packet.keyframe = (data->flags & AV_PKT_FLAG_KEY) != 0;
    packet.stream = data->stream_index;
    packet.data = std::move(data);
    return ReadResult::packet;
}

bool Demuxer::seek(double seconds)
{
    Impl &self = *impl_;
    const double target = std::max(seconds, 0.0) + self.start_offset;
    const int result = av_seek_frame(
        self.format, -1, static_cast<std::int64_t>(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    if (result < 0)
        return false;
    self.ended = false;
    return true;
}

const AVCodecParameters *Demuxer::parameters(int stream) const
{
    if (stream < 0 || static_cast<unsigned int>(stream) >= impl_->format->nb_streams)
        return nullptr;
    return impl_->format->streams[stream]->codecpar;
}

StreamTiming Demuxer::timing(int stream) const
{
    StreamTiming timing;
    if (stream >= 0 && static_cast<unsigned int>(stream) < impl_->format->nb_streams)
        timing.seconds_per_tick = av_q2d(impl_->format->streams[stream]->time_base);
    timing.start_offset = impl_->start_offset;
    return timing;
}

} // namespace hui::video
