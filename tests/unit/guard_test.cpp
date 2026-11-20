// EliVier Explorer - Which folders are never changed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/guard.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::explorer::is_protected_folder;

TEST(Guard, TheRootAndTheMountFolderAreProtected)
{
    EXPECT_TRUE(is_protected_folder("/"));
    EXPECT_TRUE(is_protected_folder("/mnt"));
    EXPECT_TRUE(is_protected_folder("/mnt/")); // a slash at the end is the same folder
}

TEST(Guard, SystemTreesAreProtectedAtAnyDepth)
{
    for (const char *path :
         {"/system", "/system/common/lib", "/system_data", "/system_data/priv", "/system_ex/app",
          "/preinst", "/preinst/common/font", "/preinst2", "/boot", "/dev/null", "/rescue"})
        EXPECT_TRUE(is_protected_folder(path)) << path;
}

TEST(Guard, MountsThatAreNotDrivesAreProtected)
{
    for (const char *path : {"/mnt/sandbox", "/mnt/sandbox/PPSA01_000/data", "/mnt/disc",
                             "/mnt/original_m2", "/mnt/pfsmnt"})
        EXPECT_TRUE(is_protected_folder(path)) << path;
}

TEST(Guard, DrivesAndTheUsersOwnFoldersAreOpen)
{
    for (const char *path :
         {"/data", "/data/homebrew/PPSA01", "/user", "/user/home", "/mnt/usb0", "/mnt/usb7/Games",
          "/mnt/ext0", "/mnt/ext1/user/app", "/download0", "/tmp/elivier-test"})
        EXPECT_FALSE(is_protected_folder(path)) << path;
}

TEST(Guard, ANameThatOnlyStartsLikeAProtectedTreeIsNot)
{
    EXPECT_FALSE(is_protected_folder("/systemfiles"));
    EXPECT_FALSE(is_protected_folder("/data/system"));
    EXPECT_FALSE(is_protected_folder("/devices"));
}

} // namespace
