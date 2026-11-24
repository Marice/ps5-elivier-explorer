// EliVier Explorer - ArchiveSource reading ZIP fixtures through a LocalSource.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/archive_source.hpp"

#include "explorer/local_source.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>

namespace
{

using hui::explorer::ArchiveSource;
using hui::explorer::Entry;
using hui::explorer::EntryKind;
using hui::explorer::ListResult;
using hui::explorer::LocalSource;

const std::string kDir = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/archives";

std::string read_whole(const ArchiveSource &archive, const std::string &path, std::uint64_t size)
{
    std::string buffer(static_cast<std::size_t>(size), '\0');
    const std::int64_t got =
        archive.read_range(path, 0, buffer.data(), buffer.size());
    if (got < 0 || static_cast<std::uint64_t>(got) != size)
        return std::string();
    return buffer;
}

const Entry *find(const ListResult &result, const std::string &name)
{
    for (const Entry &entry : result.entries)
        if (entry.name == name)
            return &entry;
    return nullptr;
}

TEST(ArchiveSource, RefusesSomethingThatIsNotAnArchive)
{
    LocalSource local;
    std::string error;
    const auto archive = ArchiveSource::open(local, kDir + "/basic.zip.not-real", &error);
    EXPECT_EQ(archive, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(ArchiveSource, ListsNestedFoldersAndAnEmptyOne)
{
    LocalSource local;
    std::string error;
    const auto archive = ArchiveSource::open(local, kDir + "/basic.zip", &error);
    ASSERT_NE(archive, nullptr) << error;
    EXPECT_STREQ(archive->kind(), ".zip");
    EXPECT_EQ(archive->unreadable(), 0);

    const ListResult root = archive->list("/");
    ASSERT_TRUE(root.ok);
    const Entry *top = find(root, "top.txt");
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->kind, EntryKind::file);
    EXPECT_EQ(top->size, 15u);
    const Entry *folder = find(root, "folder");
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->kind, EntryKind::directory);

    const ListResult inside = archive->list("/folder");
    ASSERT_TRUE(inside.ok);
    EXPECT_NE(find(inside, "inside.txt"), nullptr);
    EXPECT_NE(find(inside, "nested"), nullptr);
    EXPECT_NE(find(inside, "empty"), nullptr); // implied purely by a zero-length entry under it

    const ListResult nested = archive->list("/folder/nested");
    ASSERT_TRUE(nested.ok);
    const Entry *deep = find(nested, "deep.txt");
    ASSERT_NE(deep, nullptr);
    EXPECT_EQ(deep->size, 17u);

    const ListResult empty = archive->list("/folder/empty");
    ASSERT_TRUE(empty.ok);
    EXPECT_EQ(empty.entries.size(), 1u); // the ".keep" file that implied the folder
}

TEST(ArchiveSource, ReadsAStoredFileByItsFullContent)
{
    LocalSource local;
    std::string error;
    const auto archive = ArchiveSource::open(local, kDir + "/basic.zip", &error);
    ASSERT_NE(archive, nullptr) << error;
    EXPECT_EQ(read_whole(*archive, "/top.txt", 15), "top-level file\n");
    EXPECT_EQ(read_whole(*archive, "/folder/nested/deep.txt", 17), "two folders deep\n");
}

TEST(ArchiveSource, StoredEntryReadsButDeflatedOneIsUnreadable)
{
    LocalSource local;
    std::string error;
    const auto archive = ArchiveSource::open(local, kDir + "/mixed.zip", &error);
    ASSERT_NE(archive, nullptr) << error;
    EXPECT_EQ(archive->unreadable(), 1);

    const ListResult root = archive->list("/");
    ASSERT_TRUE(root.ok);
    EXPECT_EQ(find(root, "stored.txt") != nullptr, true);
    // An unreadable (compressed) entry is left out of the listing entirely.
    EXPECT_EQ(find(root, "deflated.txt"), nullptr);

    EXPECT_EQ(read_whole(*archive, "/stored.txt", 19), "this one is stored\n");
    char buffer[8];
    EXPECT_EQ(archive->read_range("/deflated.txt", 0, buffer, sizeof(buffer)), -ENOENT);
}

TEST(ArchiveSource, ReadsAnEntryWrittenWithZip64Fields)
{
    LocalSource local;
    std::string error;
    const auto archive = ArchiveSource::open(local, kDir + "/zip64.zip", &error);
    ASSERT_NE(archive, nullptr) << error;
    EXPECT_EQ(archive->unreadable(), 0);
    EXPECT_EQ(read_whole(*archive, "/forced.txt", 43), "small file, but written with 64-bit fields\n");
}

TEST(ArchiveSource, ReadsAFileSplitAcrossTwoHandBuiltVolumes)
{
    LocalSource local;
    std::string error;
    // Opened from either volume: the set is found beside whichever one is named.
    const auto from_z01 = ArchiveSource::open(local, kDir + "/split.z01", &error);
    ASSERT_NE(from_z01, nullptr) << error;
    const auto from_zip = ArchiveSource::open(local, kDir + "/split.zip", &error);
    ASSERT_NE(from_zip, nullptr) << error;

    for (const auto *archive : {from_z01.get(), from_zip.get()})
    {
        EXPECT_EQ(archive->unreadable(), 0);
        const ListResult root = archive->list("/");
        ASSERT_TRUE(root.ok);
        EXPECT_NE(find(root, "a.txt"), nullptr); // entirely in split.z01 (disk 0)
        EXPECT_NE(find(root, "b.txt"), nullptr); // entirely in split.zip (disk 1)
        EXPECT_EQ(read_whole(*archive, "/a.txt", 28), "the first volume's own file\n");
        EXPECT_EQ(read_whole(*archive, "/b.txt", 29), "the second volume's own file\n");
    }
}

TEST(ArchiveSource, IsNeverPlainAndNeverTheSameDeviceAsAnything)
{
    LocalSource local;
    std::string error;
    const auto archive = ArchiveSource::open(local, kDir + "/basic.zip", &error);
    ASSERT_NE(archive, nullptr) << error;
    EXPECT_FALSE(archive->is_plain_path("/top.txt"));
    EXPECT_FALSE(archive->same_device("/top.txt", "/folder/inside.txt"));
}

TEST(ArchiveSource, WritesAreAllRefused)
{
    LocalSource local;
    std::string error;
    auto archive = ArchiveSource::open(local, kDir + "/basic.zip", &error);
    ASSERT_NE(archive, nullptr) << error;
    EXPECT_EQ(archive->make_directory("/new"), -EROFS);
    EXPECT_EQ(archive->rename("/top.txt", "/renamed.txt"), -EROFS);
    EXPECT_EQ(archive->remove_file("/top.txt"), -EROFS);
    EXPECT_EQ(archive->remove_directory("/folder/empty"), -EROFS);
    EXPECT_EQ(archive->copy_file("/top.txt", "/copy.txt", 4096, [](std::uint64_t) { return true; }),
             -EROFS);
}

} // namespace
