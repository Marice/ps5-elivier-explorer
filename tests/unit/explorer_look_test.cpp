// EliVier Explorer - A pane's error line: a daemon-missing hint, or the plain errno.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/explorer/look.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::concepts::explorer::pane_error_text;
using hui::concepts::explorer::speed_text;

TEST(PaneErrorText, NoErrorIsNotReached)
{
    // list_error == 0 is handled by the caller (the "sorted by" line instead); the sandboxed
    // branch never applies without an error to explain.
    EXPECT_EQ(pane_error_text(0, false), "Could not open that folder (error 0)");
}

TEST(PaneErrorText, AnOrdinaryErrorKeepsTheErrnoMessage)
{
    const std::string message = pane_error_text(13, false); // EACCES, but not from the sandbox
    EXPECT_NE(message.find("13"), std::string::npos);
    EXPECT_EQ(message.find("etaHEN"), std::string::npos);
}

TEST(PaneErrorText, StillSandboxedExplainsWhatToDo)
{
    const std::string message = pane_error_text(2, true);
    EXPECT_NE(message.find("etaHEN"), std::string::npos);
    EXPECT_NE(message.find("OnionHEN"), std::string::npos);
    EXPECT_NE(message.find("Lapy"), std::string::npos);
    EXPECT_EQ(message.find("error 2"), std::string::npos); // the real cause, not the errno
}

TEST(SpeedText, AlwaysTwoDecimalsAtEveryUnit)
{
    EXPECT_EQ(speed_text(0.0), "0.00 B/s");
    EXPECT_EQ(speed_text(512.0), "512.00 B/s");
    EXPECT_EQ(speed_text(1024.0), "1.00 KB/s");
    EXPECT_EQ(speed_text(1536.0), "1.50 KB/s");
    EXPECT_EQ(speed_text(1024.0 * 1024.0 * 12.5), "12.50 MB/s");
    EXPECT_EQ(speed_text(1024.0 * 1024.0 * 1024.0), "1.00 GB/s");
    // Never a negative or NaN-looking rate, whatever poll_job's own arithmetic hands it.
    EXPECT_EQ(speed_text(-5.0), "0.00 B/s");
}

} // namespace
