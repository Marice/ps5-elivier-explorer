// EliVier Explorer - Copy, move and delete: a job that runs on the worker thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screen builds a CopyPlan from the marked names and queues a lambda on the JobWorker:
//
//   JobShared job;                 // progress + the mutex that guards it
//   worker.queue([&] { run_copy_job(source, source, plan, &job.progress, &job.mutex); });
//   ... every frame: lock job.mutex, copy job.progress out, draw the copy
//   ... on a conflict (state == waiting_conflict): answer_conflict(&job.progress, &job.mutex, ...)
//   ... to stop: request_cancel(&job.progress, &job.mutex)
//
// Rules the engine keeps, whatever goes wrong:
//  - A file is copied to "<name>.elivier-part" and renamed into place when whole, so a file under
//    its own name is always complete (ShadowMountPlus must never mount a half-copied game) and an
//    overwrite that fails leaves the old file untouched.
//  - A move deletes a source file only after its copy is whole; a rename is used instead on the
//    same device. A cancel or a failure never removes anything that was not copied.
//  - Nothing is replaced without being asked, unless the plan says so: a folder is never replaced
//    by a file or the other way round, and a folder is never copied into itself.
//  - No full scan first: the totals grow as folders are listed.

#pragma once

#include "explorer/source.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hui::explorer
{

constexpr std::size_t kCopyChunk = 2u << 20; // 2 MiB per read and write (D10)

enum class JobKind : std::uint8_t
{
    copy,
    move,
    remove,
};

enum class JobState : std::uint8_t
{
    queued,
    running,
    waiting_conflict, // a name is already there and the plan says ask: see conflict_name
    done,             // ran to the end (failed may still be above 0)
    failed,           // stopped for good: the drive is full or read-only
    cancelled,
};

struct CopyPlan
{
    JobKind kind = JobKind::copy;
    std::vector<std::string> names; // in from_dir
    std::string from_dir;
    std::string to_dir; // unused for remove
    ConflictPolicy policy = ConflictPolicy::ask;
};

// Written by the job thread, read by the screen: both under the job's mutex.
struct CopyProgress
{
    JobKind kind = JobKind::copy;
    JobState state = JobState::queued;
    std::string current_name;
    int files_done = 0;
    int files_total = 0; // what is known so far: it grows as folders are listed
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    int skipped = 0;
    int failed = 0;
    int last_error = 0; // errno of the last failure
    std::string error_path;
    std::string conflict_name; // while waiting_conflict: the name that is already there
    // The screen's answer to a conflict; the job takes it and puts it back to ask.
    ConflictPolicy pending_conflict = ConflictPolicy::ask;
};

// Progress and its mutex, together.
struct JobShared
{
    std::mutex mutex;
    CopyProgress progress;
};

// Runs the whole plan on the calling thread. `from` and `to` must be the same kind of source
// (copying between kinds needs streams the Source does not offer yet).
void run_copy_job(Source &from, Source &to, const CopyPlan &plan, CopyProgress *progress,
                  std::mutex *mutex, std::size_t chunk = kCopyChunk);

// Stops the job at the next chunk or name; also when it waits for an answer, or has not started.
void request_cancel(CopyProgress *progress, std::mutex *mutex);

// Answers a job in state waiting_conflict (skip, overwrite or rename); ask is ignored.
void answer_conflict(CopyProgress *progress, std::mutex *mutex, ConflictPolicy answer);

} // namespace hui::explorer
