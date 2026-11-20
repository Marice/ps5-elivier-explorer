// EliVier Explorer - What a pane shows: listing, sorting, selection, navigation.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/local_source.hpp"
#include "explorer/pane_model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using hui::explorer::EntryKind;
using hui::explorer::kPaneRowCap;
using hui::explorer::LocalSource;
using hui::explorer::PaneModel;
using hui::explorer::SortKey;

void write_file(const std::string &path, std::size_t size)
{
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
}

std::vector<std::string> names_of(const PaneModel &pane)
{
    std::vector<std::string> names;
    for (const auto &entry : pane.state().entries)
        names.push_back(entry.name);
    return names;
}

class PaneModelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = (fs::temp_directory_path() /
                 ("elivier-pane-" +
                  std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
                    .string();
        fs::remove_all(root_);
        fs::create_directories(root_ + "/beta");
        fs::create_directories(root_ + "/Alpha");
        write_file(root_ + "/file10.txt", 10);
        write_file(root_ + "/file2.txt", 300);
        write_file(root_ + "/File1.txt", 20);
        write_file(root_ + "/zero.bin", 0);
    }
    void TearDown() override
    {
        fs::remove_all(root_);
    }
    std::string root_;
    LocalSource source_;
};

TEST_F(PaneModelTest, ListsFoldersFirstThenByNaturalCaseInsensitiveName)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    EXPECT_EQ(pane.state().path, root_);
    EXPECT_EQ(names_of(pane), (std::vector<std::string>{"Alpha", "beta", "File1.txt", "file2.txt",
                                                        "file10.txt", "zero.bin"}));
    EXPECT_EQ(pane.state().total_entries, 6);
    EXPECT_FALSE(pane.state().truncated);
    EXPECT_TRUE(pane.state().space.known);
}

TEST_F(PaneModelTest, SortsBySizeDescendingKeepingFoldersFirst)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    pane.set_sort(SortKey::size, true);
    EXPECT_EQ(names_of(pane).front(), "Alpha"); // folders stay first, in name order
    EXPECT_EQ(names_of(pane)[1], "beta");
    EXPECT_EQ(names_of(pane)[2], "file2.txt"); // 300 bytes
    EXPECT_EQ(names_of(pane).back(), "zero.bin");
    pane.set_sort(SortKey::size, false);
    EXPECT_EQ(names_of(pane)[2], "zero.bin");
    EXPECT_EQ(names_of(pane).back(), "file2.txt");
}

TEST_F(PaneModelTest, FoldersCanSortWithTheFilesWhenAsked)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    pane.set_dirs_first(false);
    pane.set_sort(SortKey::size, true);
    EXPECT_EQ(names_of(pane).front(), "file2.txt");
}

TEST_F(PaneModelTest, EntersAFolderAndGoesUpOntoIt)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    EXPECT_FALSE(pane.enter("file2.txt")); // a file
    EXPECT_FALSE(pane.enter("nothing"));
    EXPECT_EQ(pane.state().path, root_);
    ASSERT_TRUE(pane.enter("beta"));
    EXPECT_EQ(pane.state().path, root_ + "/beta");
    EXPECT_TRUE(pane.state().entries.empty());
    ASSERT_TRUE(pane.go_up());
    EXPECT_EQ(pane.state().path, root_);
    ASSERT_NE(pane.focused(), nullptr);
    EXPECT_EQ(pane.focused()->name, "beta"); // back where it came from
}

TEST_F(PaneModelTest, GoUpStopsAtTheRoot)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to("/"));
    EXPECT_FALSE(pane.go_up());
    EXPECT_EQ(pane.state().path, "/");
}

TEST_F(PaneModelTest, AFolderThatCannotBeListedLeavesThePaneWhereItWas)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    EXPECT_FALSE(pane.go_to(root_ + "/nowhere"));
    EXPECT_EQ(pane.state().path, root_);
    EXPECT_NE(pane.state().list_error, 0);
    EXPECT_EQ(pane.state().total_entries, 6); // still what it showed
}

TEST_F(PaneModelTest, SelectionTogglesAndClears)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    pane.toggle_selected("file2.txt");
    pane.toggle_selected("beta");
    EXPECT_TRUE(pane.is_selected("file2.txt"));
    EXPECT_EQ(pane.state().selected.size(), 2u);
    pane.toggle_selected("file2.txt");
    EXPECT_FALSE(pane.is_selected("file2.txt"));
    pane.toggle_selected("nothing"); // not in this folder: ignored
    EXPECT_EQ(pane.state().selected.size(), 1u);
    pane.clear_selection();
    EXPECT_TRUE(pane.state().selected.empty());
}

TEST_F(PaneModelTest, SelectAllAndInvert)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    pane.select_all();
    EXPECT_EQ(pane.state().selected.size(), 6u);
    pane.toggle_selected("zero.bin");
    pane.invert_selection();
    EXPECT_EQ(pane.state().selected, (std::vector<std::string>{"zero.bin"}));
}

TEST_F(PaneModelTest, ReloadPicksUpChangesAndDropsVanishedSelections)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    pane.toggle_selected("file2.txt");
    pane.toggle_selected("zero.bin");
    write_file(root_ + "/new.txt", 5);
    fs::remove(root_ + "/file2.txt");
    pane.reload();
    EXPECT_EQ(pane.state().total_entries, 6);
    EXPECT_NE(std::find(names_of(pane).begin(), names_of(pane).end(), "new.txt"), names_of(pane).end());
    EXPECT_FALSE(pane.is_selected("file2.txt"));
    EXPECT_TRUE(pane.is_selected("zero.bin"));
}

TEST_F(PaneModelTest, ReloadKeepsTheFocusOnTheSameNameWhenRowsMove)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    pane.set_focus(3); // file2.txt
    ASSERT_EQ(pane.focused()->name, "file2.txt");
    write_file(root_ + "/aaa.txt", 1); // sorts before it: its index moves
    pane.reload();
    ASSERT_NE(pane.focused(), nullptr);
    EXPECT_EQ(pane.focused()->name, "file2.txt");
}

TEST_F(PaneModelTest, AFolderRemovedUnderThePaneSendsItUpToOneThatExists)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    ASSERT_TRUE(pane.enter("beta"));
    fs::remove_all(root_ + "/beta");
    pane.reload();
    EXPECT_EQ(pane.state().path, root_);
}

TEST_F(PaneModelTest, CapsTheRowsAndTheSelectionOfAHugeFolder)
{
    fs::create_directories(root_ + "/many");
    for (int i = 0; i < kPaneRowCap + 50; ++i)
        write_file(root_ + "/many/f" + std::to_string(i) + ".dat", 1);
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_ + "/many"));
    EXPECT_EQ(static_cast<int>(pane.state().entries.size()), kPaneRowCap);
    EXPECT_EQ(pane.state().total_entries, kPaneRowCap + 50);
    EXPECT_TRUE(pane.state().truncated);
    pane.select_all();
    EXPECT_EQ(static_cast<int>(pane.state().selected.size()), kPaneRowCap);
    // The cap keeps the first rows in sort order, not an arbitrary 2000.
    EXPECT_EQ(pane.state().entries.front().name, "f0.dat");
    EXPECT_EQ(pane.state().entries[10].name, "f10.dat"); // natural order: f2 < f10
}

TEST_F(PaneModelTest, MakesAFolderAndRenamesWithTheFocusFollowing)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    EXPECT_EQ(pane.make_folder("gamma"), 0);
    ASSERT_NE(pane.focused(), nullptr);
    EXPECT_EQ(pane.focused()->name, "gamma");
    EXPECT_EQ(pane.make_folder("gamma"), EEXIST);
    EXPECT_EQ(pane.rename_entry("gamma", "delta"), 0);
    EXPECT_EQ(pane.focused()->name, "delta");
    EXPECT_FALSE(fs::exists(root_ + "/gamma"));
    EXPECT_EQ(pane.rename_entry("delta", "beta"), EEXIST); // never over an existing name
    EXPECT_EQ(pane.rename_entry("delta", "a/b"), EINVAL);  // a name, not a path
    EXPECT_EQ(pane.make_folder(""), EINVAL);
    EXPECT_EQ(pane.make_folder(".."), EINVAL);
}

TEST_F(PaneModelTest, EntryKindsAreKept)
{
    PaneModel pane(source_);
    ASSERT_TRUE(pane.go_to(root_));
    EXPECT_EQ(pane.state().entries[0].kind, EntryKind::directory);
    EXPECT_EQ(pane.state().entries.back().kind, EntryKind::file);
}

} // namespace
