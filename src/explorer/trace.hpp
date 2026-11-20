// EliVier Explorer - A small trace file: what the app got through before it stopped.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console's klog is gone when nobody listens, and a crashed title's own storage is unmounted
// with it. The trace goes where a PC can read it afterwards, over FTP: the first of the folders
// that could be made. Each line is written and synced at once, so it survives the crash it
// describes. A new run appends to the file; past 64 KiB it starts over.
//
//   hui::explorer::trace_open(folders);          // first thing in main
//   hui::explorer::trace("display %dx%d", w, h);

#pragma once

#include <span>

namespace hui::explorer
{

// Opens the trace in the first folder that can hold it (made with mode 0777 when missing) and
// writes a "run" line. False when none can.
bool trace_open(std::span<const char *const> folders);

// Appends one line with the milliseconds since trace_open. A no-op before trace_open.
void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));

// The trace file's path, "" before trace_open or when none could be opened.
const char *trace_path();

} // namespace hui::explorer
