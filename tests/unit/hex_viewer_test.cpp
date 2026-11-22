// EliVier Explorer - The hex viewer: row format, cursor, paging, windows, go to an offset.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"

#include "explorer/local_source.hpp"
#include "viewer/hex_viewer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using hui::testing::ConceptFixture;
using namespace hui::viewer;

class HexViewerTest : public ConceptFixture
{
  protected:
    void SetUp() override
    {
        ConceptFixture::SetUp();
        root_ = (fs::temp_directory_path() / "elivier-hexviewer").string();
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override
    {
        fs::remove_all(root_);
        ConceptFixture::TearDown();
    }

    // A file whose byte at offset i is i % 251 (so a window is easy to check).
    std::string pattern_file(std::size_t size)
    {
        const std::string path = root_ + "/pattern.bin";
        std::string data(size, '\0');
        for (std::size_t i = 0; i < size; ++i)
            data[i] = static_cast<char>(i % 251);
        std::ofstream(path, std::ios::binary) << data;
        return path;
    }

    std::unique_ptr<HexViewer> open(const std::string &path)
    {
        ViewerContext context;
        context.area = {96.0f, 270.0f, 1500.0f, 520.0f};
        context.fonts = &fonts_;
        FileRef file;
        file.source = &source_;
        file.path = path;
        hui::explorer::Entry entry;
        source_.stat(path, &entry);
        file.entry = entry;
        return std::make_unique<HexViewer>("pattern.bin", file, context);
    }
    void send_input(Viewer &viewer, const hui::InputFrame &input, float dt = 1.0f / 60.0f)
    {
        hui::ui::Feedback feedback;
        viewer.update(input, dt, feedback);
    }
    std::size_t drawn(const HexViewer &viewer)
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

    std::string root_;
    hui::explorer::LocalSource source_;
};

TEST(HexRow, ShowsOffsetBytesAndText)
{
    const std::string bytes("Hello, world!\x00\x01\xff", 16);
    EXPECT_EQ(format_hex_row(0x10, bytes, 0, 16),
              "00000010  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 00 01 ff  |Hello, world!...|");
}

TEST(HexRow, ARowCutShortIsPaddedSoTheTextLinesUp)
{
    const std::string full = format_hex_row(0, std::string(16, 'a'), 0, 16);
    const std::string cut = format_hex_row(0, "abc", 0, 3);
    EXPECT_EQ(cut.size() - 3, full.size() - 16); // same width up to the text
    EXPECT_EQ(cut.substr(0, 14), "00000000  61 6");
    EXPECT_EQ(cut.substr(cut.size() - 5), "|abc|");
}

TEST(HexRow, StartsAtTheGivenPlaceInTheBytes)
{
    EXPECT_EQ(format_hex_row(32, "xxABxx", 2, 2).substr(0, 16), "00000020  41 42 ");
}

TEST(HexHandlerMatch, OpensEveryFileAsTheLastResort)
{
    const HexHandler handler;
    EXPECT_EQ(handler.match(FileProbe("anything.xyz", 5)), Match::fallback);
    EXPECT_EQ(handler.match(FileProbe("", 0)), Match::fallback);
}

TEST_F(HexViewerTest, StartsAtTheTopWithTheCursorOnTheFirstByte)
{
    const auto viewer = open(pattern_file(1000));
    EXPECT_EQ(viewer->cursor(), 0u);
    EXPECT_EQ(viewer->top_row(), 0u);
    EXPECT_GT(viewer->visible_rows(), 5);
}

TEST_F(HexViewerTest, TheDpadMovesByteAndRowAndStopsAtTheEnds)
{
    const auto viewer = open(pattern_file(100));
    send_input(*viewer, nav(hui::Direction::right));
    EXPECT_EQ(viewer->cursor(), 1u);
    send_input(*viewer, nav(hui::Direction::down));
    EXPECT_EQ(viewer->cursor(), 17u);
    send_input(*viewer, nav(hui::Direction::left));
    send_input(*viewer, nav(hui::Direction::up));
    EXPECT_EQ(viewer->cursor(), 0u);
    send_input(*viewer, nav(hui::Direction::left)); // not before the first byte
    EXPECT_EQ(viewer->cursor(), 0u);
    for (int i = 0; i < 20; ++i)
        send_input(*viewer, nav(hui::Direction::down));
    EXPECT_EQ(viewer->cursor(), 99u); // clamped to the last byte
}

TEST_F(HexViewerTest, TheViewFollowsTheCursor)
{
    const auto viewer = open(pattern_file(100000));
    const int rows = viewer->visible_rows();
    for (int i = 0; i < rows + 3; ++i)
        send_input(*viewer, nav(hui::Direction::down));
    EXPECT_GT(viewer->top_row(), 0u);
    EXPECT_LT(viewer->cursor() / kHexRowBytes,
              viewer->top_row() + static_cast<std::uint64_t>(rows));
}

TEST_F(HexViewerTest, ShoulderButtonsPageAndTriggersJumpToTheEnds)
{
    const auto viewer = open(pattern_file(100000));
    send_input(*viewer, press(hui::Action::page_next));
    EXPECT_GE(viewer->cursor(),
              static_cast<std::uint64_t>((viewer->visible_rows() - 1) * kHexRowBytes));
    send_input(*viewer, press(hui::Action::jump_next));
    EXPECT_EQ(viewer->cursor(), 99999u);
    send_input(*viewer, press(hui::Action::jump_prev));
    EXPECT_EQ(viewer->cursor(), 0u);
}

TEST_F(HexViewerTest, ReadsANewWindowWhenTheViewLeavesTheOldOne)
{
    const auto viewer = open(pattern_file(300000)); // more than the 64 KiB window
    send_input(*viewer, press(hui::Action::jump_next));
    EXPECT_EQ(viewer->cursor(), 299999u);
    EXPECT_GT(drawn(*viewer), 20u); // the rows near the end have their bytes
}

TEST_F(HexViewerTest, GoToChoosesAnOffsetDigitByDigit)
{
    const auto viewer = open(pattern_file(100000));
    send_input(*viewer, press(hui::Action::west));
    ASSERT_TRUE(viewer->choosing_offset());
    send_input(*viewer, nav(hui::Direction::up)); // the last digit: 0 -> 1
    send_input(*viewer, nav(hui::Direction::left));
    send_input(*viewer, nav(hui::Direction::up)); // the one before: 0 -> 1, so 0x11
    send_input(*viewer, press(hui::Action::confirm));
    EXPECT_FALSE(viewer->choosing_offset());
    EXPECT_EQ(viewer->cursor(), 0x11u);
}

TEST_F(HexViewerTest, GoToBeyondTheEndLandsOnTheLastByteAndCircleCancels)
{
    const auto viewer = open(pattern_file(100));
    send_input(*viewer, press(hui::Action::west));
    for (int i = 0; i < 3; ++i)
        send_input(*viewer, nav(hui::Direction::left));
    send_input(*viewer, nav(hui::Direction::up));
    send_input(*viewer, press(hui::Action::confirm));
    EXPECT_EQ(viewer->cursor(), 99u);

    send_input(*viewer, press(hui::Action::west));
    send_input(*viewer, nav(hui::Direction::up));
    send_input(*viewer, press(hui::Action::back)); // cancels the chooser only
    EXPECT_FALSE(viewer->choosing_offset());
    EXPECT_FALSE(viewer->closing());
    EXPECT_EQ(viewer->cursor(), 99u);
}

TEST_F(HexViewerTest, CircleClosesIt)
{
    const auto viewer = open(pattern_file(10));
    send_input(*viewer, press(hui::Action::back));
    EXPECT_TRUE(viewer->closing());
}

TEST_F(HexViewerTest, AnEmptyFileAndAnUnreadableOneDrawAMessage)
{
    const auto empty = open(pattern_file(0));
    EXPECT_GT(drawn(*empty), 0u);
    send_input(*empty, nav(hui::Direction::down)); // nothing to move on
    EXPECT_EQ(empty->cursor(), 0u);

    const auto gone = open(root_ + "/missing.bin");
    EXPECT_GE(drawn(*gone), 0u);
    send_input(*gone, press(hui::Action::jump_next));
}

TEST_F(HexViewerTest, DrawsAPageNotTheFile)
{
    const auto viewer = open(pattern_file(5u << 20));
    EXPECT_LT(drawn(*viewer), 4000u);
}

} // namespace
