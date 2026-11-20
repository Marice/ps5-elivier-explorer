// EliVier Explorer - Asking the HEN to free the app from its sandbox, so it sees every drive.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A title starts in its sandbox: /app0, /download0 and, with etaHEN's "Allow /data in sandbox",
// /data. USB drives, extended storage and /user are not there. A HEN frees a process on
// request; etaHEN, OnionHEN and Lapy-style daemons each take their own request:
//
//   hui::ps5::hen_prepare();                 // first thing in main, before any thread
//   hui::explorer::AccessReport report = hui::ps5::hen_free(4, "/data/homebrew/LAPY69432");
//
// Ported from Porpoise's porpoise_jailbreak.cpp (Copyright (C) 2026 Ruben, Project Porpoise,
// GPL-3.0-or-later), which ports PS5SX2's approach.

#pragma once

#include "explorer/access_report.hpp"

namespace hui::ps5
{

// Gives the process its own credential before any thread exists: the Lapy owned-root daemon
// only frees a process that did this. Harmless for etaHEN and OnionHEN.
void hen_prepare();

// Frees the app unless it sees the whole console already. Tries up to `rounds` times (each about
// three seconds when nobody answers): the request files in /download0, then the legacy command
// ports 9028 (etaHEN) and 9069. When freeing takes /app0 away, the app reads its files from
// install_folder (/data/homebrew/<TITLE_ID>); without that folder the sandbox root comes back.
explorer::AccessReport hen_free(int rounds, const char *install_folder);

} // namespace hui::ps5
