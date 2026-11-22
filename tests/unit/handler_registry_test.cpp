// EliVier Explorer - Which handler opens which file: probes, match levels, the registry.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/handler.hpp"
#include "viewer/viewer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace hui::viewer;

// A handler that knows files by their extension.
class ByExtension final : public FileHandler
{
  public:
    ByExtension(std::string id, std::string ext, IconKind icon = IconKind::text)
        : id_(std::move(id)), ext_(std::move(ext)), icon_(icon)
    {
    }
    const char *id() const override
    {
        return id_.c_str();
    }
    const char *label() const override
    {
        return id_.c_str();
    }
    const char *verb() const override
    {
        return "View";
    }
    IconKind icon() const override
    {
        return icon_;
    }
    Match match(const FileProbe &probe) const override
    {
        return probe.ext() == ext_ ? Match::extension : Match::none;
    }
    std::unique_ptr<Viewer> open(ViewerContext &, const FileRef &) const override
    {
        return nullptr;
    }

  private:
    std::string id_;
    std::string ext_;
    IconKind icon_;
};

// A handler that sniffs: files starting with "MAGIC" are its own, whatever they are called.
class ByContent final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "magic";
    }
    const char *label() const override
    {
        return "Magic";
    }
    const char *verb() const override
    {
        return "View";
    }
    IconKind icon() const override
    {
        return IconKind::image;
    }
    Match match(const FileProbe &probe) const override
    {
        if (probe.ext() == "mgc")
            return Match::extension;
        if (probe.can_sniff() && probe.head().rfind("MAGIC", 0) == 0)
            return Match::content;
        return Match::none;
    }
    std::unique_ptr<Viewer> open(ViewerContext &, const FileRef &) const override
    {
        return nullptr;
    }
};

// Opens anything, badly: the last resort.
class Fallback final : public FileHandler
{
  public:
    const char *id() const override
    {
        return "hex";
    }
    const char *label() const override
    {
        return "Hex";
    }
    const char *verb() const override
    {
        return "Hex";
    }
    IconKind icon() const override
    {
        return IconKind::other;
    }
    Match match(const FileProbe &) const override
    {
        return Match::fallback;
    }
    std::unique_ptr<Viewer> open(ViewerContext &, const FileRef &) const override
    {
        return nullptr;
    }
};

TEST(FileProbe, TheExtensionIsTheLastPartInLowerCase)
{
    EXPECT_EQ(FileProbe("Photo.JPG", 1).ext(), "jpg");
    EXPECT_EQ(FileProbe("archive.tar.gz", 1).ext(), "gz");
    EXPECT_EQ(FileProbe("README", 1).ext(), "");
    EXPECT_EQ(FileProbe(".hidden", 1).ext(), ""); // a dot first is a name, not an extension
    EXPECT_EQ(FileProbe("trailing.", 1).ext(), "");
    EXPECT_EQ(FileProbe("a.b.c", 1).ext(), "c");
}

TEST(FileProbe, TheHeadIsReadOnceAndOnlyWhenAsked)
{
    int reads = 0;
    FileProbe probe("x.bin", 100,
                    [&reads]
                    {
                        ++reads;
                        return std::string("HEADER");
                    });
    EXPECT_EQ(reads, 0); // making a probe reads nothing
    EXPECT_TRUE(probe.can_sniff());
    EXPECT_EQ(probe.head(), "HEADER");
    EXPECT_EQ(probe.head(), "HEADER");
    EXPECT_EQ(reads, 1);
}

TEST(FileProbe, WithoutAReaderTheHeadIsEmptyAndNothingCanSniff)
{
    FileProbe probe("x.bin", 100);
    EXPECT_FALSE(probe.can_sniff());
    EXPECT_EQ(probe.head(), "");
}

TEST(HandlerRegistry, ThePreciserMatchWins)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<Fallback>());
    registry.add(std::make_unique<ByExtension>("text", "txt"));
    const FileProbe probe("notes.txt", 10);
    ASSERT_NE(registry.best(probe), nullptr);
    EXPECT_STREQ(registry.best(probe)->id(), "text"); // not the fallback registered first
}

TEST(HandlerRegistry, ContentBeatsExtension)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<ByExtension>("text", "txt"));
    registry.add(std::make_unique<ByContent>());
    // Called .txt, but it says what it is.
    const FileProbe probe("odd.txt", 10, [] { return std::string("MAGIC and more"); });
    EXPECT_STREQ(registry.best(probe)->id(), "magic");
}

TEST(HandlerRegistry, EqualMatchesGoToTheOneAddedFirst)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<ByExtension>("first", "dat"));
    registry.add(std::make_unique<ByExtension>("second", "dat"));
    EXPECT_STREQ(registry.best(FileProbe("a.dat", 1))->id(), "first");
}

TEST(HandlerRegistry, SniffingHappensOnlyWhenAHandlerAsksAndOnlyOnce)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<ByExtension>("text", "txt"));
    registry.add(std::make_unique<ByContent>());
    registry.add(std::make_unique<Fallback>());
    int reads = 0;
    const auto reader = [&reads]
    {
        ++reads;
        return std::string("plain");
    };
    // A .txt file: ByContent still looks (it might know better), but the head is read once.
    FileProbe known("a.txt", 5, reader);
    registry.best(known);
    registry.all(known);
    EXPECT_EQ(reads, 1);
    // The same call without a reader never reads, and still finds the extension match.
    FileProbe listing("a.txt", 5);
    EXPECT_STREQ(registry.best(listing)->id(), "text");
}

TEST(HandlerRegistry, AnUnknownExtensionIsFoundByItsContent)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<ByContent>());
    registry.add(std::make_unique<Fallback>());
    FileProbe probe("mystery.xyz", 20, [] { return std::string("MAGIC!"); });
    EXPECT_STREQ(registry.best(probe)->id(), "magic");
    FileProbe other("mystery.xyz", 20, [] { return std::string("nothing"); });
    EXPECT_STREQ(registry.best(other)->id(), "hex");
}

TEST(HandlerRegistry, AllListsEveryHandlerThatCanOpenItBestFirst)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<Fallback>());
    registry.add(std::make_unique<ByExtension>("text", "txt"));
    registry.add(std::make_unique<ByExtension>("other", "png"));
    const std::vector<const FileHandler *> all = registry.all(FileProbe("a.txt", 1));
    ASSERT_EQ(all.size(), 2u); // "other" does not know .txt
    EXPECT_STREQ(all[0]->id(), "text");
    EXPECT_STREQ(all[1]->id(), "hex");
}

TEST(HandlerRegistry, SupportedMeansMoreThanTheFallback)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<Fallback>());
    registry.add(std::make_unique<ByExtension>("text", "txt"));
    EXPECT_TRUE(registry.supported(FileProbe("a.txt", 1)));
    EXPECT_FALSE(registry.supported(FileProbe("a.bin", 1)));  // only hex can open it
    ASSERT_NE(registry.best(FileProbe("a.bin", 1)), nullptr); // but it can be opened
}

TEST(HandlerRegistry, NothingRegisteredOpensNothing)
{
    HandlerRegistry registry;
    EXPECT_EQ(registry.best(FileProbe("a.txt", 1)), nullptr);
    EXPECT_TRUE(registry.all(FileProbe("a.txt", 1)).empty());
    EXPECT_FALSE(registry.supported(FileProbe("a.txt", 1)));
}

TEST(HandlerRegistry, TheIconOfTheBestHandlerIsWhatTheListShows)
{
    HandlerRegistry registry;
    registry.add(std::make_unique<Fallback>());
    registry.add(std::make_unique<ByExtension>("image", "png", IconKind::image));
    EXPECT_EQ(registry.icon_for(FileProbe("a.png", 1)), IconKind::image);
    // No specific handler: the icon follows the extension table, as for any file.
    EXPECT_EQ(registry.icon_for(FileProbe("a.zip", 1)), IconKind::archive);
    EXPECT_EQ(registry.icon_for(FileProbe("README", 1)), IconKind::other);
}

} // namespace
