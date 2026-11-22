// EliVier Explorer - The text viewer and its handler: matching, reading, scrolling, drawing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"

#include "explorer/local_source.hpp"
#include "viewer/text_viewer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using hui::testing::ConceptFixture;
using namespace hui::viewer;

class TextViewerTest : public ConceptFixture
{
  protected:
    void SetUp() override
    {
        ConceptFixture::SetUp();
        root_ = (fs::temp_directory_path() / "elivier-textviewer").string();
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override
    {
        fs::remove_all(root_);
        ConceptFixture::TearDown();
    }

    std::string write(const std::string &name, const std::string &data)
    {
        const std::string path = root_ + "/" + name;
        std::ofstream(path, std::ios::binary) << data;
        return path;
    }

    // A viewer on a file of `lines` numbered lines, in an area that shows about 13 rows.
    std::unique_ptr<TextViewer> viewer_of_lines(int lines, int line_width = 10)
    {
        std::string text;
        for (int i = 1; i <= lines; ++i)
            text += "line " + std::to_string(i) +
                    std::string(static_cast<std::size_t>(line_width), 'x') + "\n";
        TextDocument document;
        document.load(text, false);
        ViewerContext context;
        context.area = {96.0f, 270.0f, 1000.0f, 450.0f};
        context.fonts = &fonts_;
        return std::make_unique<TextViewer>("test.txt", std::move(document), text.size(), context);
    }

    void send_input(Viewer &viewer, const hui::InputFrame &input)
    {
        hui::ui::Feedback feedback;
        viewer.update(input, 1.0f / 60.0f, feedback);
    }

    std::string root_;
    hui::explorer::LocalSource source_;
};

TEST(TextHandlerMatch, KnowsTextByItsExtensionAnyCase)
{
    const TextHandler handler;
    for (const char *name : {"a.txt", "B.JSON", "c.xml", "d.md", "e.log", "f.ini", "g.lua"})
        EXPECT_EQ(handler.match(FileProbe(name, 10)), Match::extension) << name;
    EXPECT_EQ(handler.match(FileProbe("a.png", 10)), Match::none);
    EXPECT_EQ(handler.match(FileProbe("noext", 10)), Match::none); // a listing cannot sniff
}

TEST(TextHandlerMatch, FindsUnknownFilesByTheirContent)
{
    const TextHandler handler;
    EXPECT_EQ(
        handler.match(FileProbe("README", 40, [] { return std::string("plain text\nmore\n"); })),
        Match::extension);
    EXPECT_EQ(
        handler.match(FileProbe("data.bin", 40, [] { return std::string("\x00\x01\x02\x03", 4); })),
        Match::none);
    EXPECT_EQ(handler.match(FileProbe("empty", 0, [] { return std::string(); })), Match::none);
}

TEST_F(TextViewerTest, OpensAFileAndReadsItsLines)
{
    const std::string path = write("hello.txt", "first\nsecond\r\nthird");
    hui::explorer::Entry entry;
    ASSERT_TRUE(source_.stat(path, &entry));
    ViewerContext context;
    context.fonts = &fonts_;
    FileRef file;
    file.source = &source_;
    file.path = path;
    file.entry = entry;
    const std::unique_ptr<Viewer> viewer = TextHandler().open(context, file);
    ASSERT_NE(viewer, nullptr);
    EXPECT_EQ(viewer->title(), "hello.txt");
    const auto *text = dynamic_cast<TextViewer *>(viewer.get());
    ASSERT_NE(text, nullptr);
    ASSERT_EQ(text->document().line_count(), 3);
    EXPECT_EQ(text->document().display_line(1), "second");
    EXPECT_FALSE(text->document().truncated());
}

TEST_F(TextViewerTest, ABigFileIsReadOnlyUpToTheCapAndSaysSo)
{
    const std::string path =
        write("big.txt", std::string(10000, 'a') + "\n" + std::string(10000, 'b'));
    hui::explorer::Entry entry;
    ASSERT_TRUE(source_.stat(path, &entry));
    ViewerContext context;
    context.fonts = &fonts_;
    FileRef file;
    file.source = &source_;
    file.path = path;
    file.entry = entry;
    const std::unique_ptr<Viewer> viewer = TextHandler(4096).open(context, file);
    ASSERT_NE(viewer, nullptr);
    const auto *text = dynamic_cast<TextViewer *>(viewer.get());
    EXPECT_TRUE(text->document().truncated());
    EXPECT_EQ(text->document().bytes(), 4096u);
}

TEST_F(TextViewerTest, AFileThatCannotBeReadDoesNotOpen)
{
    ViewerContext context;
    FileRef file;
    file.source = &source_;
    file.path = root_ + "/nowhere.txt";
    EXPECT_EQ(TextHandler().open(context, file), nullptr);
}

TEST_F(TextViewerTest, DownMovesALineAndStopsWhenTheLastLineIsAtTheBottom)
{
    const auto viewer = viewer_of_lines(100);
    ASSERT_GT(viewer->visible_rows(), 5);
    send_input(*viewer, nav(hui::Direction::down));
    EXPECT_EQ(viewer->top_line(), 1);
    send_input(*viewer, nav(hui::Direction::up));
    send_input(*viewer, nav(hui::Direction::up)); // already at the top
    EXPECT_EQ(viewer->top_line(), 0);
    for (int i = 0; i < 300; ++i)
        send_input(*viewer, nav(hui::Direction::down));
    EXPECT_EQ(viewer->top_line(), 100 - viewer->visible_rows());
}

TEST_F(TextViewerTest, PagesMoveOneScreenLessOneLine)
{
    const auto viewer = viewer_of_lines(200);
    const int page = viewer->visible_rows() - 1;
    send_input(*viewer, press(hui::Action::page_next));
    EXPECT_EQ(viewer->top_line(), page);
    send_input(*viewer, press(hui::Action::page_next));
    EXPECT_EQ(viewer->top_line(), 2 * page);
    send_input(*viewer, press(hui::Action::page_prev));
    EXPECT_EQ(viewer->top_line(), page);
}

TEST_F(TextViewerTest, L2GoesToTheTopAndR2ToTheEnd)
{
    const auto viewer = viewer_of_lines(200);
    send_input(*viewer, press(hui::Action::jump_next));
    EXPECT_EQ(viewer->top_line(), 200 - viewer->visible_rows());
    send_input(*viewer, nav(hui::Direction::right));
    send_input(*viewer, press(hui::Action::jump_prev));
    EXPECT_EQ(viewer->top_line(), 0);
    EXPECT_EQ(viewer->left_column(), 0);
}

TEST_F(TextViewerTest, LeftAndRightMoveByColumnsAndStopAtTheLongestLine)
{
    const auto viewer = viewer_of_lines(5, 400); // lines of about 410 columns
    send_input(*viewer, nav(hui::Direction::right));
    EXPECT_EQ(viewer->left_column(), 4);
    send_input(*viewer, nav(hui::Direction::left));
    send_input(*viewer, nav(hui::Direction::left));
    EXPECT_EQ(viewer->left_column(), 0);
    for (int i = 0; i < 500; ++i)
        send_input(*viewer, nav(hui::Direction::right));
    EXPECT_EQ(viewer->left_column(), viewer->document().max_columns() - viewer->visible_columns());
}

TEST_F(TextViewerTest, ShortLinesDoNotScrollSideways)
{
    const auto viewer = viewer_of_lines(5, 2);
    send_input(*viewer, nav(hui::Direction::right));
    EXPECT_EQ(viewer->left_column(), 0);
}

TEST_F(TextViewerTest, CircleAsksToClose)
{
    const auto viewer = viewer_of_lines(5);
    EXPECT_FALSE(viewer->closing());
    send_input(*viewer, press(hui::Action::back));
    EXPECT_TRUE(viewer->closing());
}

TEST_F(TextViewerTest, TheStickScrollsSteadilyAndTinyTiltsAddUp)
{
    const auto viewer = viewer_of_lines(500);
    hui::InputFrame tilt;
    tilt.stick2_y = 1.0f;
    hui::ui::Feedback feedback;
    for (int i = 0; i < 60; ++i) // one second at full tilt: about 30 lines
        viewer->update(tilt, 1.0f / 60.0f, feedback);
    EXPECT_NEAR(viewer->top_line(), 30, 2);
}

TEST_F(TextViewerTest, OnlyTheVisibleLinesAreDrawnWhateverTheFileSize)
{
    const auto viewer = viewer_of_lines(100000);
    hui::gfx::DrawList list;
    hui::ui::Painter paint(list, fonts_, hui::ui::default_theme());
    hui::ui::Canvas canvas{list, fonts_, 0, 0.0f};
    viewer->draw(paint, canvas, {96.0f, 270.0f, 1000.0f, 450.0f});
    EXPECT_GT(list.instances().size(), 10u);
    EXPECT_LT(list.instances().size(), 3000u); // a page of lines, not a hundred thousand
    for (const hui::gfx::Instance &instance : list.instances())
        for (const float value : instance.rect)
            ASSERT_TRUE(std::isfinite(value));
}

TEST_F(TextViewerTest, AnEmptyFileDrawsAMessageInsteadOfLines)
{
    TextDocument document;
    document.load("", false);
    ViewerContext context;
    context.fonts = &fonts_;
    TextViewer viewer("empty.txt", std::move(document), 0, context);
    hui::gfx::DrawList list;
    hui::ui::Painter paint(list, fonts_, hui::ui::default_theme());
    hui::ui::Canvas canvas{list, fonts_, 0, 0.0f};
    viewer.draw(paint, canvas, {96.0f, 270.0f, 1000.0f, 450.0f});
    EXPECT_FALSE(list.empty());
}

} // namespace
