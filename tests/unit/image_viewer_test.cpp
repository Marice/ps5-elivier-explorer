// EliVier Explorer - Picture decoding and the image viewer: formats, limits, worker, textures.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"
#include "fake_textures.hpp"

#include "explorer/local_source.hpp"
#include "explorer/worker.hpp"
#include "gfx/decode_image.hpp"
#include "viewer/image_viewer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

namespace fs = std::filesystem;
using hui::testing::ConceptFixture;
using hui::testing::FakeTextures;
using namespace hui::gfx;
using namespace hui::viewer;

const std::string kDir = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/images";

std::string slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

TEST(DecodeImage, ReadsEveryFormatAs64By48Rgba)
{
    for (const char *name : {"pic.png", "pic.jpg", "pic.bmp", "pic.tga", "pic.gif"})
    {
        ImageData image;
        std::string error;
        ASSERT_TRUE(decode_image(slurp(kDir + "/" + name), &image, &error))
            << name << ": " << error;
        EXPECT_EQ(image.width, 64) << name;
        EXPECT_EQ(image.height, 48) << name;
        EXPECT_EQ(image.rgba.size(), 64u * 48u * 4u) << name;
        // The test card is not one colour.
        bool varied = false;
        for (std::size_t i = 4; i < image.rgba.size() && !varied; i += 4)
            varied = image.rgba[i] != image.rgba[0] || image.rgba[i + 1] != image.rgba[1];
        EXPECT_TRUE(varied) << name;
    }
}

TEST(DecodeImage, RefusesWhatIsNotAPicture)
{
    ImageData image;
    std::string error;
    EXPECT_FALSE(decode_image("", &image, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(decode_image(std::string(500, 'q'), &image, &error));
    EXPECT_FALSE(decode_image("\x89PNG\r\n\x1a\n", &image, &error));
}

TEST(DecodeImage, ADamagedFileFailsWithoutCrashing)
{
    std::string bytes = slurp(kDir + "/pic.png");
    bytes.resize(bytes.size() / 2);
    ImageData image;
    std::string error;
    EXPECT_FALSE(decode_image(bytes, &image, &error));
    EXPECT_FALSE(error.empty());
    std::string jpeg = slurp(kDir + "/pic.jpg");
    jpeg.resize(jpeg.size() / 2);
    decode_image(jpeg, &image, &error); // either way, no crash
}

TEST(DecodeImage, AHugePictureIsRefusedBeforeItIsDecoded)
{
    // A PNG header that claims 9000 x 9000 pixels (81 megapixels), and nothing else; and one that
    // claims more than stb's own arithmetic allows (refused too, with its own wording).
    for (const char *size : {"\0\0\x23\x28", "\0\x01\x86\xa0"})
    {
        std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
        png += std::string(size, 4) + std::string(size, 4) + std::string("\x08\x02\0\0\0", 5);
        png += std::string(4, '\0');           // the checksum
        png += std::string("\0\0\0\0IDAT", 8); // the header scan stops at the first data chunk
        ImageData image;
        std::string error;
        EXPECT_FALSE(decode_image(png, &image, &error));
        if (size[1] == '\0') // 9000 x 9000: our own limit names the reason
            EXPECT_NE(error.find("large"), std::string::npos) << error;
    }
}

TEST(DecodeImage, BoxDownscaleAveragesBlocksAndKeepsOddEdges)
{
    ImageData image;
    image.width = 3;
    image.height = 2;
    image.rgba = {0,   0,   0,   255, 100, 100, 100, 255, 200, 200, 200, 255,
                  100, 100, 100, 255, 200, 200, 200, 255, 40,  40,  40,  255};
    box_downscale(&image, 2);
    ASSERT_EQ(image.width, 2);
    ASSERT_EQ(image.height, 1);
    EXPECT_EQ(image.rgba[0], 100); // (0 + 100 + 100 + 200) / 4
    EXPECT_EQ(image.rgba[3], 255);
    EXPECT_EQ(image.rgba[4], 120); // the odd column: (200 + 40) / 2
    box_downscale(&image, 1);      // a factor of one changes nothing
    EXPECT_EQ(image.width, 2);
}

TEST(DecodeImage, RecognizesPicturesByTheirFirstBytes)
{
    EXPECT_TRUE(looks_like_image("\x89PNG\r\n\x1a\n...."));
    EXPECT_TRUE(looks_like_image("\xff\xd8\xff\xe0"));
    EXPECT_TRUE(looks_like_image("GIF89a.."));
    EXPECT_TRUE(looks_like_image("BM...."));
    EXPECT_FALSE(looks_like_image("plain text"));
    EXPECT_FALSE(looks_like_image(""));
}

TEST(ImageHandlerMatch, ByExtensionAndByContent)
{
    const ImageHandler handler;
    for (const char *name : {"a.jpg", "A.JPEG", "b.png", "c.bmp", "d.gif", "e.tga"})
        EXPECT_EQ(handler.match(FileProbe(name, 10)), Match::extension) << name;
    EXPECT_EQ(handler.match(FileProbe("a.txt", 10)), Match::none);
    EXPECT_EQ(handler.match(FileProbe("noext", 10, [] { return std::string("\xff\xd8\xff\xe0"); })),
              Match::content);
    EXPECT_EQ(handler.match(FileProbe("noext", 10, [] { return std::string("hello"); })),
              Match::none);
}

class ImageViewerTest : public ConceptFixture
{
  protected:
    std::unique_ptr<ImageFileViewer> open(const std::string &path, FakeTextures *textures)
    {
        ViewerContext context;
        context.area = {96.0f, 270.0f, 1500.0f, 520.0f};
        context.fonts = &fonts_;
        context.worker = &worker_;
        context.textures = textures;
        FileRef file;
        file.source = &source_;
        file.path = path;
        source_.stat(path, &file.entry);
        return std::make_unique<ImageFileViewer>("pic", file, context);
    }
    void tick(ImageFileViewer &viewer, const hui::InputFrame &input = idle())
    {
        hui::ui::Feedback feedback;
        viewer.update(input, 1.0f / 60.0f, feedback);
    }
    // Lets the worker finish, then the viewer notice.
    void settle(ImageFileViewer &viewer)
    {
        worker_.wait_idle();
        tick(viewer);
    }
    std::size_t drawn(const ImageFileViewer &viewer)
    {
        hui::gfx::DrawList list;
        hui::ui::Painter paint(list, fonts_, hui::ui::default_theme());
        hui::ui::Canvas canvas{list, fonts_, 0, 0.0f};
        viewer.draw(paint, canvas, {96.0f, 270.0f, 1500.0f, 520.0f});
        for (const hui::gfx::Instance &instance : list.instances())
            for (const float value : instance.rect)
                EXPECT_TRUE(std::isfinite(value));
        return list.instances().size();
    }

    hui::explorer::LocalSource source_;
    hui::explorer::JobWorker worker_;
};

TEST_F(ImageViewerTest, DecodesOnTheWorkerAndMakesOneTexture)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/pic.jpg", &textures);
    EXPECT_FALSE(viewer->ready());
    EXPECT_GT(drawn(*viewer), 0u); // "Loading" meanwhile
    settle(*viewer);
    ASSERT_TRUE(viewer->ready()) << viewer->error();
    EXPECT_EQ(textures.created, 1);
    EXPECT_EQ(textures.last_width, 64);
    EXPECT_EQ(viewer->picture_height(), 48);
    EXPECT_FALSE(viewer->loading());
    EXPECT_GT(drawn(*viewer), 0u);
    tick(*viewer);
    EXPECT_EQ(textures.created, 1); // not again
}

TEST_F(ImageViewerTest, ClosingGivesTheTextureBack)
{
    FakeTextures textures;
    const auto viewer = open(kDir + "/pic.png", &textures);
    settle(*viewer);
    ASSERT_TRUE(viewer->ready());
    viewer->close();
    EXPECT_TRUE(textures.live.empty());
    EXPECT_EQ(textures.destroyed, 1);
    viewer->close(); // twice is fine
    EXPECT_EQ(textures.destroyed, 1);
}

TEST_F(ImageViewerTest, AnUnreadableOrBrokenFileSaysSoAndMakesNoTexture)
{
    FakeTextures textures;
    const auto missing = open(kDir + "/nope.png", &textures);
    settle(*missing);
    EXPECT_FALSE(missing->ready());
    EXPECT_FALSE(missing->error().empty());
    EXPECT_GT(drawn(*missing), 0u);

    const std::string junk = (fs::temp_directory_path() / "elivier-junk.png").string();
    std::ofstream(junk, std::ios::binary) << std::string(300, 'z');
    const auto broken = open(junk, &textures);
    settle(*broken);
    EXPECT_FALSE(broken->error().empty());
    EXPECT_EQ(textures.created, 0);
    fs::remove(junk);
}

TEST_F(ImageViewerTest, CircleClosesWhileLoadingAndWhenShown)
{
    FakeTextures textures;
    const auto early = open(kDir + "/pic.png", &textures);
    tick(*early, press(hui::Action::back));
    EXPECT_TRUE(early->closing());
    worker_.wait_idle();

    const auto shown = open(kDir + "/pic.png", &textures);
    settle(*shown);
    ASSERT_TRUE(shown->ready());
    tick(*shown, press(hui::Action::back));
    EXPECT_TRUE(shown->closing());
}

TEST_F(ImageViewerTest, WithoutATextureHostItSaysSoAtOnce)
{
    const auto viewer = open(kDir + "/pic.png", nullptr);
    EXPECT_FALSE(viewer->error().empty());
    EXPECT_FALSE(viewer->loading());
    EXPECT_GT(drawn(*viewer), 0u);
}

TEST_F(ImageViewerTest, AViewerGoneBeforeTheWorkerIsDoneLeavesNothingBehind)
{
    FakeTextures textures;
    {
        const auto viewer = open(kDir + "/pic.png", &textures);
        viewer->close();
    }
    worker_.wait_idle(); // the job finishes into a result nobody takes
    EXPECT_EQ(textures.created, 0);
}

TEST_F(ImageViewerTest, AFileOverTheLimitIsNotRead)
{
    FakeTextures textures;
    ViewerContext context;
    context.fonts = &fonts_;
    context.worker = &worker_;
    context.textures = &textures;
    FileRef file;
    file.source = &source_;
    file.path = kDir + "/pic.png";
    file.entry.size = kMaxImageFileBytes + 1;
    const ImageFileViewer viewer("big", file, context);
    EXPECT_FALSE(viewer.error().empty());
}

const std::string kDds = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/dds";

ImageData load(const std::string &path)
{
    ImageData image;
    std::string error;
    EXPECT_TRUE(decode_image(slurp(path), &image, &error)) << path << ": " << error;
    return image;
}

// The largest difference in one channel (0 = red, 1 = green, 2 = blue, 3 = alpha) between a
// decoded picture and a reference whose channel `from` is compared (the reference can be grey).
int worst_difference(const ImageData &got, const ImageData &want, int channel, int from)
{
    EXPECT_EQ(got.width, want.width);
    EXPECT_EQ(got.height, want.height);
    int worst = 0;
    for (std::size_t i = 0; i + 3 < got.rgba.size() && i + 3 < want.rgba.size(); i += 4)
        worst = std::max(
            worst, std::abs(static_cast<int>(got.rgba[i + static_cast<std::size_t>(channel)]) -
                            static_cast<int>(want.rgba[i + static_cast<std::size_t>(from)])));
    return worst;
}

TEST(Dds, Bc7MatchesTheReferenceDecoderExactly)
{
    const ImageData got = load(kDds + "/bc7.dds");
    const ImageData want = load(kDds + "/bc7.ref.png");
    ASSERT_EQ(got.width, 64);
    ASSERT_EQ(got.height, 48);
    for (int c = 0; c < 3; ++c)
        EXPECT_LE(worst_difference(got, want, c, c), 1) << "channel " << c;
}

TEST(Dds, Bc1AndBc3MatchTheReferenceWithinRounding)
{
    for (const char *name : {"bc1", "bc3"})
    {
        const ImageData got = load(kDds + "/" + name + ".dds");
        const ImageData want = load(kDds + "/" + name + ".ref.png");
        for (int c = 0; c < 3; ++c)
            EXPECT_LE(worst_difference(got, want, c, c), 4) << name << " channel " << c;
        EXPECT_EQ(got.rgba[3], 255) << name;
    }
}

TEST(Dds, Bc4IsGreyAndBc5KeepsItsTwoChannels)
{
    const ImageData grey = load(kDds + "/bc4.dds");
    const ImageData grey_ref = load(kDds + "/bc4.ref.png");
    EXPECT_LE(worst_difference(grey, grey_ref, 0, 0), 2);
    EXPECT_EQ(worst_difference(grey, grey, 1, 0), 0); // g equals r
    EXPECT_EQ(worst_difference(grey, grey, 2, 0), 0);

    const ImageData two = load(kDds + "/bc5.dds");
    const ImageData two_ref = load(kDds + "/bc5.ref.png");
    EXPECT_LE(worst_difference(two, two_ref, 0, 0), 2);
    EXPECT_LE(worst_difference(two, two_ref, 1, 1), 2);
}

TEST(Dds, AlphaSurvivesBc3AndBc7)
{
    for (const char *name : {"bc3-alpha", "bc7-alpha"})
    {
        const ImageData got = load(kDds + "/" + name + ".dds");
        const ImageData alpha = load(kDds + "/" + name + ".refa.png");
        EXPECT_LE(worst_difference(got, alpha, 3, 0), 4) << name;
        EXPECT_LT(got.rgba[3], 40) << name;             // the left edge is clear
        EXPECT_GE(got.rgba[(63) * 4 + 3], 248) << name; // the right edge is solid
    }
}

TEST(Dds, TheDx10HeaderGivesTheSameAsTheClassicOne)
{
    EXPECT_EQ(load(kDds + "/bc1-dx10.dds").rgba, load(kDds + "/bc1.dds").rgba);
    EXPECT_EQ(load(kDds + "/bc5-dx10.dds").rgba, load(kDds + "/bc5.dds").rgba);
}

TEST(Dds, UncompressedPixelsKeepTheirChannels)
{
    for (const char *name : {"bgra8", "bgr24"})
    {
        const ImageData image = load(kDds + "/" + name + ".dds");
        ASSERT_EQ(image.width, 8) << name;
        ASSERT_EQ(image.height, 4) << name;
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 8; ++x)
            {
                const std::uint8_t *p = &image.rgba[static_cast<std::size_t>(y * 8 + x) * 4];
                EXPECT_EQ(p[0], x * 30) << name;
                EXPECT_EQ(p[1], y * 60) << name;
                EXPECT_EQ(p[2], 200 - x * 10) << name;
                EXPECT_EQ(p[3], std::string(name) == "bgra8" ? 255 - y * 20 : 255) << name;
            }
        }
    }
}

TEST(Dds, DamagedFilesFailInsteadOfCrashing)
{
    ImageData image;
    std::string error;
    std::string bytes = slurp(kDds + "/bc7.dds");
    EXPECT_FALSE(decode_image(bytes.substr(0, 100), &image, &error)); // not even a header
    EXPECT_FALSE(decode_image(bytes.substr(0, bytes.size() - 100), &image, &error));
    EXPECT_NE(error.find("short"), std::string::npos) << error;
    std::string zero = bytes;
    std::fill(zero.begin() + 12, zero.begin() + 20, '\0'); // width and height 0
    EXPECT_FALSE(decode_image(zero, &image, &error));
    std::string huge = bytes;
    huge[16] = huge[17] = huge[18] = '\x7f'; // a width of millions
    EXPECT_FALSE(decode_image(huge, &image, &error));
    std::string weird = bytes;
    weird[84] = 'Z'; // an unknown four-letter code
    weird[148 - 20] = 'Z';
    EXPECT_FALSE(decode_image(weird, &image, &error));
    // The fuzz in miniature: every byte of the header set to a few values, never a crash.
    for (std::size_t i = 4; i < 148; ++i)
        for (const char v : {'\0', '\x01', '\xff'})
        {
            std::string mangled = bytes;
            mangled[i] = v;
            decode_image(mangled, &image, &error);
        }
}

TEST(Dds, ABlockOfBc2HasItsExplicitAlpha)
{
    // 4x4, DXT3: sixteen 4-bit alphas (0, 1, 2 ... 15), then white/white colours.
    std::string file = slurp(kDds + "/bc3.dds").substr(0, 128);
    file.replace(12, 8, std::string("\x04\0\0\0\x04\0\0\0", 8));
    file.replace(84, 4, "DXT3");
    std::string block;
    for (int i = 0; i < 8; ++i)
        block += static_cast<char>((2 * i) | ((2 * i + 1) << 4));
    block += std::string("\xff\xff\xff\xff\0\0\0\0", 8);
    ImageData image;
    std::string error;
    ASSERT_TRUE(decode_image(file + block, &image, &error)) << error;
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_EQ(image.rgba[static_cast<std::size_t>(i) * 4 + 3], i * 17) << i;
        EXPECT_EQ(image.rgba[static_cast<std::size_t>(i) * 4], 255);
    }
}

TEST(Dds, ThePictureHandlerOpensDdsFilesAndSniffsThem)
{
    const ImageHandler handler;
    EXPECT_EQ(handler.match(FileProbe("tex.DDS", 10)), Match::extension);
    EXPECT_EQ(handler.match(FileProbe("noext", 10, [] { return std::string("DDS |\0\0\0", 8); })),
              Match::content);
}

TEST_F(ImageViewerTest, ShowsADdsPicture)
{
    FakeTextures textures;
    const auto viewer = open(kDds + "/bc7.dds", &textures);
    settle(*viewer);
    ASSERT_TRUE(viewer->ready()) << viewer->error();
    EXPECT_EQ(viewer->picture_width(), 64);
    EXPECT_EQ(textures.last_height, 48);
}

} // namespace
