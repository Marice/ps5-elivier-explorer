// EliVier Explorer - The Files screen opens a file in the viewer its handler makes, and closes it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"

#include "concepts/concepts.hpp"
#include "viewer/handler.hpp"
#include "viewer/viewer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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
        std::ofstream(root_ + "/a.fake") << "fake content";
        std::ofstream(root_ + "/b.txt") << "plain";
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
