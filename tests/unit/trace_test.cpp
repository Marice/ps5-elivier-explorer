// EliVier Explorer - The trace file.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/trace.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace
{

namespace fs = std::filesystem;

std::string read_all(const std::string &path)
{
    std::stringstream text;
    text << std::ifstream(path).rdbuf();
    return text.str();
}

TEST(Trace, DoesNothingWithoutAFile)
{
    const char *const nowhere[] = {"/nonexistent-elivier/deeper"};
    hui::explorer::trace_open(nowhere);
    EXPECT_STREQ(hui::explorer::trace_path(), "");
    hui::explorer::trace("nobody listens"); // no file, no crash
}

TEST(Trace, WritesLinesToTheFirstFolderThatWorks)
{
    const fs::path root = fs::temp_directory_path() / "elivier-trace";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string missing = (root / "nowhere" / "deeper").string(); // mkdir cannot make this
    const std::string folder = (root / "here").string();
    const char *const folders[] = {missing.c_str(), folder.c_str()};
    ASSERT_TRUE(hui::explorer::trace_open(folders));
    EXPECT_EQ(std::string(hui::explorer::trace_path()), folder + "/trace.txt");
    hui::explorer::trace("display %dx%d", 1920, 1080);
    hui::explorer::trace("second");
    const std::string text = read_all(folder + "/trace.txt");
    EXPECT_NE(text.find("=== run, pid "), std::string::npos);
    EXPECT_NE(text.find("ms  display 1920x1080\n"), std::string::npos);
    EXPECT_NE(text.find("ms  second\n"), std::string::npos);
    EXPECT_EQ(fs::status(folder + "/trace.txt").permissions() & fs::perms::all, fs::perms(0666));
    fs::remove_all(root);
}

TEST(Trace, AppendsRunsAndStartsOverWhenItGrowsTooBig)
{
    const fs::path root = fs::temp_directory_path() / "elivier-trace-big";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string folder = root.string();
    const char *const folders[] = {folder.c_str()};
    ASSERT_TRUE(hui::explorer::trace_open(folders));
    hui::explorer::trace("first run");
    ASSERT_TRUE(hui::explorer::trace_open(folders));
    const std::string both = read_all(folder + "/trace.txt");
    EXPECT_NE(both.find("first run"), std::string::npos); // the next run appends
    std::ofstream(folder + "/trace.txt", std::ios::app) << std::string(70 * 1024, 'x') << '\n';
    ASSERT_TRUE(hui::explorer::trace_open(folders));
    const std::string fresh = read_all(folder + "/trace.txt");
    EXPECT_EQ(fresh.find("first run"), std::string::npos);
    EXPECT_LT(fresh.size(), 200u);
    fs::remove_all(root);
}

TEST(Trace, FailsWhenNoFolderWorks)
{
    const std::string missing = "/nonexistent-elivier/deeper";
    const char *const folders[] = {missing.c_str()};
    EXPECT_FALSE(hui::explorer::trace_open(folders));
    EXPECT_STREQ(hui::explorer::trace_path(), "");
}

} // namespace
