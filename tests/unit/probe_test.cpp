// EliVier Explorer - The probe: what the app can do with each drive, on temporary folders.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/access_report.hpp"
#include "explorer/probe.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using hui::explorer::AccessReport;
using hui::explorer::AccessRoute;
using hui::explorer::PlaceCheck;
using hui::explorer::ProbePlace;
using hui::explorer::SpeedRun;

class Probe : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = (fs::temp_directory_path() /
                 ("elivier-probe-" +
                  std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
                    .string();
        fs::remove_all(root_);
        fs::create_directories(root_ + "/data/homebrew");
        fs::create_directories(root_ + "/mnt/usb0");
        fs::create_directories(root_ + "/locked");
    }
    void TearDown() override
    {
        ::chmod((root_ + "/locked").c_str(), 0755);
        fs::remove_all(root_);
    }
    std::string root_;
};

TEST_F(Probe, ChecksWhatIsThereAndWhatIsNot)
{
    static constexpr ProbePlace kPlaces[] = {
        {"/data", "Internal"},
        {"/mnt/usb0", "USB 1"},
        {"/mnt/usb1", "USB 2"},
    };
    const std::vector<PlaceCheck> checks = hui::explorer::check_places(root_, kPlaces);
    ASSERT_EQ(checks.size(), 3u);
    EXPECT_TRUE(checks[0].exists);
    EXPECT_TRUE(checks[0].listable);
    EXPECT_EQ(checks[0].entries, 1); // homebrew; the probe file is gone again
    EXPECT_TRUE(checks[0].writable);
    EXPECT_FALSE(fs::exists(root_ + "/data/.elivier-probe"));
    // free_bytes comes from fstatvfs on the real file system the temp folder sits on: some space
    // is free and it fits inside the total, rather than any specific number.
    EXPECT_GT(checks[0].total_bytes, 0u);
    EXPECT_LE(checks[0].free_bytes, checks[0].total_bytes);
    EXPECT_TRUE(checks[1].writable);
    EXPECT_EQ(checks[1].entries, 0);
    EXPECT_FALSE(checks[2].exists);
    EXPECT_EQ(checks[2].label, "USB 2");
}

TEST_F(Probe, LeavesSpaceUnknownForATinyPlaceholderMount)
{
    // A mount point with nothing attached reports a few megabytes of its own (the root file
    // system's folder, not a drive): that must read as unknown, not as "nearly full".
    static constexpr ProbePlace kPlaces[] = {{"/mnt/usb0", "USB 1"}};
    const std::vector<PlaceCheck> checks = hui::explorer::check_places(root_, kPlaces);
    ASSERT_EQ(checks.size(), 1u);
    if (checks[0].total_bytes <= 16ull * 1024 * 1024)
    {
        EXPECT_EQ(checks[0].free_bytes, 0u);
    }
}

TEST_F(Probe, ReportsAFolderItCannotWrite)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "root writes everywhere on the PC";
    ::chmod((root_ + "/locked").c_str(), 0555);
    static constexpr ProbePlace kPlaces[] = {{"/locked", "Locked"}};
    const std::vector<PlaceCheck> checks = hui::explorer::check_places(root_, kPlaces);
    ASSERT_EQ(checks.size(), 1u);
    EXPECT_TRUE(checks[0].listable);
    EXPECT_FALSE(checks[0].writable);
    EXPECT_EQ(checks[0].write_error, EACCES);
}

TEST_F(Probe, LeavesAReadOnlyPlaceAlone)
{
    static constexpr ProbePlace kPlaces[] = {{"/data", "System (read-only)", false}};
    const std::vector<PlaceCheck> checks = hui::explorer::check_places(root_, kPlaces);
    ASSERT_EQ(checks.size(), 1u);
    EXPECT_TRUE(checks[0].listable);
    EXPECT_FALSE(checks[0].writable);
    EXPECT_EQ(checks[0].write_error, hui::explorer::kWriteSkipped);
    EXPECT_EQ(fs::directory_iterator(root_ + "/data") == fs::directory_iterator(), false);
    EXPECT_FALSE(fs::exists(root_ + "/data/.elivier-probe")); // nothing was tried
}

TEST_F(Probe, MeasuresSpeedAndLeavesNothingBehind)
{
    const hui::explorer::SpeedResult result =
        hui::explorer::measure_speed(root_ + "/data", 3u << 20, 1u << 20);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.bytes, 3u << 20);
    EXPECT_GT(result.write_mb_s, 0.0);
    EXPECT_GT(result.read_mb_s, 0.0);
    EXPECT_FALSE(fs::exists(root_ + "/data/.elivier-speed"));
}

TEST_F(Probe, SpeedInAMissingFolderFails)
{
    const hui::explorer::SpeedResult result =
        hui::explorer::measure_speed(root_ + "/nowhere", 1u << 20, 1u << 20);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, ENOENT);
}

TEST_F(Probe, ReportNamesTheRouteEveryPlaceAndEverySpeed)
{
    AccessReport access;
    access.route = AccessRoute::request_file;
    access.request_taken = "/download0/etahen_jailbreak";
    access.uid_before = 1;
    access.uid_after = 0;
    access.steps = {"sandboxed (uid 1); asking the HEN"};
    static constexpr ProbePlace kPlaces[] = {{"/data", "Internal"}, {"/mnt/usb1", "USB 2"}};
    std::vector<SpeedRun> speeds(1);
    speeds[0].folder = "/data";
    speeds[0].chunk = 2u << 20;
    speeds[0].result = hui::explorer::measure_speed(root_ + "/data", 1u << 20, 1u << 20);
    const std::string text =
        hui::explorer::probe_report(access, hui::explorer::check_places(root_, kPlaces), speeds);
    EXPECT_NE(text.find("access: request file /download0/etahen_jailbreak; uid 1 -> 0"),
              std::string::npos);
    EXPECT_NE(text.find("  sandboxed (uid 1); asking the HEN"), std::string::npos);
    EXPECT_NE(text.find("/data"), std::string::npos);
    EXPECT_NE(text.find("/mnt/usb1    not there"), std::string::npos);
    EXPECT_NE(text.find("chunk 2048 KiB: 1 MiB, write"), std::string::npos);
}

TEST(AccessReport, KeepsWhatTheAppSet)
{
    AccessReport report;
    report.route = AccessRoute::port_9028;
    report.uid_after = 0;
    hui::explorer::set_access_report(report);
    EXPECT_EQ(hui::explorer::access_report().route, AccessRoute::port_9028);
    EXPECT_STREQ(hui::explorer::route_name(AccessRoute::none), "still sandboxed");
    hui::explorer::set_access_report(AccessReport{});
}

} // namespace
