// EliVier Explorer - Where the explorer may change things: /data and the drives, nowhere else.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/guard.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::explorer::change_policy;
using hui::explorer::is_protected_folder;

class Guard : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        change_policy().clear_extra(); // the console's rules only
    }
    void TearDown() override
    {
        change_policy().clear_extra();
    }
};

TEST_F(Guard, DataAndTheDrivesMayBeChanged)
{
    for (const char *path :
         {"/data", "/data/homebrew", "/data/homebrew/PPSA01", "/data/", "/mnt/usb0",
          "/mnt/usb7/Games", "/mnt/ext0", "/mnt/ext1/user/app", "/mnt/usb12/x", "/mnt/ext10"})
        EXPECT_FALSE(is_protected_folder(path)) << path;
}

TEST_F(Guard, EverythingElseIsReadOnly)
{
    for (const char *path : {"/",
                             "/mnt",
                             "/mnt/",
                             "/user",
                             "/user/home/1000",
                             "/download0",
                             "/tmp",
                             "/var/log",
                             "/etc",
                             "/system",
                             "/system/common/lib",
                             "/system_data",
                             "/system_ex",
                             "/preinst",
                             "/boot",
                             "/dev",
                             "/app0",
                             "/hostapp",
                             "/some/new/folder",
                             "/home"})
        EXPECT_TRUE(is_protected_folder(path)) << path;
}

TEST_F(Guard, MountsThatAreNotDrivesAreReadOnly)
{
    for (const char *path : {"/mnt/sandbox", "/mnt/sandbox/PPSA01_000/data", "/mnt/disc",
                             "/mnt/original_m2", "/mnt/pfsmnt", "/mnt/shadowmnt/game"})
        EXPECT_TRUE(is_protected_folder(path)) << path;
}

TEST_F(Guard, ANameThatOnlyStartsLikeAnAllowedFolderIsNotInIt)
{
    for (const char *path : {"/database", "/data2", "/data-old", "/mnt/usb", "/mnt/usbx",
                             "/mnt/usb0x", "/mnt/ext", "/mnt/external", "/mnt/usb0a/y"})
        EXPECT_TRUE(is_protected_folder(path)) << path;
    EXPECT_FALSE(
        is_protected_folder("/data/system")); // a folder called system inside /data is fine
}

TEST_F(Guard, DotsAndSlashesCannotWalkOutOfWhereItIsAllowed)
{
    // Paths are built from listed names, but the rule must not depend on that.
    for (const char *path : {"/data/../system", "/data/../../etc", "/mnt/usb0/../../system",
                             "/data/./../user", "/data/a/../../user"})
        EXPECT_TRUE(is_protected_folder(path)) << path;
    EXPECT_FALSE(is_protected_folder("/data/a/../b")); // still inside /data
    EXPECT_FALSE(is_protected_folder("/data//homebrew"));
}

TEST_F(Guard, AnExtraRootIsAllowedUntilCleared)
{
    EXPECT_TRUE(is_protected_folder("/tmp/elivier-test"));
    change_policy().allow_also("/tmp/elivier-test");
    EXPECT_FALSE(is_protected_folder("/tmp/elivier-test"));
    EXPECT_FALSE(is_protected_folder("/tmp/elivier-test/sub"));
    EXPECT_TRUE(is_protected_folder("/tmp/elivier-test2"));
    change_policy().clear_extra();
    EXPECT_TRUE(is_protected_folder("/tmp/elivier-test"));
}

} // namespace
