// EliVier Explorer - The playback engine: one file in, pictures, sound and subtitles out in time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/playback_engine.hpp"

#include "explorer/trace.hpp"


#include "video/audio_decoder.hpp"
#include "video/av_sync.hpp"
#include "video/convert_pool.hpp"
#include "video/hardware_decoder.hpp"
#include "video/subtitle_decoder.hpp"
#include "video/video_decoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <pthread.h>

extern "C"
{
#include <libavcodec/packet.h>
}

namespace hui::video
{

namespace
{

constexpr int kKeep = -2; // a track request that was not made
// Compressed packets read ahead of the decoders, in bytes. Reading must not wait for the picture
// queue: a file keeps its sound a second or more behind its pictures, and the clock is the sound.

// A request to leave a track as it is is kKeep; -1 is "none".
bool is_requested(int stream)
{
    return stream != kKeep;
}

} // namespace

struct PlaybackEngine::Impl
{
    EngineOptions options;
    std::unique_ptr<Demuxer> demuxer;
    MediaInfo info;

    // ---- the engine thread's own (pump() is the only user) ----
    std::unique_ptr<VideoDecoder> video;
    std::unique_ptr<AudioDecoder> audio;
    std::unique_ptr<SubtitleDecoder> subtitle;
    ConvertTables tables;
    std::unique_ptr<ConvertPool> converter; // made at open, with options.convert_threads
    std::unique_ptr<VideoPicture> held_picture; // converted, waiting for room in the queue
    int picture_limit = 0; // pictures the engine may hold in all, from the budget; 0 = not known yet
    AudioChunk held_chunk;
    bool has_held_chunk = false;
    std::deque<Packet> video_packets; // read, waiting for the decoder
    std::deque<Packet> audio_packets;
    std::size_t buffered_bytes = 0;
    double drop_video_before = -1.0;
    double drop_audio_before = -1.0;
    bool demux_ended = false;
    bool video_ended = false;
    bool audio_ended = false;
    bool sent_end = false;

    // ---- shared with the game thread ----
    mutable std::mutex mutex;
    std::deque<std::unique_ptr<VideoPicture>> pictures;
    std::vector<std::unique_ptr<VideoPicture>> spare;
    std::deque<AudioChunk> sound;
    std::size_t sound_front_offset = 0; // frames of the front chunk already read
    double sound_seconds = 0.0;
    SubtitleTrack track;
    bool seek_requested = false;
    double seek_target = 0.0;
    int next_audio = kKeep;
    int next_subtitle = kKeep;
    bool awaiting_audio = false; // a seek happened and no sound has arrived since
    bool audio_done = true;      // the sound decoder has nothing more (or there is none)
    bool finished = false;       // everything is decoded (not necessarily shown or heard)
    std::string video_error;
    std::string audio_error;
    int video_index = -1;
    int audio_index = -1;
    int subtitle_index = -1;
    Stats counters;
    bool source_taken = false;

    // ---- the conversion stage (D61) ----
    // While the threads run, a decoded picture goes to a thread of its own for its conversion, so
    // that the next one is decoded meanwhile: the hardware decoder is a synchronous call and a 4K
    // conversion takes about 12 ms, which in turn left a 4K60 film at about 47 pictures a second.
    // With pump() alone (the tests) the conversion happens in line, as before.
    struct DecodedFrame
    {
        Frame frame;
        unsigned generation = 0;
    };
    std::deque<DecodedFrame> decoded; // under `mutex`: at most one waits
    bool converting = false;          // under `mutex`
    bool unconvertible = false;       // under `mutex`: a picture the converter refused
    unsigned video_generation = 0;    // under `mutex`: one more at every seek
    bool convert_stopping = false;    // under `mutex`
    std::condition_variable convert_wake;
    pthread_t convert_thread{};
    std::atomic<bool> convert_running{false};
    std::atomic<bool> hdr_passthrough{false}; // HDR10 output on: PQ pictures keep their signal

    // ---- the thread ----
    pthread_t thread{};
    bool running = false;
    std::atomic<bool> stopping{false};
    std::mutex sleep_mutex;
    std::condition_variable wake;

    // ---- helpers ----
    void notify()
    {
        wake.notify_all();
    }

    bool wants_pictures() const
    {
        return video != nullptr;
    }

    bool wants_sound() const
    {
        return audio != nullptr;
    }

    std::unique_ptr<VideoPicture> new_picture()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!spare.empty())
        {
            auto picture = std::move(spare.back());
            spare.pop_back();
            return picture;
        }
        return std::make_unique<VideoPicture>();
    }

    // How many pictures may wait in the queue: one less than the limit leaves room for the one
    // that is held, and never fewer than one.
    int queue_cap_locked() const
    {
        if (picture_limit <= 0)
            return options.picture_queue;
        return std::max(1, std::min(options.picture_queue, picture_limit - 1));
    }

    int owned_locked() const
    {
        return static_cast<int>(pictures.size() + spare.size()) + (held_picture ? 1 : 0);
    }

    // A converted picture waits for room in the queue (the conversion stage may set it).
    bool holding_picture() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return held_picture != nullptr;
    }

    void recycle_locked(std::unique_ptr<VideoPicture> picture)
    {
        if (picture && spare.size() < 4 &&
            (picture_limit <= 0 || owned_locked() + 1 <= picture_limit))
            spare.push_back(std::move(picture));
    }

    void clear_queues_locked()
    {
        for (auto &picture : pictures)
            recycle_locked(std::move(picture));
        pictures.clear();
        // Decoded but not converted yet: from before the request too. A conversion in flight is
        // dropped when it finishes, by its generation.
        decoded.clear();
        ++video_generation;
        sound.clear();
        sound_front_offset = 0;
        sound_seconds = 0.0;
    }

    // Moves what could not be queued before into the queues, when there is room.
    bool flush_held()
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool moved = false;
        if (held_picture && static_cast<int>(pictures.size()) < queue_cap_locked())
        {
            pictures.push_back(std::move(held_picture));
            moved = true;
            convert_wake.notify_one(); // the conversion stage waits while a picture is held
        }
        if (has_held_chunk && sound_seconds < options.audio_queue_seconds)
        {
            sound_seconds += static_cast<double>(held_chunk.frames()) / kAudioOutputRate;
            sound.push_back(std::move(held_chunk));
            held_chunk = AudioChunk{};
            has_held_chunk = false;
            awaiting_audio = false;
            moved = true;
        }
        return moved;
    }

    bool apply_requests()
    {
        bool seek = false;
        double target = 0.0;
        int want_audio = kKeep;
        int want_subtitle = kKeep;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!seek_requested)
                return false;
            seek = true;
            target = seek_target;
            want_audio = next_audio;
            want_subtitle = next_subtitle;
            seek_requested = false;
            next_audio = kKeep;
            next_subtitle = kKeep;
        }
        if (!seek)
            return false;
        std::string error;
        if (is_requested(want_audio))
        {
            audio.reset();
            std::string reason;
            if (want_audio >= 0)
                audio = open_audio_decoder(*demuxer, want_audio, &reason, options.sound_channels);
            std::lock_guard<std::mutex> lock(mutex);
            audio_index = audio ? want_audio : -1;
            audio_error = want_audio >= 0 && !audio ? reason : std::string();
        }
        if (is_requested(want_subtitle))
        {
            subtitle.reset();
            std::string reason;
            if (want_subtitle >= 0)
                subtitle = open_subtitle_decoder(*demuxer, want_subtitle, &reason);
            std::lock_guard<std::mutex> lock(mutex);
            subtitle_index = subtitle ? want_subtitle : -1;
            track.clear();
        }
        const bool sought = demuxer->seek(target);
        if (video)
            video->flush();
        if (audio)
            audio->flush();
        if (subtitle)
            subtitle->flush();
        {
            std::lock_guard<std::mutex> lock(mutex); // stats() counts it from another thread
            recycle_locked(std::move(held_picture));
            held_picture.reset();
        }
        held_chunk = AudioChunk{};
        has_held_chunk = false;
        video_packets.clear();
        audio_packets.clear();
        buffered_bytes = 0;
        demux_ended = false;
        video_ended = video == nullptr;
        audio_ended = audio == nullptr;
        sent_end = false;
        drop_video_before = sought ? target : -1.0;
        drop_audio_before = sought ? target : -1.0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = false;
            audio_done = audio == nullptr;
            // Whatever was queued while the request was made is from before it.
            clear_queues_locked();
            awaiting_audio = audio != nullptr;
        }
        return true;
    }

    bool drain_video()
    {
        if (!video || video_ended)
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (unconvertible)
            {
                video.reset();
                video_ended = true;
                return true;
            }
            if (!decoded.empty())
                return false; // the conversion stage has one waiting already
        }
        Frame frame;
        const ReceiveResult result = video->receive(frame);
        if (result == ReceiveResult::end)
        {
            video_ended = true;
            return true;
        }
        if (result == ReceiveResult::error)
        {
            std::lock_guard<std::mutex> lock(mutex);
            video_error = "The picture could not be decoded";
            video.reset();
            video_ended = true;
            return true;
        }
        if (result != ReceiveResult::frame)
            return false;
        // After a seek the decoder runs forward from a keyframe: what is before the target is
        // decoded (it has to be) but never converted or shown.
        if (frame.pts != kNoTimestamp && frame.pts < drop_video_before - 1e-3)
            return true;
        if (convert_running.load())
        {
            std::lock_guard<std::mutex> lock(mutex);
            decoded.push_back({std::move(frame), video_generation});
            convert_wake.notify_one();
            return true;
        }
        present(frame, 0, false);
        return true;
    }

    // Converts a decoded picture and queues it. On the conversion stage `generation` says which
    // seek the picture belongs to; one from before the latest seek is dropped.
    void present(const Frame &frame, unsigned generation, bool check_generation)
    {
        const std::size_t picture_bytes =
            static_cast<std::size_t>(frame.width) * frame.height * 4 * sizeof(std::uint16_t);
        {
            std::lock_guard<std::mutex> lock(mutex);
            const std::size_t fit = options.picture_budget_bytes / std::max<std::size_t>(picture_bytes, 1);
            picture_limit = static_cast<int>(
                std::clamp<std::size_t>(fit, 2, static_cast<std::size_t>(options.picture_queue) + 5));
        }
        tables.pq_passthrough = hdr_passthrough.load();
        std::unique_ptr<VideoPicture> picture = new_picture();
        picture->width = frame.width;
        picture->height = frame.height;
        picture->pts = frame.pts;
        picture->rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
        if (!converter->convert_rgba16(frame, tables, picture->rgba.data(),
                               static_cast<std::size_t>(frame.width) * 8))
        {
            std::lock_guard<std::mutex> lock(mutex);
            video_error = "This pixel format cannot be shown yet";
            unconvertible = true; // the engine thread ends the picture (it owns the decoder)
            recycle_locked(std::move(picture));
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (check_generation && generation != video_generation)
            recycle_locked(std::move(picture));
        else if (static_cast<int>(pictures.size()) < queue_cap_locked())
            pictures.push_back(std::move(picture));
        else
            held_picture = std::move(picture);
    }

    // The conversion stage: one decoded picture at a time, never while a converted one is held.
    void convert_loop()
    {
        for (;;)
        {
            DecodedFrame item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                convert_wake.wait(lock, [&] {
                    return convert_stopping || (!decoded.empty() && !held_picture);
                });
                if (convert_stopping)
                    return;
                item = std::move(decoded.front());
                decoded.pop_front();
                converting = true;
            }
            present(item.frame, item.generation, true);
            item.frame = Frame{}; // gives the decoder's buffer back before the next is taken
            {
                std::lock_guard<std::mutex> lock(mutex);
                converting = false;
            }
            notify();
        }
    }

    static void *convert_main(void *self)
    {
        static_cast<Impl *>(self)->convert_loop();
        return nullptr;
    }

    bool drain_audio()
    {
        if (!audio || audio_ended)
            return false;
        AudioChunk chunk;
        const ReceiveResult result = audio->receive(chunk);
        if (result == ReceiveResult::end)
        {
            audio_ended = true;
            std::lock_guard<std::mutex> lock(mutex);
            audio_done = true;
            return true;
        }
        if (result == ReceiveResult::error)
        {
            std::lock_guard<std::mutex> lock(mutex);
            audio_error = "The sound could not be decoded";
            audio.reset();
            audio_ended = true;
            audio_done = true;
            return true;
        }
        if (result != ReceiveResult::frame)
            return false;
        // The codec starts a little before the wanted time (its run-up): cut that part off, so
        // that the first sample the player gets is the one at the target.
        if (drop_audio_before >= 0.0 && chunk.pts != kNoTimestamp)
        {
            const double length = static_cast<double>(chunk.frames()) / kAudioOutputRate;
            if (chunk.pts + length <= drop_audio_before)
                return true;
            if (chunk.pts < drop_audio_before)
            {
                const auto cut = static_cast<std::size_t>((drop_audio_before - chunk.pts) *
                                                          kAudioOutputRate);
                const std::size_t values =
                    std::min(cut * static_cast<std::size_t>(chunk.channels), chunk.samples.size());
                chunk.samples.erase(chunk.samples.begin(),
                                    chunk.samples.begin() + static_cast<std::ptrdiff_t>(values));
                chunk.pts = drop_audio_before;
            }
        }
        if (chunk.frames() == 0)
            return true;
        drop_audio_before = -1.0; // the first chunk of this run is in
        std::lock_guard<std::mutex> lock(mutex);
        if (sound_seconds < options.audio_queue_seconds)
        {
            sound_seconds += static_cast<double>(chunk.frames()) / kAudioOutputRate;
            sound.push_back(std::move(chunk));
            awaiting_audio = false;
        }
        else
        {
            held_chunk = std::move(chunk);
            has_held_chunk = true;
        }
        return true;
    }

    // The indices are changed only by this thread (in apply_requests); reading them here needs no
    // lock, and the game thread reads them under the lock.
    int video_index_unlocked() const
    {
        return video_index;
    }
    int audio_index_unlocked() const
    {
        return audio_index;
    }
    int subtitle_index_unlocked() const
    {
        return subtitle_index;
    }

    static std::size_t size_of(const Packet &packet)
    {
        return packet.data ? static_cast<std::size_t>(std::max(packet.data->size, 0)) : 0;
    }

    // Reads one packet and sorts it: subtitles are decoded at once, pictures and sound wait in
    // their own queue for their decoder.
    bool read_packet()
    {
        Packet packet;
        const ReadResult result = demuxer->read(packet);
        if (result != ReadResult::packet)
        {
            demux_ended = true; // the end of the file, or a read error: the same for the viewer
            return true;
        }
        if (packet.stream == video_index_unlocked() && video)
        {
            buffered_bytes += size_of(packet);
            video_packets.push_back(std::move(packet));
        }
        else if (packet.stream == audio_index_unlocked() && audio)
        {
            buffered_bytes += size_of(packet);
            audio_packets.push_back(std::move(packet));
        }
        else if (packet.stream == subtitle_index_unlocked() && subtitle)
        {
            std::lock_guard<std::mutex> lock(mutex);
            subtitle->decode(packet, track);
        }
        return true;
    }

    // Gives the next packet of a queue to its decoder. False when there is none or the decoder is
    // full (it has to be drained first).
    bool feed(std::deque<Packet> &packets, bool is_video)
    {
        if (packets.empty())
            return false;
        const SendResult sent =
            is_video ? video->send(packets.front()) : audio->send(packets.front());
        if (sent == SendResult::full)
            return false;
        if (sent == SendResult::error)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (is_video)
            {
                video_error = "The picture could not be decoded";
                video.reset();
                video_ended = true;
            }
            else
            {
                audio_error = "The sound could not be decoded";
                audio.reset();
                audio_ended = true;
                audio_done = true;
            }
            for (const Packet &packet : packets)
                buffered_bytes -= std::min(buffered_bytes, size_of(packet));
            packets.clear();
            return true;
        }
        buffered_bytes -= std::min(buffered_bytes, size_of(packets.front()));
        packets.pop_front();
        return true;
    }

    void finish_if_done()
    {
        if (demux_ended && sent_end && video_ended && audio_ended && !has_held_chunk)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!held_picture && decoded.empty() && !converting)
                finished = true;
        }
    }

    bool pump()
    {
        bool worked = apply_requests();
        if (flush_held())
            worked = true;
        // Each side waits only for its own queue: a full picture queue must not keep the sound
        // from being read, or the clock (the sound) never moves and the pictures never become due.
        const bool holding = holding_picture();
        if (!holding && drain_video())
            return true;
        if (!has_held_chunk && drain_audio())
            return true;
        if (!holding && feed(video_packets, true))
            return true;
        if (!has_held_chunk && feed(audio_packets, false))
            return true;
        if (!demux_ended)
        {
            if (buffered_bytes < options.max_buffered_bytes)
                return read_packet();
            return worked;
        }
        if (!video_packets.empty() || !audio_packets.empty())
            return worked; // the decoders still have packets to take
        if (!sent_end)
        {
            if (video)
                video->send_end();
            if (audio)
                audio->send_end();
            sent_end = true;
            return true;
        }
        finish_if_done();
        return worked;
    }

    // Takes up to `frames` frames of sound from the queue, interleaved as the decoder made them
    // (options.sound_channels a frame). Fewer at the end; audio::Decoder::kTryLater when nothing
    // is queued yet (a seek being served, the decoder behind): the caller writes nothing and its
    // clock waits.
    int read_sound(float *out, int frames)
    {
        std::lock_guard<std::mutex> lock(mutex);
        int written = 0;
        while (written < frames && !sound.empty())
        {
            const AudioChunk &chunk = sound.front();
            const auto channels = static_cast<std::size_t>(chunk.channels);
            const std::size_t available =
                static_cast<std::size_t>(chunk.frames()) - sound_front_offset;
            const std::size_t count =
                std::min<std::size_t>(available, static_cast<std::size_t>(frames - written));
            std::memcpy(out + static_cast<std::size_t>(written) * channels,
                        chunk.samples.data() + sound_front_offset * channels,
                        count * channels * sizeof(float));
            written += static_cast<int>(count);
            sound_front_offset += count;
            sound_seconds -= static_cast<double>(count) / kAudioOutputRate;
            if (sound_front_offset >= static_cast<std::size_t>(chunk.frames()))
            {
                sound.pop_front();
                sound_front_offset = 0;
            }
        }
        if (sound_seconds < 0.0)
            sound_seconds = 0.0;
        if (written < frames)
        {
            // Nothing (more) queued. At the end that is the end. Otherwise frames already copied
            // are handed over, and with none the reader is told to ask again.
            const bool at_end = audio_done && !awaiting_audio;
            if (at_end || audio_index < 0 || written > 0)
                return written;
            ++counters.starved_reads;
            return audio::Decoder::kTryLater;
        }
        return written;
    }

    static void *thread_main(void *self)
    {
        static_cast<Impl *>(self)->run();
        return nullptr;
    }

    void run()
    {
        while (!stopping.load())
        {
            if (!pump())
            {
                std::unique_lock<std::mutex> lock(sleep_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(3));
            }
        }
    }
};

namespace
{

// The sound as audio::AudioPlayer wants it (a pull decoder), fed from the engine's queue.
class EngineAudioSource final : public audio::Decoder
{
  public:
    explicit EngineAudioSource(PlaybackEngine::Impl &impl, PlaybackEngine &engine)
        : impl_(impl), engine_(engine)
    {
    }

    ~EngineAudioSource() override
    {
        std::lock_guard<std::mutex> lock(impl_.mutex);
        impl_.source_taken = false;
    }

    int rate() const override
    {
        return kAudioOutputRate;
    }

    double duration() const override
    {
        return impl_.info.duration;
    }

    int read(float *stereo, int frames) override
    {
        const int written = impl_.read_sound(stereo, frames);
        return written;
    }

    bool seek(double seconds) override
    {
        engine_.seek(seconds);
        return true;
    }

    const std::string &title() const override
    {
        return empty_;
    }
    const std::string &artist() const override
    {
        return empty_;
    }
    const std::string &description() const override
    {
        return empty_;
    }
    const audio::CoverArt &cover() const override
    {
        return cover_;
    }

  private:
    PlaybackEngine::Impl &impl_;
    PlaybackEngine &engine_;
    std::string empty_;
    audio::CoverArt cover_;
};

} // namespace

PlaybackEngine::PlaybackEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

PlaybackEngine::~PlaybackEngine()
{
    stop();
}

std::unique_ptr<PlaybackEngine> PlaybackEngine::open(audio::ByteReader reader, std::uint64_t size,
                                                     const EngineOptions &options,
                                                     std::string *error)
{
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->converter = std::make_unique<ConvertPool>(options.convert_threads);
    std::string reason;
    impl->demuxer = Demuxer::open(std::move(reader), size, &reason);
    if (!impl->demuxer)
    {
        if (error != nullptr)
            *error = reason;
        return nullptr;
    }
    impl->info = impl->demuxer->info();
    for (const StreamInfo &stream : impl->info.streams)
    {
        if (stream.kind == StreamKind::video && impl->video_index < 0)
        {
            // The hardware first, when the platform lent one and the stream suits it; software
            // otherwise (docs/decision-log.md, D58).
            std::string hardware_error;
            impl->video = open_hardware_decoder(*impl->demuxer, stream.index, &hardware_error);
            if (!impl->video)
                impl->video = open_software_decoder(*impl->demuxer, stream.index, options.limits,
                                                    &impl->video_error);
            if (impl->video)
                hui::explorer::trace("film: video decoder %s", impl->video->name().c_str());
            if (impl->video)
                impl->video_index = stream.index;
        }
        else if (stream.kind == StreamKind::audio && impl->audio_index < 0)
        {
            impl->audio = open_audio_decoder(*impl->demuxer, stream.index, &impl->audio_error,
                                             options.sound_channels);
            if (impl->audio)
                impl->audio_index = stream.index;
        }
    }
    if (!impl->video && !impl->audio)
    {
        if (error != nullptr)
        {
            *error = !impl->video_error.empty() ? impl->video_error
                     : !impl->audio_error.empty() ? impl->audio_error
                                                  : "There is nothing in this file to play";
        }
        return nullptr;
    }
    impl->video_ended = impl->video == nullptr;
    impl->audio_ended = impl->audio == nullptr;
    impl->audio_done = impl->audio == nullptr;
    return std::unique_ptr<PlaybackEngine>(new PlaybackEngine(std::move(impl)));
}

std::string PlaybackEngine::video_decoder_name() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video ? impl_->video->name() : std::string();
}

const MediaInfo &PlaybackEngine::info() const
{
    return impl_->info;
}

double PlaybackEngine::duration() const
{
    return impl_->info.duration;
}

bool PlaybackEngine::has_video() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video_index >= 0 && impl_->video_error.empty();
}

bool PlaybackEngine::has_audio() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->audio_index >= 0;
}

std::string PlaybackEngine::video_error() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video_error;
}

std::string PlaybackEngine::audio_error() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->audio_error;
}

int PlaybackEngine::video_stream() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video_index;
}

int PlaybackEngine::audio_stream() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->audio_index;
}

int PlaybackEngine::subtitle_stream() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->subtitle_index;
}

bool PlaybackEngine::pump()
{
    return impl_->pump();
}

void PlaybackEngine::start()
{
    if (impl_->running)
        return;
    impl_->stopping.store(false);
    // The conversion stage first, so that the engine thread finds it running. Without it the
    // engine converts in line.
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->convert_stopping = false;
    }
    pthread_attr_t convert_attributes;
    pthread_attr_init(&convert_attributes);
    pthread_attr_setstacksize(&convert_attributes, 512 * 1024);
    impl_->convert_running.store(pthread_create(&impl_->convert_thread, &convert_attributes,
                                                &Impl::convert_main, impl_.get()) == 0);
    pthread_attr_destroy(&convert_attributes);
    // A bigger stack than the default: the decoders keep tables on it. Never name the thread:
    // pthread_setname_np hangs on the console.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    int result = pthread_create(&impl_->thread, &attributes, &Impl::thread_main, impl_.get());
    pthread_attr_destroy(&attributes);
    if (result != 0)
        result = pthread_create(&impl_->thread, nullptr, &Impl::thread_main, impl_.get());
    impl_->running = result == 0;
}

void PlaybackEngine::stop()
{
    if (!impl_->running)
        return;
    impl_->stopping.store(true);
    impl_->notify();
    pthread_join(impl_->thread, nullptr);
    impl_->running = false;
    if (impl_->convert_running.load())
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->convert_stopping = true;
        }
        impl_->convert_wake.notify_all();
        pthread_join(impl_->convert_thread, nullptr);
        impl_->convert_running.store(false);
    }
}

void PlaybackEngine::seek(double seconds)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->seek_requested = true;
        impl_->seek_target = std::max(seconds, 0.0);
        impl_->clear_queues_locked();
        impl_->awaiting_audio = impl_->audio_index >= 0;
    }
    impl_->notify();
}

void PlaybackEngine::select_audio(int stream, double resume_at)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->next_audio = stream < 0 ? -1 : stream;
    }
    seek(resume_at);
}

void PlaybackEngine::select_subtitle(int stream, double resume_at)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->next_subtitle = stream < 0 ? -1 : stream;
    }
    seek(resume_at);
}

bool PlaybackEngine::take_picture(double clock_seconds, VideoPicture &picture)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->pictures.empty())
        return false;
    std::vector<double> times;
    times.reserve(impl_->pictures.size());
    for (const auto &queued : impl_->pictures)
        times.push_back(queued->pts);
    const Selection pick = select_frame(times, clock_seconds, SyncConfig{});
    if (!pick.present)
        return false;
    for (std::size_t i = 0; i < pick.drop; ++i)
    {
        impl_->recycle_locked(std::move(impl_->pictures.front()));
        impl_->pictures.pop_front();
        ++impl_->counters.pictures_late;
    }
    std::unique_ptr<VideoPicture> chosen = std::move(impl_->pictures.front());
    impl_->pictures.pop_front();
    picture.pts = chosen->pts;
    picture.width = chosen->width;
    picture.height = chosen->height;
    picture.rgba.swap(chosen->rgba); // the caller's old buffer goes back into the pool
    impl_->recycle_locked(std::move(chosen));
    impl_->notify();
    return true;
}

std::vector<SubtitleLine> PlaybackEngine::subtitles_at(double seconds) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<SubtitleLine> lines;
    for (const Cue *cue : impl_->track.active_at(seconds))
        lines.push_back({cue->text, cue->align});
    return lines;
}

int PlaybackEngine::read_sound(float *out, int frames)
{
    return impl_->read_sound(out, frames);
}

int PlaybackEngine::sound_channels() const
{
    return impl_->options.sound_channels;
}

void PlaybackEngine::set_hdr_passthrough(bool on)
{
    impl_->hdr_passthrough.store(on);
}

bool PlaybackEngine::set_sound_channels(int channels)
{
    if (impl_->running || impl_->source_taken || impl_->audio_index < 0)
        return false;
    if (channels == impl_->options.sound_channels)
        return true;
    std::string reason;
    auto decoder = open_audio_decoder(*impl_->demuxer, impl_->audio_index, &reason, channels);
    if (!decoder)
        return false;
    // Nothing has been decoded yet when this is called before start(); a pump() before it may
    // have queued stereo chunks, which are dropped with the old decoder.
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->audio = std::move(decoder);
    impl_->options.sound_channels = channels;
    impl_->sound.clear();
    impl_->sound_front_offset = 0;
    impl_->sound_seconds = 0.0;
    return true;
}

std::unique_ptr<audio::Decoder> PlaybackEngine::make_audio_source()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->options.sound_channels != 2 || impl_->audio_index < 0 || impl_->source_taken)
            return nullptr;
        impl_->source_taken = true;
    }
    return std::make_unique<EngineAudioSource>(*impl_, *this);
}

bool PlaybackEngine::ended() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->finished && impl_->pictures.empty() && impl_->sound.empty();
}

PlaybackEngine::Stats PlaybackEngine::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Stats result = impl_->counters;
    result.pictures_ready = static_cast<int>(impl_->pictures.size());
    result.pictures_owned = impl_->owned_locked();
    result.audio_seconds_ready = impl_->sound_seconds;
    return result;
}

} // namespace hui::video
