// EliVier Explorer - MP3 decoding, resampling, the player, playlists and the sound viewer.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"
#include "fake_textures.hpp"

#include "audio/decoder.hpp"
#include "audio/mixer.hpp"
#include "audio/player.hpp"
#include "audio/resampler.hpp"
#include "explorer/local_source.hpp"
#include "viewer/playlist.hpp"
#include "viewer/sound_viewer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

using namespace hui::audio;
using namespace hui::viewer;
using hui::testing::ConceptFixture;

const std::string kDir = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/mp3";
const std::string kStereo = kDir + "/tagged-stereo.mp3"; // 44.1 kHz, 0.5 s, 440 Hz, tags + cover
const std::string kMono = kDir + "/plain-mono-22k.mp3";  // 22.05 kHz, 0.5 s, no tags

std::string slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

ByteReader memory_reader(const std::string &data)
{
    return [&data](std::uint64_t offset, void *buffer, std::size_t size) -> std::int64_t
    {
        if (offset >= data.size())
            return 0;
        const std::size_t n = std::min(size, data.size() - static_cast<std::size_t>(offset));
        std::copy_n(data.data() + offset, n, static_cast<char *>(buffer));
        return static_cast<std::int64_t>(n);
    };
}

std::unique_ptr<Decoder> open(const std::string &data, std::string *error = nullptr)
{
    return open_mp3(memory_reader(data), data.size(), error);
}

// Decodes to the end; returns the frames and the loudest sample.
int drain(Decoder &decoder, float *peak = nullptr)
{
    std::vector<float> buffer(2048 * 2);
    int total = 0;
    float loudest = 0.0f;
    for (;;)
    {
        const int got = decoder.read(buffer.data(), 2048);
        if (got <= 0)
            break;
        total += got;
        for (int i = 0; i < got * 2; ++i)
            loudest = std::max(loudest, std::fabs(buffer[static_cast<std::size_t>(i)]));
    }
    if (peak != nullptr)
        *peak = loudest;
    return total;
}

TEST(Mp3Decoder, ReadsTheFormatTagsAndLength)
{
    const std::string data = slurp(kStereo);
    ASSERT_FALSE(data.empty());
    std::string error;
    const auto decoder = open(data, &error);
    ASSERT_NE(decoder, nullptr) << error;
    EXPECT_EQ(decoder->rate(), 44100);
    EXPECT_NEAR(decoder->duration(), 0.5, 0.1);
    EXPECT_EQ(decoder->title(), "Sine 440");
    EXPECT_EQ(decoder->artist(), "Test Artist");
    EXPECT_NE(decoder->description().find("44.1 kHz"), std::string::npos);
    EXPECT_NE(decoder->description().find("stereo"), std::string::npos);
}

TEST(Mp3Decoder, ReturnsThePictureInTheTag)
{
    const std::string data = slurp(kStereo);
    const auto decoder = open(data);
    ASSERT_NE(decoder, nullptr);
    EXPECT_EQ(decoder->cover().mime, "image/png");
    EXPECT_EQ(decoder->cover().data.compare(1, 3, "PNG"), 0); // the PNG signature after 0x89
}

TEST(Mp3Decoder, DecodesTheWholeSongAsASineWave)
{
    const std::string data = slurp(kStereo);
    const auto decoder = open(data);
    ASSERT_NE(decoder, nullptr);
    float peak = 0.0f;
    const int frames = drain(*decoder, &peak);
    EXPECT_NEAR(frames, 22050, 2500); // encoder delay and padding
    EXPECT_GT(peak, 0.05f);
    EXPECT_LT(peak, 1.0f);
    EXPECT_EQ(decoder->read(std::vector<float>(8).data(), 4), 0); // the end stays the end
}

TEST(Mp3Decoder, MonoComesOutAsTwoEqualChannels)
{
    const std::string data = slurp(kMono);
    const auto decoder = open(data);
    ASSERT_NE(decoder, nullptr);
    EXPECT_EQ(decoder->rate(), 22050);
    EXPECT_NE(decoder->description().find("mono"), std::string::npos);
    std::vector<float> buffer(4096 * 2);
    ASSERT_GT(decoder->read(buffer.data(), 4096), 0);
    for (int i = 0; i < 4096; ++i)
        EXPECT_EQ(buffer[static_cast<std::size_t>(i) * 2],
                  buffer[static_cast<std::size_t>(i) * 2 + 1]);
    EXPECT_TRUE(decoder->cover().data.empty());
    EXPECT_TRUE(decoder->title().empty());
}

TEST(Mp3Decoder, SeekingMovesAboutThatFarIntoTheSong)
{
    const std::string data = slurp(kMono);
    const auto decoder = open(data);
    ASSERT_NE(decoder, nullptr);
    ASSERT_TRUE(decoder->seek(0.25));
    const int rest = drain(*decoder);
    EXPECT_NEAR(rest, 0.25 * 22050, 2500);
    ASSERT_TRUE(decoder->seek(0.0)); // and back to the start
    EXPECT_NEAR(drain(*decoder), 0.5 * 22050, 2500);
}

TEST(Mp3Decoder, RefusesWhatIsNotAnMp3)
{
    std::string error;
    EXPECT_EQ(open(std::string(5000, 'x'), &error), nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(open("", &error), nullptr);
    EXPECT_EQ(open("ID3", &error), nullptr);
}

TEST(Mp3Decoder, ATruncatedFileEndsInsteadOfFailing)
{
    std::string data = slurp(kStereo);
    data.resize(data.size() / 2);
    const auto decoder = open(data);
    ASSERT_NE(decoder, nullptr);
    EXPECT_GT(drain(*decoder), 1000);
}

TEST(Mp3Decoder, ASlowOrFailingReaderIsHandled)
{
    const std::string data = slurp(kMono);
    int calls = 0;
    // Gives at most 100 bytes at a time, then fails after a while.
    const ByteReader stingy = [&](std::uint64_t offset, void *buffer,
                                  std::size_t size) -> std::int64_t
    {
        if (++calls > 400)
            return -5;
        return memory_reader(data)(offset, buffer, std::min<std::size_t>(size, 100));
    };
    const auto decoder = open_mp3(stingy, data.size(), nullptr);
    if (decoder != nullptr)
        EXPECT_GE(drain(*decoder), 0); // must not hang or crash
}

TEST(Resampler, SameRateIsACopy)
{
    Resampler resampler(48000, 48000);
    std::vector<float> in(20), out(20);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i);
    resampler.push(in.data(), 10);
    EXPECT_EQ(resampler.pull(out.data(), 20), 10);
    EXPECT_EQ(out, in);
}

TEST(Resampler, DoublingTheRateDoublesTheFramesAndInterpolates)
{
    Resampler resampler(24000, 48000);
    std::vector<float> in(2 * 100);
    for (int i = 0; i < 100; ++i)
        in[static_cast<std::size_t>(i) * 2] = in[static_cast<std::size_t>(i) * 2 + 1] =
            static_cast<float>(i);
    resampler.push(in.data(), 100);
    std::vector<float> out(2 * 300);
    const int made = resampler.pull(out.data(), 300);
    EXPECT_NEAR(made, 198, 2);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[2], 0.5f); // halfway between the first two input frames
    EXPECT_FLOAT_EQ(out[4], 1.0f);
}

TEST(Resampler, PullingInPiecesGivesTheSameSamplesAsAllAtOnce)
{
    std::vector<float> in(2 * 500);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(static_cast<float>(i) * 0.05f);
    Resampler whole(44100, 48000);
    whole.push(in.data(), 500);
    std::vector<float> expected(2 * 600);
    const int total = whole.pull(expected.data(), 600);

    Resampler pieces(44100, 48000);
    std::vector<float> got;
    for (int from = 0; from < 500; from += 37)
    {
        pieces.push(in.data() + from * 2, std::min(37, 500 - from));
        std::vector<float> part(2 * 64);
        for (int n; (n = pieces.pull(part.data(), 64)) > 0;)
            got.insert(got.end(), part.begin(), part.begin() + n * 2);
    }
    ASSERT_EQ(static_cast<int>(got.size() / 2), total);
    for (std::size_t i = 0; i < got.size(); ++i)
        EXPECT_NEAR(got[i], expected[i], 1e-5f);
}

TEST(Resampler, NothingComesOutOfNothingAndResetForgets)
{
    Resampler resampler(22050, 48000);
    std::vector<float> out(64);
    EXPECT_EQ(resampler.pull(out.data(), 32), 0);
    const float one[2] = {1.0f, 1.0f};
    resampler.push(one, 1); // a single frame cannot be interpolated
    EXPECT_EQ(resampler.pull(out.data(), 32), 0);
    resampler.reset();
    EXPECT_EQ(resampler.ready(), 0);
}

TEST(Playlist, ReadsPathsAndTitlesAndResolvesRelativeOnes)
{
    const std::string text =
        "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:12,First one\r\na.mp3\r\n\r\n# a comment\r\nsub\\b.mp3\r\n"
        "/data/abs/c.mp3\r\n../up.mp3\r\nhttp://x.example/stream.mp3\r\n";
    const auto list = parse_m3u(text, "/data/music/lists");
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list[0].path, "/data/music/lists/a.mp3");
    EXPECT_EQ(list[0].title, "First one");
    EXPECT_EQ(list[1].path, "/data/music/lists/sub/b.mp3");
    EXPECT_EQ(list[1].title, ""); // the title belonged to the line above only
    EXPECT_EQ(list[2].path, "/data/abs/c.mp3");
    EXPECT_EQ(list[3].path, "/data/music/up.mp3");
}

TEST(Playlist, EmptyAndCommentOnlyFilesGiveNothing)
{
    EXPECT_TRUE(parse_m3u("", "/data").empty());
    EXPECT_TRUE(parse_m3u("#EXTM3U\n#EXTINF:1,x\n", "/data").empty());
}

TEST(Playlist, NormalizeResolvesDotsAndNeverClimbsAboveTheRoot)
{
    EXPECT_EQ(normalize_path("/data/a/../b/./c.mp3"), "/data/b/c.mp3");
    EXPECT_EQ(normalize_path("/../../x"), "/x");
    EXPECT_EQ(normalize_path("/"), "/");
    EXPECT_EQ(normalize_path("//a//b/"), "/a/b");
    EXPECT_EQ(normalize_path("build/x/../y/z.mp3"), "build/y/z.mp3"); // relative stays relative
    EXPECT_EQ(normalize_path(""), ".");
}

TEST(Clock, FormatsMinutesAndHours)
{
    EXPECT_EQ(format_clock(0.0), "0:00");
    EXPECT_EQ(format_clock(59.9), "0:59");
    EXPECT_EQ(format_clock(187.0), "3:07");
    EXPECT_EQ(format_clock(3723.0), "1:02:03");
    EXPECT_EQ(format_clock(-4.0), "0:00");
}

std::vector<std::int16_t> render(Mixer &mixer, int frames)
{
    std::vector<std::int16_t> out(static_cast<std::size_t>(frames) * 2);
    mixer.render(out.data(), frames);
    return out;
}

// A song held in memory for as long as the test runs.
struct Song
{
    explicit Song(const std::string &path) : data(slurp(path))
    {
    }
    std::unique_ptr<Decoder> decoder() const
    {
        return open_mp3([this](std::uint64_t o, void *b, std::size_t n)
                        { return memory_reader(data)(o, b, n); }, data.size(), nullptr);
    }
    std::string data;
};

TEST(AudioPlayer, PlaysASongThroughTheMixerAndCountsTheTime)
{
    const Song song(kStereo);
    Mixer mixer;
    AudioPlayer player(mixer);
    EXPECT_FALSE(player.active());
    player.start(song.decoder());
    ASSERT_TRUE(player.active());
    EXPECT_NEAR(player.position(), 0.0, 0.01);
    player.pump(0.0f);
    const auto out = render(mixer, 4800); // 0.1 s reaches the speakers
    double peak = 0.0;
    for (const std::int16_t v : out)
        peak = std::max(peak, std::fabs(static_cast<double>(v)));
    EXPECT_GT(peak, 500.0);
    EXPECT_NEAR(player.position(), 0.1, 0.03);
    EXPECT_FALSE(player.finished());
}

TEST(AudioPlayer, FinishesAtTheEndOfTheSong)
{
    const Song song(kMono);
    Mixer mixer;
    AudioPlayer player(mixer);
    player.start(song.decoder());
    for (int i = 0; i < 100 && !player.finished(); ++i)
    {
        player.pump(1.0f / 60.0f);
        render(mixer, 800);
    }
    EXPECT_TRUE(player.finished());
    EXPECT_NEAR(player.position(), player.duration(), 0.1);
}

TEST(AudioPlayer, PausingHoldsThePositionAndResumingContinues)
{
    const Song song(kStereo);
    Mixer mixer;
    AudioPlayer player(mixer);
    player.start(song.decoder());
    render(mixer, 2400);
    player.set_paused(true);
    for (int i = 0; i < 10; ++i) // the fade-out, then the stream is parked
    {
        player.pump(0.02f);
        render(mixer, 960);
    }
    const double held = player.position();
    for (int i = 0; i < 10; ++i)
    {
        player.pump(0.02f);
        render(mixer, 960);
    }
    EXPECT_DOUBLE_EQ(player.position(), held);
    EXPECT_TRUE(player.paused());
    player.set_paused(false);
    for (int i = 0; i < 5; ++i)
    {
        player.pump(0.02f);
        render(mixer, 960);
    }
    EXPECT_GT(player.position(), held + 0.05);
}

TEST(AudioPlayer, SeekingJumpsInsideTheSongAndClamps)
{
    const Song song(kMono);
    Mixer mixer;
    AudioPlayer player(mixer);
    player.start(song.decoder());
    render(mixer, 480);
    player.seek_to(0.3);
    render(mixer, 480);
    EXPECT_NEAR(player.position(), 0.31, 0.05);
    player.seek_by(-10.0); // before the start: the start
    render(mixer, 480);
    EXPECT_LT(player.position(), 0.05);
    player.seek_by(100.0); // past the end: near the end
    EXPECT_LE(player.position(), player.duration());
}

TEST(AudioPlayer, StoppingSilencesTheStreamAndAllowsAnotherSong)
{
    const Song one(kStereo);
    const Song two(kMono);
    Mixer mixer;
    AudioPlayer player(mixer);
    player.start(one.decoder());
    render(mixer, 2400);
    player.stop();
    player.stop(); // twice is fine
    EXPECT_FALSE(player.active());
    const auto quiet = render(mixer, 4800);
    for (const std::int16_t v : quiet)
        ASSERT_LT(std::abs(v), 40); // the fade of the last grain, then nothing
    player.start(two.decoder());
    EXPECT_TRUE(player.active());
    EXPECT_NEAR(player.position(), 0.0, 0.01);
}

TEST(AudioPlayer, VolumeStaysInRange)
{
    Mixer mixer;
    AudioPlayer player(mixer);
    player.set_volume(99);
    EXPECT_EQ(player.volume(), 10);
    player.set_volume(-3);
    EXPECT_EQ(player.volume(), 0);
}

class SoundViewerTest : public ConceptFixture
{
  protected:
    std::unique_ptr<SoundViewer> viewer_of(std::vector<std::string> paths)
    {
        std::vector<PlaylistEntry> tracks;
        for (const std::string &path : paths)
            tracks.push_back({path, ""});
        ViewerContext context;
        context.fonts = &fonts_;
        context.mixer = &mixer_;
        context.textures = &textures_;
        return std::make_unique<SoundViewer>("test.mp3", &source_, std::move(tracks), context);
    }
    void send_input(Viewer &viewer, const hui::InputFrame &input, float dt = 1.0f / 60.0f)
    {
        hui::ui::Feedback feedback;
        viewer.update(input, dt, feedback);
    }

    Mixer mixer_;
    hui::testing::FakeTextures textures_;
    hui::explorer::LocalSource source_;
};

TEST(SoundHandlerMatch, KnowsSongsAndPlaylistsByExtensionAndMp3sByContent)
{
    const SoundHandler handler;
    EXPECT_EQ(handler.match(FileProbe("a.MP3", 10)), Match::extension);
    EXPECT_EQ(handler.match(FileProbe("list.m3u", 10)), Match::extension);
    EXPECT_EQ(handler.match(FileProbe("list.m3u8", 10)), Match::extension);
    EXPECT_EQ(handler.match(FileProbe("a.txt", 10)), Match::none);
    EXPECT_EQ(handler.match(FileProbe("noext", 10, [] { return std::string("ID3\x03rest"); })),
              Match::content);
    EXPECT_EQ(handler.match(FileProbe("noext", 10, [] { return std::string("plain"); })),
              Match::none);
}

TEST_F(SoundViewerTest, PlaysAndTogglesPauseWithCross)
{
    const auto viewer = viewer_of({kStereo});
    ASSERT_TRUE(viewer->error().empty());
    ASSERT_NE(viewer->player(), nullptr);
    EXPECT_TRUE(viewer->player()->active());
    send_input(*viewer, press(hui::Action::confirm));
    EXPECT_TRUE(viewer->player()->paused());
    send_input(*viewer, press(hui::Action::confirm));
    EXPECT_FALSE(viewer->player()->paused());
}

TEST_F(SoundViewerTest, TheAlbumPictureBecomesATextureAndIsGivenBackOnClose)
{
    const auto viewer = viewer_of({kStereo, kMono});
    EXPECT_EQ(textures_.created, 1); // the first song has a picture
    EXPECT_EQ(textures_.last_width, 300);
    send_input(*viewer, press(hui::Action::page_next)); // the second has none: the first is let go
    EXPECT_EQ(textures_.created, 1);
    EXPECT_TRUE(textures_.live.empty());
    send_input(*viewer, press(hui::Action::page_prev));
    EXPECT_EQ(textures_.live.size(), 1u);
    viewer->close();
    EXPECT_TRUE(textures_.live.empty());
}

TEST_F(SoundViewerTest, CircleStopsTheMusicAndCloses)
{
    const auto viewer = viewer_of({kStereo});
    send_input(*viewer, press(hui::Action::back));
    EXPECT_TRUE(viewer->closing());
    EXPECT_FALSE(viewer->player()->active());
}

TEST_F(SoundViewerTest, UpAndDownChangeTheVolume)
{
    const auto viewer = viewer_of({kStereo});
    send_input(*viewer, nav(hui::Direction::down));
    EXPECT_EQ(viewer->player()->volume(), 9);
    send_input(*viewer, nav(hui::Direction::up));
    send_input(*viewer, nav(hui::Direction::up));
    EXPECT_EQ(viewer->player()->volume(), 10);
}

TEST_F(SoundViewerTest, ASongThatCannotBePlayedIsSkippedInAList)
{
    const auto viewer = viewer_of({"/nonexistent/a.mp3", kMono, "/nonexistent/b.mp3"});
    EXPECT_EQ(viewer->track_index(), 1); // the first one that plays
    EXPECT_TRUE(viewer->player()->active());
    send_input(*viewer,
               press(hui::Action::page_next)); // b fails, a fails, back to the one that plays
    EXPECT_EQ(viewer->track_index(), 1);
}

TEST_F(SoundViewerTest, NothingPlayableSaysSo)
{
    const auto viewer = viewer_of({"/nonexistent/a.mp3"});
    EXPECT_FALSE(viewer->error().empty());
    EXPECT_FALSE(viewer->player()->active());
}

TEST_F(SoundViewerTest, ListsMoveWithShoulderButtonsAndWrap)
{
    const auto viewer = viewer_of({kStereo, kMono});
    EXPECT_EQ(viewer->track_index(), 0);
    send_input(*viewer, press(hui::Action::page_next));
    EXPECT_EQ(viewer->track_index(), 1);
    send_input(*viewer, press(hui::Action::page_next));
    EXPECT_EQ(viewer->track_index(), 0);
    send_input(*viewer, press(hui::Action::page_prev));
    EXPECT_EQ(viewer->track_index(), 1);
}

TEST_F(SoundViewerTest, WithoutAMixerItSaysThereIsNoAudioOutput)
{
    ViewerContext context;
    context.fonts = &fonts_;
    SoundViewer viewer("x.mp3", &source_, {{kStereo, ""}}, context);
    EXPECT_FALSE(viewer.error().empty());
    send_input(viewer, press(hui::Action::confirm)); // nothing to do, nothing breaks
    send_input(viewer, press(hui::Action::back));
    EXPECT_TRUE(viewer.closing());
}

TEST_F(SoundViewerTest, DrawsFiniteShapesForAPlayingListAndForAnError)
{
    const auto draw = [this](const SoundViewer &viewer)
    {
        hui::gfx::DrawList list;
        hui::ui::Painter paint(list, fonts_, hui::ui::default_theme());
        hui::ui::Canvas canvas{list, fonts_, 0, 0.0f};
        viewer.draw(paint, canvas, {96.0f, 270.0f, 1200.0f, 560.0f});
        EXPECT_FALSE(list.empty());
        for (const hui::gfx::Instance &instance : list.instances())
            for (const float value : instance.rect)
                ASSERT_TRUE(std::isfinite(value));
    };
    draw(*viewer_of({kStereo, kMono}));
    draw(*viewer_of({"/nonexistent/a.mp3"}));
}

} // namespace
