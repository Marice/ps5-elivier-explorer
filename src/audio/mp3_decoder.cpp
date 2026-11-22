// EliVier Explorer - MP3 decoding on minimp3, streamed through a range reader.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/decoder.hpp"

#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hui::audio
{

namespace
{

constexpr std::size_t kWindow = 16u << 10;      // minimp3 wants about this much to find a frame
constexpr std::size_t kMaxId3 = 1u << 20;       // a tag larger than this is skipped, not parsed
constexpr std::size_t kMaxCover = 8u << 20;     // a larger picture is left out
constexpr std::uint64_t kProbeBytes = 1u << 20; // how far to look for the first frame

std::uint32_t synchsafe(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0] & 0x7f) << 21 |
           static_cast<std::uint32_t>(p[1] & 0x7f) << 14 |
           static_cast<std::uint32_t>(p[2] & 0x7f) << 7 | static_cast<std::uint32_t>(p[3] & 0x7f);
}

void append_utf8(std::string *out, std::uint32_t cp)
{
    if (cp < 0x80)
        out->push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    else if (cp < 0x10000)
    {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    else
    {
        out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

// An ID3 text frame body (encoding byte, then text) as UTF-8. Control characters are dropped.
std::string id3_text(const unsigned char *p, std::size_t n)
{
    std::string out;
    if (n < 1)
        return out;
    const int encoding = p[0];
    ++p;
    --n;
    if (encoding == 0) // Latin-1
    {
        for (std::size_t i = 0; i < n && p[i] != 0; ++i)
            if (p[i] >= 0x20)
                append_utf8(&out, p[i]);
    }
    else if (encoding == 1 || encoding == 2) // UTF-16, with or without a BOM
    {
        bool big = encoding == 2;
        std::size_t i = 0;
        if (encoding == 1 && n >= 2)
        {
            if (p[0] == 0xfe && p[1] == 0xff)
                big = true, i = 2;
            else if (p[0] == 0xff && p[1] == 0xfe)
                i = 2;
        }
        for (; i + 1 < n; i += 2)
        {
            std::uint32_t unit = big ? static_cast<std::uint32_t>(p[i] << 8 | p[i + 1])
                                     : static_cast<std::uint32_t>(p[i + 1] << 8 | p[i]);
            if (unit == 0)
                break;
            if (unit >= 0xd800 && unit < 0xdc00 && i + 3 < n)
            {
                const std::uint32_t low =
                    big ? static_cast<std::uint32_t>(p[i + 2] << 8 | p[i + 3])
                        : static_cast<std::uint32_t>(p[i + 3] << 8 | p[i + 2]);
                if (low >= 0xdc00 && low < 0xe000)
                {
                    unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                    i += 2;
                }
            }
            if (unit >= 0x20)
                append_utf8(&out, unit);
        }
    }
    else // UTF-8
    {
        for (std::size_t i = 0; i < n && p[i] != 0; ++i)
            if (p[i] >= 0x20 || p[i] >= 0x80)
                out.push_back(static_cast<char>(p[i]));
    }
    return out;
}

std::string trim_field(const unsigned char *p, std::size_t n)
{
    std::string out;
    for (std::size_t i = 0; i < n && p[i] != 0; ++i)
        append_utf8(&out, p[i] >= 0x20 ? p[i] : 0x20u);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

class Mp3Decoder final : public Decoder
{
  public:
    Mp3Decoder(ByteReader reader, std::uint64_t size) : reader_(std::move(reader)), size_(size)
    {
        window_.resize(kWindow * 2);
    }

    // Reads the tags and the first frame. Returns an error, or "" when it can play.
    std::string open()
    {
        audio_start_ = 0;
        audio_end_ = size_;
        read_id3v2();
        read_id3v1();
        if (audio_end_ <= audio_start_)
            return "no audio in the file";

        // The first frame says the rate and channels; a Xing/Info frame says the length.
        begin_stream(audio_start_);
        mp3dec_frame_info_t info;
        std::uint64_t probed = 0;
        for (;;)
        {
            if (!fill())
                return "not an MP3 file";
            const int samples = mp3dec_decode_frame(&decoder_, window_.data() + head_,
                                                    static_cast<int>(tail_ - head_), pcm_, &info);
            if (info.frame_bytes <= 0)
                return "not an MP3 file";
            if (samples > 0)
            {
                rate_ = info.hz;
                channels_ = info.channels;
                bitrate_ = info.bitrate_kbps;
                layer_ = info.layer;
                frame_samples_ = info.hz >= 32000 ? 1152 : 576;
                if (layer_ == 1)
                    frame_samples_ = 384;
                const unsigned char *frame = window_.data() + head_;
                const std::size_t frame_size = static_cast<std::size_t>(info.frame_bytes);
                const std::uint64_t frame_pos = file_pos_ - (tail_ - head_);
                if (!read_xing(frame, frame_size))
                    begin_stream(frame_pos); // a real frame: decode it again from its start
                else
                {
                    audio_start_ = frame_pos + frame_size;
                    begin_stream(audio_start_);
                }
                break;
            }
            // Skipped something that was not a frame (a tag, padding).
            head_ += static_cast<std::size_t>(info.frame_bytes);
            probed += static_cast<std::uint64_t>(info.frame_bytes);
            if (probed > kProbeBytes)
                return "not an MP3 file";
        }
        if (rate_ <= 0 || channels_ <= 0)
            return "not an MP3 file";
        if (xing_frames_ > 0)
            duration_ = static_cast<double>(xing_frames_) * frame_samples_ / rate_;
        else if (bitrate_ > 0)
            duration_ = static_cast<double>(audio_end_ - audio_start_) * 8.0 / (bitrate_ * 1000.0);
        char text[96];
        std::snprintf(text, sizeof(text), "MP3, %.1f kHz, %d kbps%s, %s", rate_ / 1000.0, bitrate_,
                      xing_frames_ > 0 ? " average" : "", channels_ == 1 ? "mono" : "stereo");
        description_ = text;
        return "";
    }

    int rate() const override
    {
        return rate_;
    }
    double duration() const override
    {
        return duration_;
    }

    int read(float *stereo, int frames) override
    {
        int done = 0;
        while (done < frames)
        {
            if (pcm_pos_ < pcm_frames_)
            {
                const int take = std::min(frames - done, pcm_frames_ - pcm_pos_);
                for (int i = 0; i < take; ++i)
                {
                    const float *in = pcm_ + static_cast<std::size_t>(pcm_pos_ + i) *
                                                 static_cast<std::size_t>(channels_);
                    stereo[(done + i) * 2] = in[0];
                    stereo[(done + i) * 2 + 1] = channels_ > 1 ? in[1] : in[0];
                }
                pcm_pos_ += take;
                done += take;
                continue;
            }
            if (!next_frame())
                break;
        }
        return done;
    }

    bool seek(double seconds) override
    {
        if (duration_ <= 0.0)
            return false;
        const double fraction = std::clamp(seconds / duration_, 0.0, 1.0);
        const std::uint64_t span = audio_end_ - audio_start_;
        begin_stream(audio_start_ +
                     static_cast<std::uint64_t>(fraction * static_cast<double>(span)));
        return true;
    }

    const std::string &title() const override
    {
        return title_;
    }
    const std::string &artist() const override
    {
        return artist_;
    }
    const std::string &description() const override
    {
        return description_;
    }
    const CoverArt &cover() const override
    {
        return cover_;
    }

  private:
    // Starts decoding at a file offset: an empty window and a fresh decoder state.
    void begin_stream(std::uint64_t offset)
    {
        file_pos_ = std::min(offset, size_);
        head_ = tail_ = 0;
        eof_ = false;
        pcm_frames_ = pcm_pos_ = 0;
        mp3dec_init(&decoder_);
    }

    // Tops the window up so a whole frame is in it. False when there is nothing left to decode.
    bool fill()
    {
        if (head_ > 0 && tail_ - head_ < kWindow)
        {
            std::memmove(window_.data(), window_.data() + head_, tail_ - head_);
            tail_ -= head_;
            head_ = 0;
        }
        while (!eof_ && tail_ - head_ < kWindow && file_pos_ < audio_end_)
        {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(window_.size() - tail_, audio_end_ - file_pos_));
            if (want == 0)
                break;
            const std::int64_t got = reader_(file_pos_, window_.data() + tail_, want);
            if (got <= 0)
            {
                eof_ = true;
                break;
            }
            tail_ += static_cast<std::size_t>(got);
            file_pos_ += static_cast<std::uint64_t>(got);
        }
        return tail_ > head_;
    }

    // Decodes the next frame into pcm_. False at the end of the stream.
    bool next_frame()
    {
        for (int guard = 0; guard < 64; ++guard)
        {
            if (!fill())
                return false;
            mp3dec_frame_info_t info;
            const int samples = mp3dec_decode_frame(&decoder_, window_.data() + head_,
                                                    static_cast<int>(tail_ - head_), pcm_, &info);
            if (info.frame_bytes <= 0)
            {
                // Not enough data for a frame: at the end of the file that is the end of the song.
                if (eof_ || file_pos_ >= audio_end_)
                    return false;
                head_ = tail_; // garbage with no frame in it: read on
                continue;
            }
            head_ += static_cast<std::size_t>(info.frame_bytes);
            if (samples > 0)
            {
                pcm_frames_ = samples;
                pcm_pos_ = 0;
                return true;
            }
        }
        return false;
    }

    // Reads the length from a Xing/Info header, when the first frame is one.
    bool read_xing(const unsigned char *frame, std::size_t size)
    {
        const std::size_t limit = std::min<std::size_t>(size, 64);
        for (std::size_t i = 4; i + 12 <= limit; ++i)
        {
            if (std::memcmp(frame + i, "Xing", 4) == 0 || std::memcmp(frame + i, "Info", 4) == 0)
            {
                const std::uint32_t flags = static_cast<std::uint32_t>(frame[i + 4]) << 24 |
                                            static_cast<std::uint32_t>(frame[i + 5]) << 16 |
                                            static_cast<std::uint32_t>(frame[i + 6]) << 8 |
                                            frame[i + 7];
                if (flags & 1u)
                    xing_frames_ = static_cast<std::uint32_t>(frame[i + 8]) << 24 |
                                   static_cast<std::uint32_t>(frame[i + 9]) << 16 |
                                   static_cast<std::uint32_t>(frame[i + 10]) << 8 | frame[i + 11];
                return true;
            }
        }
        return false;
    }

    void read_id3v2()
    {
        unsigned char head[10];
        if (reader_(0, head, sizeof(head)) != 10 || std::memcmp(head, "ID3", 3) != 0)
            return;
        const std::uint32_t body = synchsafe(head + 6);
        const int version = head[3];
        audio_start_ = 10u + body + ((head[5] & 0x10) != 0 ? 10u : 0u);
        if (body > kMaxId3 || version < 2 || version > 4)
            return;
        std::vector<unsigned char> tag(body);
        if (reader_(10, tag.data(), tag.size()) != static_cast<std::int64_t>(tag.size()))
            return;
        std::size_t at = 0;
        const std::size_t id_len = version == 2 ? 3 : 4;
        const std::size_t head_len = version == 2 ? 6 : 10;
        while (at + head_len <= tag.size() && tag[at] != 0)
        {
            const unsigned char *frame = tag.data() + at;
            std::size_t size;
            if (version == 2)
                size = static_cast<std::size_t>(frame[3]) << 16 |
                       static_cast<std::size_t>(frame[4]) << 8 | frame[5];
            else if (version == 3)
                size = static_cast<std::size_t>(frame[4]) << 24 |
                       static_cast<std::size_t>(frame[5]) << 16 |
                       static_cast<std::size_t>(frame[6]) << 8 | frame[7];
            else
                size = synchsafe(frame + 4);
            if (at + head_len + size > tag.size())
                break;
            const std::string id(reinterpret_cast<const char *>(frame), id_len);
            if (id == "TIT2" || id == "TT2")
                title_ = id3_text(frame + head_len, size);
            else if (id == "TPE1" || id == "TP1")
                artist_ = id3_text(frame + head_len, size);
            else if ((id == "APIC" || id == "PIC") && cover_.data.empty())
                read_picture(frame + head_len, size, version == 2);
            at += head_len + size;
        }
    }

    // An attached picture: encoding, mime type, picture type, description, then the bytes.
    void read_picture(const unsigned char *p, std::size_t n, bool legacy)
    {
        if (n < 8 || n > kMaxCover)
            return;
        const int encoding = p[0];
        std::size_t at = 1;
        std::string mime;
        if (legacy) // a 3-letter format: "JPG" or "PNG"
        {
            const std::string format(reinterpret_cast<const char *>(p + 1), 3);
            mime = format == "PNG" ? "image/png" : "image/jpeg";
            at += 3;
        }
        else
        {
            while (at < n && p[at] != 0)
                mime.push_back(static_cast<char>(p[at++]));
            ++at;
        }
        ++at; // the picture type
        if (at >= n)
            return;
        // The description ends at a zero (two zeros for UTF-16).
        const bool wide = encoding == 1 || encoding == 2;
        while (at < n)
        {
            if (wide ? (at + 1 < n && p[at] == 0 && p[at + 1] == 0) : p[at] == 0)
            {
                at += wide ? 2 : 1;
                break;
            }
            at += wide ? 2 : 1;
        }
        if (at >= n)
            return;
        cover_.mime = mime.empty() ? "image/jpeg" : mime;
        cover_.data.assign(reinterpret_cast<const char *>(p + at), n - at);
    }

    void read_id3v1()
    {
        if (size_ < 128)
            return;
        unsigned char tag[128];
        if (reader_(size_ - 128, tag, sizeof(tag)) != 128 || std::memcmp(tag, "TAG", 3) != 0)
            return;
        audio_end_ = size_ - 128;
        if (title_.empty())
            title_ = trim_field(tag + 3, 30);
        if (artist_.empty())
            artist_ = trim_field(tag + 33, 30);
    }

    ByteReader reader_;
    std::uint64_t size_;
    std::uint64_t audio_start_ = 0;
    std::uint64_t audio_end_ = 0;
    std::uint64_t file_pos_ = 0; // the next byte to read from the file
    std::vector<unsigned char> window_;
    std::size_t head_ = 0; // the next unread byte in window_
    std::size_t tail_ = 0; // one past the last byte in window_
    bool eof_ = false;
    mp3dec_t decoder_{};
    float pcm_[MINIMP3_MAX_SAMPLES_PER_FRAME]{};
    int pcm_frames_ = 0;
    int pcm_pos_ = 0;
    int rate_ = 0;
    int channels_ = 0;
    int bitrate_ = 0;
    int layer_ = 0;
    int frame_samples_ = 1152;
    std::uint32_t xing_frames_ = 0;
    double duration_ = 0.0;
    std::string title_;
    std::string artist_;
    std::string description_;
    CoverArt cover_;
};

} // namespace

std::unique_ptr<Decoder> open_mp3(ByteReader reader, std::uint64_t size, std::string *error)
{
    auto decoder = std::make_unique<Mp3Decoder>(std::move(reader), size);
    const std::string problem = decoder->open();
    if (!problem.empty())
    {
        if (error != nullptr)
            *error = problem;
        return nullptr;
    }
    return decoder;
}

} // namespace hui::audio
