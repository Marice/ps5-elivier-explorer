// EliVier Explorer - The copy, move and delete engine, on temporary folders.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/copy_job.hpp"
#include "explorer/local_source.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace hui::explorer;

std::string read_all(const std::string &path)
{
    std::stringstream text;
    text << std::ifstream(path, std::ios::binary).rdbuf();
    return text.str();
}

void write_file(const std::string &path, const std::string &data)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << data;
}

// A LocalSource that can pretend: two paths are never one device (a move must copy), or a copy
// fails for the files a hook picks.
class ScriptedSource final : public Source
{
  public:
    bool one_device = true;
    std::function<int(const std::string &from, const std::string &to)> copy_hook;
    std::atomic<int> copies{0};

    std::string display_name() const override
    {
        return real_.display_name();
    }
    ListResult list(const std::string &p) const override
    {
        return real_.list(p);
    }
    bool stat(const std::string &p, Entry *e) const override
    {
        return real_.stat(p, e);
    }
    SpaceInfo space(const std::string &p) const override
    {
        return real_.space(p);
    }
    int make_directory(const std::string &p) override
    {
        return real_.make_directory(p);
    }
    int rename(const std::string &a, const std::string &b) override
    {
        return real_.rename(a, b);
    }
    int remove_file(const std::string &p) override
    {
        return real_.remove_file(p);
    }
    int remove_directory(const std::string &p) override
    {
        return real_.remove_directory(p);
    }
    int copy_file(const std::string &a, const std::string &b, std::size_t chunk,
                  const ProgressFn &on_progress) override
    {
        ++copies;
        if (copy_hook)
        {
            if (const int error = copy_hook(a, b))
                return error;
        }
        return real_.copy_file(a, b, chunk, on_progress);
    }
    bool same_device(const std::string &a, const std::string &b) const override
    {
        return one_device && real_.same_device(a, b);
    }
    const void *kind_tag() const override
    {
        return real_.kind_tag();
    }

  private:
    LocalSource real_;
};

class CopyJobTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = (fs::temp_directory_path() /
                 ("elivier-job-" +
                  std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
                    .string();
        fs::remove_all(root_);
        fs::create_directories(root_ + "/from");
        fs::create_directories(root_ + "/to");
    }
    void TearDown() override
    {
        fs::remove_all(root_);
    }

    // game/ (disc.bin, sub/deep.txt), notes.txt, empty.bin
    void make_tree()
    {
        write_file(root_ + "/from/game/disc.bin", std::string(3000, 'd'));
        write_file(root_ + "/from/game/sub/deep.txt", "deep");
        write_file(root_ + "/from/notes.txt", "notes");
        write_file(root_ + "/from/empty.bin", "");
    }

    CopyPlan plan(JobKind kind, std::vector<std::string> names,
                  ConflictPolicy policy = ConflictPolicy::ask)
    {
        CopyPlan p;
        p.kind = kind;
        p.names = std::move(names);
        p.from_dir = root_ + "/from";
        p.to_dir = root_ + "/to";
        p.policy = policy;
        return p;
    }

    CopyProgress run(Source &from, Source &to, const CopyPlan &p, std::size_t chunk = 1024)
    {
        CopyProgress progress;
        std::mutex mutex;
        run_copy_job(from, to, p, &progress, &mutex, chunk);
        return progress;
    }
    CopyProgress run(const CopyPlan &p)
    {
        return run(source_, source_, p);
    }

    static bool has_leftover(const std::string &folder)
    {
        for (const auto &entry : fs::recursive_directory_iterator(folder))
        {
            if (entry.path().string().find(".elivier-part") != std::string::npos)
                return true;
        }
        return false;
    }

    std::string root_;
    ScriptedSource source_;
};

TEST_F(CopyJobTest, CopiesAFolderTreeAndFilesAndLeavesTheOriginals)
{
    make_tree();
    const CopyProgress result = run(plan(JobKind::copy, {"game", "notes.txt", "empty.bin"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/to/game/disc.bin"), std::string(3000, 'd'));
    EXPECT_EQ(read_all(root_ + "/to/game/sub/deep.txt"), "deep");
    EXPECT_EQ(read_all(root_ + "/to/notes.txt"), "notes");
    EXPECT_TRUE(fs::exists(root_ + "/to/empty.bin"));
    EXPECT_TRUE(fs::exists(root_ + "/from/game/disc.bin")); // a copy keeps the original
    EXPECT_EQ(result.files_done, 4);
    EXPECT_EQ(result.files_total, 4);
    EXPECT_EQ(result.bytes_done, 3000u + 4u + 5u);
    EXPECT_EQ(result.bytes_total, result.bytes_done);
    EXPECT_EQ(result.failed, 0);
    EXPECT_FALSE(has_leftover(root_)); // no half-written file is ever left under its own name
}

TEST_F(CopyJobTest, AFileAppearsOnlyWhenItIsWhole)
{
    // The copy goes to a name ShadowMountPlus ignores and is renamed into place afterwards.
    write_file(root_ + "/from/big.bin", std::string(50000, 'b'));
    std::vector<std::string> seen_names;
    source_.copy_hook = [&](const std::string &, const std::string &to)
    {
        seen_names.push_back(to);
        return 0;
    };
    const CopyProgress result = run(plan(JobKind::copy, {"big.bin"}));
    EXPECT_EQ(result.state, JobState::done);
    ASSERT_EQ(seen_names.size(), 1u);
    EXPECT_EQ(seen_names[0], root_ + "/to/big.bin.elivier-part");
    EXPECT_TRUE(fs::exists(root_ + "/to/big.bin"));
    EXPECT_FALSE(fs::exists(root_ + "/to/big.bin.elivier-part"));
}

TEST_F(CopyJobTest, MovesWithARenameOnTheSameDevice)
{
    make_tree();
    const CopyProgress result = run(plan(JobKind::move, {"game", "notes.txt"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_FALSE(fs::exists(root_ + "/from/game"));
    EXPECT_FALSE(fs::exists(root_ + "/from/notes.txt"));
    EXPECT_EQ(read_all(root_ + "/to/game/sub/deep.txt"), "deep");
    EXPECT_EQ(source_.copies.load(), 0); // renamed, not copied
}

TEST_F(CopyJobTest, MovesByCopyingThenDeletingWhenThePathsAreOnDifferentDevices)
{
    make_tree();
    source_.one_device = false;
    const CopyProgress result = run(plan(JobKind::move, {"game", "notes.txt"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_GT(source_.copies.load(), 0);
    EXPECT_EQ(read_all(root_ + "/to/game/disc.bin"), std::string(3000, 'd'));
    EXPECT_EQ(read_all(root_ + "/to/notes.txt"), "notes");
    EXPECT_FALSE(fs::exists(root_ + "/from/game")); // emptied and removed
    EXPECT_FALSE(fs::exists(root_ + "/from/notes.txt"));
    EXPECT_TRUE(fs::exists(root_ + "/from/empty.bin")); // not asked for: untouched
    EXPECT_FALSE(has_leftover(root_));
}

TEST_F(CopyJobTest, AFailedCopyKeepsTheSourceOfAMove)
{
    write_file(root_ + "/from/keep.bin", "precious");
    source_.one_device = false;
    source_.copy_hook = [](const std::string &, const std::string &) { return EIO; };
    const CopyProgress result = run(plan(JobKind::move, {"keep.bin"}));
    EXPECT_EQ(result.failed, 1);
    EXPECT_EQ(result.last_error, EIO);
    EXPECT_EQ(read_all(root_ + "/from/keep.bin"), "precious"); // never deleted before it is safe
    EXPECT_FALSE(fs::exists(root_ + "/to/keep.bin"));
}

TEST_F(CopyJobTest, MergesIntoAFolderThatIsThere)
{
    make_tree();
    write_file(root_ + "/to/game/other.txt", "already here");
    const CopyProgress result = run(plan(JobKind::copy, {"game"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/to/game/other.txt"), "already here");
    EXPECT_EQ(read_all(root_ + "/to/game/disc.bin"), std::string(3000, 'd'));
}

TEST_F(CopyJobTest, ConflictSkipLeavesTheExistingFile)
{
    write_file(root_ + "/from/a.txt", "new");
    write_file(root_ + "/to/a.txt", "old");
    const CopyProgress result = run(plan(JobKind::copy, {"a.txt"}, ConflictPolicy::skip));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(result.skipped, 1);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "old");
}

TEST_F(CopyJobTest, ConflictOverwriteReplacesIt)
{
    write_file(root_ + "/from/a.txt", "new");
    write_file(root_ + "/to/a.txt", "old");
    const CopyProgress result = run(plan(JobKind::copy, {"a.txt"}, ConflictPolicy::overwrite));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "new");
}

TEST_F(CopyJobTest, AFailedOverwriteLeavesTheOldFileIntact)
{
    write_file(root_ + "/from/a.txt", "new");
    write_file(root_ + "/to/a.txt", "old");
    source_.copy_hook = [](const std::string &, const std::string &) { return EIO; };
    const CopyProgress result = run(plan(JobKind::copy, {"a.txt"}, ConflictPolicy::overwrite));
    EXPECT_EQ(result.failed, 1);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "old"); // the copy was to a part file, never over it
}

TEST_F(CopyJobTest, ConflictRenameKeepsBothWithANumber)
{
    write_file(root_ + "/from/a.txt", "new");
    write_file(root_ + "/to/a.txt", "old");
    write_file(root_ + "/to/a (2).txt", "older");
    const CopyProgress result = run(plan(JobKind::copy, {"a.txt"}, ConflictPolicy::rename));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "old");
    EXPECT_EQ(read_all(root_ + "/to/a (2).txt"), "older");
    EXPECT_EQ(read_all(root_ + "/to/a (3).txt"), "new");
}

TEST_F(CopyJobTest, ConflictRenameOfAFolderNumbersTheFolder)
{
    write_file(root_ + "/from/pack/x.txt", "x");
    write_file(root_ + "/to/pack", "a file in the way"); // a file where the folder would go
    const CopyProgress result = run(plan(JobKind::copy, {"pack"}, ConflictPolicy::rename));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/to/pack"), "a file in the way");
    EXPECT_EQ(read_all(root_ + "/to/pack (2)/x.txt"), "x");
}

TEST_F(CopyJobTest, OverwriteNeverReplacesAFolderWithAFile)
{
    write_file(root_ + "/from/pack", "a file");
    fs::create_directories(root_ + "/to/pack");
    write_file(root_ + "/to/pack/inner.txt", "inner");
    const CopyProgress result = run(plan(JobKind::copy, {"pack"}, ConflictPolicy::overwrite));
    EXPECT_EQ(result.failed, 1);
    EXPECT_TRUE(fs::is_directory(root_ + "/to/pack"));
    EXPECT_EQ(read_all(root_ + "/to/pack/inner.txt"), "inner");
}

TEST_F(CopyJobTest, AskWaitsForTheAnswerAndThenFollowsIt)
{
    write_file(root_ + "/from/a.txt", "new");
    write_file(root_ + "/from/b.txt", "bee");
    write_file(root_ + "/to/a.txt", "old");
    CopyProgress progress;
    std::mutex mutex;
    const CopyPlan p = plan(JobKind::copy, {"a.txt", "b.txt"}, ConflictPolicy::ask);
    std::thread worker([&] { run_copy_job(source_, source_, p, &progress, &mutex, 1024); });
    // The job stops at the name that is already there and says which.
    bool waiting = false;
    for (int i = 0; i < 500 && !waiting; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lock(mutex);
        waiting = progress.state == JobState::waiting_conflict;
        if (waiting)
            EXPECT_EQ(progress.conflict_name, "a.txt");
    }
    ASSERT_TRUE(waiting);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "old"); // nothing was decided for the player
    answer_conflict(&progress, &mutex, ConflictPolicy::overwrite);
    worker.join();
    EXPECT_EQ(progress.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "new");
    EXPECT_EQ(read_all(root_ + "/to/b.txt"), "bee");
}

TEST_F(CopyJobTest, CancellingWhileItWaitsForAnAnswerEndsTheJob)
{
    write_file(root_ + "/from/a.txt", "new");
    write_file(root_ + "/to/a.txt", "old");
    CopyProgress progress;
    std::mutex mutex;
    const CopyPlan p = plan(JobKind::copy, {"a.txt"}, ConflictPolicy::ask);
    std::thread worker([&] { run_copy_job(source_, source_, p, &progress, &mutex, 1024); });
    bool waiting = false;
    for (int i = 0; i < 500 && !waiting; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lock(mutex);
        waiting = progress.state == JobState::waiting_conflict;
    }
    ASSERT_TRUE(waiting);
    request_cancel(&progress, &mutex);
    worker.join();
    EXPECT_EQ(progress.state, JobState::cancelled);
    EXPECT_EQ(read_all(root_ + "/to/a.txt"), "old");
}

TEST_F(CopyJobTest, CopyingAFolderIntoItselfIsRefusedWithoutRecursing)
{
    make_tree();
    CopyPlan p = plan(JobKind::copy, {"game"});
    p.to_dir = root_ + "/from/game/sub";
    const CopyProgress result = run(p);
    EXPECT_EQ(result.failed, 1);
    EXPECT_EQ(result.last_error, EINVAL);
    EXPECT_FALSE(fs::exists(root_ + "/from/game/sub/game"));
}

TEST_F(CopyJobTest, CopyingIntoTheSameFolderMakesANumberedCopyAndNeverOverwrites)
{
    write_file(root_ + "/from/a.txt", "content");
    CopyPlan p = plan(JobKind::copy, {"a.txt"}, ConflictPolicy::overwrite);
    p.to_dir = p.from_dir;
    const CopyProgress result = run(p);
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/from/a.txt"), "content"); // the original is whole
    EXPECT_EQ(read_all(root_ + "/from/a (2).txt"), "content");
}

TEST_F(CopyJobTest, MovingIntoTheSameFolderChangesNothing)
{
    write_file(root_ + "/from/a.txt", "content");
    CopyPlan p = plan(JobKind::move, {"a.txt"});
    p.to_dir = p.from_dir;
    const CopyProgress result = run(p);
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(read_all(root_ + "/from/a.txt"), "content");
    EXPECT_EQ(result.failed, 0);
}

TEST_F(CopyJobTest, CancelStopsInTheMiddleOfAFileAndLeavesNoPartialFile)
{
    write_file(root_ + "/from/big.bin", std::string(2 * 1024 * 1024, 'z'));
    write_file(root_ + "/from/second.bin", "never copied");
    CopyProgress progress;
    std::mutex mutex;
    const CopyPlan p = plan(JobKind::copy, {"big.bin", "second.bin"});
    std::thread worker([&] { run_copy_job(source_, source_, p, &progress, &mutex, 4096); });
    for (int i = 0; i < 1000; ++i)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (progress.bytes_done > 0)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    request_cancel(&progress, &mutex);
    worker.join();
    EXPECT_EQ(progress.state, JobState::cancelled);
    EXPECT_FALSE(fs::exists(root_ + "/to/big.bin"));
    EXPECT_FALSE(fs::exists(root_ + "/to/second.bin"));
    EXPECT_FALSE(has_leftover(root_));
    EXPECT_TRUE(fs::exists(root_ + "/from/big.bin")); // the source is never touched
}

TEST_F(CopyJobTest, AFileThatCannotBeCopiedIsCountedAndTheRestContinues)
{
    write_file(root_ + "/from/bad.bin", "bad");
    write_file(root_ + "/from/good.bin", "good");
    source_.copy_hook = [](const std::string &from, const std::string &)
    { return from.find("bad.bin") != std::string::npos ? EACCES : 0; };
    const CopyProgress result = run(plan(JobKind::copy, {"bad.bin", "good.bin"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(result.failed, 1);
    EXPECT_EQ(result.last_error, EACCES);
    EXPECT_EQ(result.error_path, root_ + "/from/bad.bin");
    EXPECT_EQ(read_all(root_ + "/to/good.bin"), "good");
}

TEST_F(CopyJobTest, AFullDriveStopsTheWholeJob)
{
    write_file(root_ + "/from/a.bin", "a");
    write_file(root_ + "/from/b.bin", "b");
    source_.copy_hook = [](const std::string &, const std::string &) { return ENOSPC; };
    const CopyProgress result = run(plan(JobKind::copy, {"a.bin", "b.bin"}));
    EXPECT_EQ(result.state, JobState::failed);
    EXPECT_EQ(result.last_error, ENOSPC);
    EXPECT_EQ(source_.copies.load(), 1); // it did not go on to try the next one
}

TEST_F(CopyJobTest, DeletesFoldersAndFilesAndCountsThem)
{
    make_tree();
    const CopyProgress result = run(plan(JobKind::remove, {"game", "notes.txt"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_FALSE(fs::exists(root_ + "/from/game"));
    EXPECT_FALSE(fs::exists(root_ + "/from/notes.txt"));
    EXPECT_TRUE(fs::exists(root_ + "/from/empty.bin")); // not asked for
    EXPECT_EQ(result.files_done, 3); // disc.bin, deep.txt, notes.txt
}

TEST_F(CopyJobTest, ANameThatIsNotThereIsAFailureNotACrash)
{
    const CopyProgress result = run(plan(JobKind::copy, {"nothing.txt"}));
    EXPECT_EQ(result.state, JobState::done);
    EXPECT_EQ(result.failed, 1);
    EXPECT_EQ(result.last_error, ENOENT);
}

TEST_F(CopyJobTest, ACancelBeforeItStartsStopsItAtOnce)
{
    make_tree();
    CopyProgress progress;
    std::mutex mutex;
    request_cancel(&progress, &mutex);
    run_copy_job(source_, source_, plan(JobKind::copy, {"game"}), &progress, &mutex, 1024);
    EXPECT_EQ(progress.state, JobState::cancelled);
    EXPECT_FALSE(fs::exists(root_ + "/to/game"));
}

} // namespace
