// EliVier Explorer - What the app can do with each drive: see it, list it, write it, how fast.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Probe screen runs this on the console and writes the outcome to a text file, so the
// file engine is built on measured facts. Plain POSIX; the tests run it on temporary folders.
//
//   std::vector<PlaceCheck> places = check_places("", kProbePlaces);
//   SpeedResult speed = measure_speed("/data/elivier-explorer", 100u << 20, 2u << 20);

#pragma once

#include "explorer/access_report.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hui::explorer
{

struct ProbePlace
{
    const char *path;
    const char *label;
    bool test_write = true; // false for places that must only be read (/system)
};

// The places the probe looks at, in screen order.
std::span<const ProbePlace> probe_places();

constexpr int kWriteSkipped = -1;

struct PlaceCheck
{
    std::string path;
    std::string label;
    bool exists = false;
    bool mounted = false; // another file system than its parent folder
    bool listable = false;
    int entries = 0;         // names in it, without . and ..
    int list_error = 0;      // errno when it could not be listed
    bool writable = false;   // a test file was made, read back and removed
    int write_error = 0;     // errno of the step that failed; kWriteSkipped when it was not tried
    std::string file_system; // "exfatfs", "bfs", "nullfs" ... (empty where unknown)
    std::uint64_t free_bytes = 0;
    std::uint64_t total_bytes = 0;
};

// Checks every place under root ("" on the console; a temporary folder in tests). Writing makes
// and removes one small file named ".elivier-probe" in each place.
std::vector<PlaceCheck> check_places(const std::string &root, std::span<const ProbePlace> places);

struct SpeedResult
{
    bool ok = false;
    int error = 0; // errno of the step that failed
    std::uint64_t bytes = 0;
    double write_mb_s = 0.0; // written and fsynced
    double read_mb_s = 0.0;  // read back (may come from the cache)
};

// Writes `bytes` to a file in folder in `chunk`-sized writes, fsyncs, reads it back, and removes
// it. The folder must exist.
SpeedResult measure_speed(const std::string &folder, std::uint64_t bytes, std::size_t chunk);

struct SpeedRun
{
    std::string folder;
    std::size_t chunk = 0;
    SpeedResult result;
};

// The whole outcome as text: the access steps, every place, every speed run.
std::string probe_report(const AccessReport &access, const std::vector<PlaceCheck> &places,
                         const std::vector<SpeedRun> &speeds);

} // namespace hui::explorer
