// EliVier Explorer - LocalSource on temporary folders.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/local_source.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using hui::explorer::Entry;
using hui::explorer::EntryKind;
using hui::explorer::ListResult;
using hui::explorer::LocalSource;

std::string read_all(const std::string &path)
{
    std::stringstream text;
    text << std::ifstream(path, std::ios::binary).rdbuf();
    return text.str();
}

void write_file(const std::string &path, const std::string &data)
{
    std::ofstream(path, std::ios::binary) << data;
}

class LocalSourceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = (fs::temp_directory_path() /
                 ("elivier-local-" +
                  std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
                    .string();
        fs::remove_all(root_);
        fs::create_directories(root_ + "/dir/inner");
        write_file(root_ + "/dir/a.txt", "hello");
        write_file(root_ + "/dir/empty.bin", "");
    }
    void TearDown() override
    {
        fs::remove_all(root_);
    }
    std::string root_;
    LocalSource source_;
};

TEST_F(LocalSourceTest, ListsEntriesWithKindSizeAndTime)
{
    const ListResult result = source_.list(root_ + "/dir");
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.entries.size(), 3u);
    const Entry *a = nullptr;
    const Entry *inner = nullptr;
    const Entry *empty = nullptr;
    for (const Entry &entry : result.entries)
    {
        if (entry.name == "a.txt")
            a = &entry;
        else if (entry.name == "inner")
            inner = &entry;
        else if (entry.name == "empty.bin")
            empty = &entry;
    }
    ASSERT_TRUE(a != nullptr && inner != nullptr && empty != nullptr);
    EXPECT_EQ(a->kind, EntryKind::file);
    EXPECT_EQ(a->size, 5u);
    EXPECT_GT(a->modified_unix, 0);
    EXPECT_EQ(inner->kind, EntryKind::directory);
    EXPECT_EQ(inner->size, 0u);
    EXPECT_EQ(empty->kind, EntryKind::file); // a 0-byte file is not a folder
    EXPECT_EQ(empty->size, 0u);
}

TEST_F(LocalSourceTest, ABrokenLinkIsListedAndDoesNotFailTheListing)
{
    fs::create_symlink(root_ + "/nowhere", root_ + "/dir/broken");
    const ListResult result = source_.list(root_ + "/dir");
    ASSERT_TRUE(result.ok) << result.error; // stat failed on one entry: not the folder's error
    ASSERT_EQ(result.entries.size(), 4u);
    bool found = false;
    for (const Entry &entry : result.entries)
    {
        if (entry.name == "broken")
        {
            found = true;
            EXPECT_EQ(entry.kind, EntryKind::other);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(LocalSourceTest, ListOfAMissingFolderFailsWithErrno)
{
    const ListResult result = source_.list(root_ + "/nowhere");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ENOENT);
    EXPECT_TRUE(result.entries.empty());
}

TEST_F(LocalSourceTest, StatsAFileAndAFolder)
{
    Entry entry;
    ASSERT_TRUE(source_.stat(root_ + "/dir/a.txt", &entry));
    EXPECT_EQ(entry.name, "a.txt");
    EXPECT_EQ(entry.kind, EntryKind::file);
    EXPECT_EQ(entry.size, 5u);
    ASSERT_TRUE(source_.stat(root_ + "/dir", &entry));
    EXPECT_EQ(entry.name, "dir");
    EXPECT_EQ(entry.kind, EntryKind::directory);
    EXPECT_FALSE(source_.stat(root_ + "/nowhere", &entry));
}

TEST_F(LocalSourceTest, SpaceIsKnownOnARealFileSystem)
{
    const hui::explorer::SpaceInfo space = source_.space(root_);
    ASSERT_TRUE(space.known);
    EXPECT_GT(space.total_bytes, 0u);
    EXPECT_LE(space.free_bytes, space.total_bytes);
    EXPECT_FALSE(source_.space(root_ + "/nowhere").known);
}

TEST_F(LocalSourceTest, MakesRenamesAndRemoves)
{
    EXPECT_EQ(source_.make_directory(root_ + "/new"), 0);
    EXPECT_TRUE(fs::is_directory(root_ + "/new"));
    EXPECT_EQ(fs::status(root_ + "/new").permissions() & fs::perms::all, fs::perms(0777));
    EXPECT_EQ(source_.make_directory(root_ + "/new"), EEXIST);
    EXPECT_EQ(source_.rename(root_ + "/dir/a.txt", root_ + "/new/b.txt"), 0);
    EXPECT_FALSE(fs::exists(root_ + "/dir/a.txt"));
    EXPECT_EQ(read_all(root_ + "/new/b.txt"), "hello");
    EXPECT_EQ(source_.remove_directory(root_ + "/new"), ENOTEMPTY);
    EXPECT_EQ(source_.remove_file(root_ + "/new/b.txt"), 0);
    EXPECT_EQ(source_.remove_directory(root_ + "/new"), 0);
    EXPECT_FALSE(fs::exists(root_ + "/new"));
    EXPECT_EQ(source_.remove_file(root_ + "/new/b.txt"), ENOENT);
}

TEST_F(LocalSourceTest, CopiesAcrossChunkBoundariesAndReportsProgress)
{
    std::string data(10 * 1024 + 37, 'x'); // not a multiple of the chunk
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>(i * 7 + 3);
    write_file(root_ + "/big.bin", data);
    std::vector<std::uint64_t> seen;
    const int result =
        source_.copy_file(root_ + "/big.bin", root_ + "/copy.bin", 4096,
                          [&seen](std::uint64_t copied)
                          {
                              seen.push_back(copied);
                              return true;
                          });
    EXPECT_EQ(result, 0);
    EXPECT_EQ(read_all(root_ + "/copy.bin"), data);
    EXPECT_EQ(fs::status(root_ + "/copy.bin").permissions() & fs::perms::all, fs::perms(0666));
    ASSERT_EQ(seen.size(), 3u); // 4096, 8192, 10277
    EXPECT_EQ(seen.front(), 4096u);
    EXPECT_EQ(seen.back(), data.size());
    for (std::size_t i = 1; i < seen.size(); ++i)
        EXPECT_GT(seen[i], seen[i - 1]);
}

TEST_F(LocalSourceTest, CancelStopsTheCopyAndRemovesThePartialFile)
{
    write_file(root_ + "/big.bin", std::string(20000, 'y'));
    int calls = 0;
    const int result = source_.copy_file(root_ + "/big.bin", root_ + "/copy.bin", 4096,
                                         [&calls](std::uint64_t) { return ++calls < 2; });
    EXPECT_EQ(result, ECANCELED);
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(fs::exists(root_ + "/copy.bin"));
    EXPECT_TRUE(fs::exists(root_ + "/big.bin")); // the source is never touched
}

TEST_F(LocalSourceTest, CopyOfAMissingSourceFailsAndLeavesNothing)
{
    const int result = source_.copy_file(root_ + "/nowhere.bin", root_ + "/copy.bin", 4096,
                                         [](std::uint64_t) { return true; });
    EXPECT_EQ(result, ENOENT);
    EXPECT_FALSE(fs::exists(root_ + "/copy.bin"));
}

TEST_F(LocalSourceTest, CopyOfAnEmptyFileMakesAnEmptyFile)
{
    const int result = source_.copy_file(root_ + "/dir/empty.bin", root_ + "/copy.bin", 4096,
                                         [](std::uint64_t) { return true; });
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(fs::exists(root_ + "/copy.bin"));
    EXPECT_EQ(fs::file_size(root_ + "/copy.bin"), 0u);
}

TEST_F(LocalSourceTest, TwoLocalSourcesAreTheSameKind)
{
    LocalSource other;
    EXPECT_TRUE(source_.same_kind(other));
    EXPECT_EQ(source_.display_name(), "Local");
}

} // namespace
