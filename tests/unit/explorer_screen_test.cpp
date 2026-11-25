// EliVier Explorer - The Files screen opens a file in the viewer its handler makes, and closes it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"

#include "concepts/concepts.hpp"
#include "explorer/guard.hpp"
#include "viewer/handler.hpp"
#include "viewer/viewer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

namespace fs = std::filesystem;
using hui::testing::ConceptFixture;
using namespace hui::viewer;

struct Seen
{
    int opened = 0;
    int updates = 0;
    int draws = 0;
    int closed = 0;
    std::string path;
    bool leave = false;
    // Set from a test, read by the next FakeViewer's update(): to be cleared by the test right
    // after one frame, or every later FakeViewer would keep asking to reopen it too.
    std::string reopen_target;
};
Seen g_seen;

class FakeViewer final : public Viewer
{
  public:
    std::string title() const override
    {
        return "Fake viewer";
    }
    void update(const hui::InputFrame &input, float, hui::ui::Feedback &) override
    {
        ++g_seen.updates;
        if (input.is_pressed(hui::Action::back))
            g_seen.leave = true;
    }
    void draw(hui::ui::Painter &paint, hui::ui::Canvas &, const hui::gfx::Rect &area) const override
    {
        ++g_seen.draws;
        paint.body("the fake file", area.x, area.y + 40.0f, 24.0f, paint.page_text());
    }
    std::span<const hui::ui::Hint> hints() const override
    {
        static constexpr hui::ui::Hint kHints[] = {{hui::ui::Button::circle, "Back"}};
        return kHints;
    }
    bool closing() const override
    {
        return g_seen.leave;
    }
    const std::string *reopen_as() const override
    {
        return g_seen.reopen_target.empty() ? nullptr : &g_seen.reopen_target;
    }
    void close() override
    {
        ++g_seen.closed;
    }
};

class FakeHandler final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "fake";
    }
    const char *label() const override
    {
        return "Fake";
    }
    const char *verb() const override
    {
        return "Fake it";
    }
    IconKind icon() const override
    {
        return IconKind::text;
    }
    Match match(const FileProbe &probe) const override
    {
        return probe.ext() == "fake" ? Match::extension : Match::none;
    }
    std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const override
    {
        EXPECT_NE(context.fonts, nullptr);
        EXPECT_NE(file.source, nullptr);
        ++g_seen.opened;
        g_seen.path = file.path;
        return std::make_unique<FakeViewer>();
    }
};

class ExplorerScreenTest : public ConceptFixture
{
  protected:
    void SetUp() override
    {
        ConceptFixture::SetUp();
        g_seen = Seen{};
        root_ = (fs::temp_directory_path() / "elivier-screen-test").string();
        fs::remove_all(root_);
        fs::create_directories(root_);
        hui::explorer::change_policy().allow_also(root_);
        std::ofstream(root_ + "/a.fake") << "fake content";
        std::ofstream(root_ + "/b.txt") << "plain";
        std::ofstream(root_ + "/c.fake") << "another fake file";
        // Sorts after the three above: every row-index-based test here still lands where it did.
        fs::copy_file(std::string(HUI_SOURCE_DIR) + "/tests/fixtures/archives/basic.zip",
                      root_ + "/d.zip");
        registry_.add(std::make_unique<FakeHandler>());
        hui::concepts::set_explorer_registry(&registry_);
        hui::concepts::set_explorer_start(root_, root_);
        screen_ = hui::concepts::make_explorer(context_);
        screen_->enter();
        send(*screen_, idle(), 0.5f);
    }
    void TearDown() override
    {
        screen_.reset();
        hui::concepts::set_explorer_registry(nullptr);
        hui::concepts::set_explorer_start("/", "/");
        hui::explorer::change_policy().clear_extra();
        fs::remove_all(root_);
        ConceptFixture::TearDown();
    }
    std::string root_;
    HandlerRegistry registry_;
    std::unique_ptr<hui::app::Concept> screen_;
};

TEST_F(ExplorerScreenTest, CrossOpensTheFileWithItsHandlerAndCircleClosesIt)
{
    expect_drawable(*screen_);
    send(*screen_, press(hui::Action::confirm)); // the first row is a.fake
    EXPECT_EQ(g_seen.opened, 1);
    EXPECT_EQ(g_seen.path, root_ + "/a.fake");
    expect_drawable(*screen_);
    EXPECT_GT(g_seen.draws, 0);
    const int updates = g_seen.updates;
    send(*screen_, idle(), 0.2f);
    EXPECT_GT(g_seen.updates, updates); // the viewer has the pad while it is open
    EXPECT_EQ(g_seen.closed, 0);
    send(*screen_, press(hui::Action::back)); // the viewer asks to leave
    EXPECT_EQ(g_seen.closed, 1);
    expect_drawable(*screen_); // back to the lists
    const int after = g_seen.updates;
    send(*screen_, idle(), 0.2f);
    EXPECT_EQ(g_seen.updates, after); // no longer fed
}

TEST_F(ExplorerScreenTest, AFileNobodyKnowsDoesNotOpenAnything)
{
    send(*screen_, nav(hui::Direction::down)); // b.txt
    send(*screen_, press(hui::Action::confirm));
    EXPECT_EQ(g_seen.opened, 0);
    expect_drawable(*screen_); // still the lists, with a note
}

TEST_F(ExplorerScreenTest, TheOptionsMenuOffersOpenAsForAFileTheHandlerKnows)
{
    send(*screen_, press(hui::Action::menu));
    expect_drawable(*screen_);
    send(*screen_, press(hui::Action::confirm)); // the first item: "Open as Fake"
    EXPECT_EQ(g_seen.opened, 1);
    EXPECT_EQ(g_seen.path, root_ + "/a.fake");
}

TEST_F(ExplorerScreenTest, AViewerThatAsksToReopenAsAnotherFileIsClosedAndReplaced)
{
    send(*screen_, press(hui::Action::confirm)); // opens a.fake
    ASSERT_EQ(g_seen.opened, 1);
    ASSERT_EQ(g_seen.path, root_ + "/a.fake");

    g_seen.reopen_target = root_ + "/c.fake"; // what the video viewer's "next episode" does
    send(*screen_, idle(), 0.0f);
    g_seen.reopen_target.clear(); // or the fresh viewer would ask to reopen it again next frame

    EXPECT_EQ(g_seen.closed, 1);  // the old viewer was closed, not just dropped
    EXPECT_EQ(g_seen.opened, 2);  // a new one was opened in its place
    EXPECT_EQ(g_seen.path, root_ + "/c.fake");
    expect_drawable(*screen_); // the new viewer, not the Files lists
}

TEST_F(ExplorerScreenTest, AskingToReopenAsAFileNobodyKnowsLeavesNoViewerAtAll)
{
    send(*screen_, press(hui::Action::confirm)); // opens a.fake
    ASSERT_EQ(g_seen.opened, 1);

    g_seen.reopen_target = root_ + "/b.txt"; // no handler for plain text in this registry
    send(*screen_, idle(), 0.0f);
    g_seen.reopen_target.clear();

    EXPECT_EQ(g_seen.closed, 1);
    EXPECT_EQ(g_seen.opened, 1); // the attempt failed: no second open
    expect_drawable(*screen_);   // back to the Files lists, not stuck on a closed viewer
}

TEST_F(ExplorerScreenTest, CrossOnAZipEntersItAndBackAtItsRootLeavesIt)
{
    send(*screen_, nav(hui::Direction::down), 0.0f); // b.txt
    send(*screen_, nav(hui::Direction::down), 0.0f); // c.fake
    send(*screen_, nav(hui::Direction::down), 0.0f); // d.zip
    send(*screen_, press(hui::Action::confirm));     // enters it: no handler is asked
    EXPECT_EQ(g_seen.opened, 0);
    expect_drawable(*screen_); // the archive's own rows, not a viewer

    // basic.zip's own content: top.txt, then the "folder" directory (dirs-first sorting puts it
    // first); neither has a handler for ".fake"/".txt", nor would one be looked for, since
    // viewing a file still inside an archive is out of this round's scope (D77/D86 follow-up).
    send(*screen_, press(hui::Action::confirm)); // "folder": a real subfolder inside the archive
    expect_drawable(*screen_);
    send(*screen_, press(hui::Action::back)); // out of "folder", still inside the archive
    expect_drawable(*screen_);

    send(*screen_, press(hui::Action::back)); // at the archive's root: leaves it
    expect_drawable(*screen_);
    EXPECT_EQ(g_seen.opened, 0); // nothing was ever opened as a viewer through any of this

    // Back on the real folder behaves normally again: one more Back does nothing (already at the
    // fixture's own root), proving the pane is truly back on the real Source, not still looking
    // like it succeeded inside the (by now gone) archive handle.
    send(*screen_, press(hui::Action::back));
    expect_drawable(*screen_);
}

TEST_F(ExplorerScreenTest, CopiesAFileOutOfAnOpenArchiveOntoTheOtherPane)
{
    send(*screen_, nav(hui::Direction::down), 0.0f); // b.txt
    send(*screen_, nav(hui::Direction::down), 0.0f); // c.fake
    send(*screen_, nav(hui::Direction::down), 0.0f); // d.zip
    send(*screen_, press(hui::Action::confirm));     // enters it
    expect_drawable(*screen_);

    // Inside basic.zip, dirs-first sorting puts "folder" before "top.txt": one more step down.
    send(*screen_, nav(hui::Direction::down), 0.0f); // top.txt
    send(*screen_, press(hui::Action::north));       // mark it
    send(*screen_, press(hui::Action::west), 0.0f);  // copy to the other pane (also root_ here)
    // The worker thread runs the job off the main thread, in real wall-clock time; send() only
    // advances simulated dt with no actual delay, so the loop needs a real sleep to give the OS a
    // chance to schedule that thread at all.
    for (int i = 0; i < 200 && !fs::exists(root_ + "/top.txt"); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        send(*screen_, idle(), 0.0f);
    }

    ASSERT_TRUE(fs::exists(root_ + "/top.txt"));
    std::stringstream copied;
    copied << std::ifstream(root_ + "/top.txt", std::ios::binary).rdbuf();
    EXPECT_EQ(copied.str(), "top-level file\n");
    expect_drawable(*screen_); // the screen survived the whole job, still the lists
}

TEST_F(ExplorerScreenTest, CrossOnADamagedArchiveToastsAndLeavesThePaneWhereItWas)
{
    std::ofstream(root_ + "/e.zip") << "not actually a zip file";
    hui::concepts::set_explorer_start(root_, root_);
    screen_ = hui::concepts::make_explorer(context_);
    screen_->enter();
    send(*screen_, idle(), 0.5f);
    for (int i = 0; i < 4; ++i) // a.fake, b.txt, c.fake, d.zip -> e.zip
        send(*screen_, nav(hui::Direction::down), 0.0f);
    send(*screen_, press(hui::Action::confirm));
    EXPECT_EQ(g_seen.opened, 0);
    expect_drawable(*screen_); // still the lists: the bad archive refused, nothing crashed
}

TEST_F(ExplorerScreenTest, ALongNameDrawsWhileItScrolls)
{
    screen_.reset();
    const std::string name = std::string(150, 'n') + ".fake";
    std::ofstream(root_ + "/" + name) << "x";
    hui::concepts::set_explorer_start(root_, root_);
    screen_ = hui::concepts::make_explorer(context_);
    screen_->enter();
    for (int i = 0; i < 6; ++i)
    {
        send(*screen_, idle(), 0.7f); // well past the one second before it starts
        expect_drawable(*screen_);
    }
}

} // namespace
