// EliVier Explorer - Copy, move and delete: a job that runs on the worker thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/copy_job.hpp"

#include "explorer/guard.hpp"
#include "explorer/path.hpp"
#include "explorer/trace.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

namespace hui::explorer
{

namespace
{

constexpr const char *kPartSuffix = ".elivier-part";
constexpr int kMaxRenameTries = 1000;

// A drive that is full or read-only fails every file the same way: stop instead of trying each.
bool is_fatal(int error)
{
    return error == ENOSPC || error == EROFS || error == EDQUOT;
}

class Engine
{
  public:
    Engine(Source &from, Source &to, const CopyPlan &plan, CopyProgress *progress,
           std::mutex *mutex, std::size_t chunk)
        : from_(from), to_(to), plan_(plan), progress_(progress), mutex_(mutex), chunk_(chunk)
    {
    }

    void run()
    {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            if (progress_->state == JobState::cancelled)
                return; // cancelled before it started
            progress_->state = JobState::running;
            progress_->kind = plan_.kind;
        }
        const auto started = std::chrono::steady_clock::now();
        trace("job: %s %zu name(s) %s -> %s",
              plan_.kind == JobKind::copy ? "copy"
                                          : (plan_.kind == JobKind::move ? "move" : "delete"),
              plan_.names.size(), plan_.from_dir.c_str(), plan_.to_dir.c_str());
        if (plan_.kind != JobKind::remove && !from_.same_kind(to_))
        {
            fail(ENOTSUP, plan_.to_dir);
            fatal_ = true;
        }
        // Reading and copying from a protected folder is fine; changing one is not (guard.hpp).
        if ((plan_.kind != JobKind::copy && is_protected_folder(plan_.from_dir)) ||
            (plan_.kind != JobKind::remove && is_protected_folder(plan_.to_dir)))
        {
            fail(EPERM, is_protected_folder(plan_.from_dir) ? plan_.from_dir : plan_.to_dir);
            fatal_ = true;
        }
        move_fast_ = plan_.kind == JobKind::move && from_.same_kind(to_) &&
                     from_.same_device(plan_.from_dir, plan_.to_dir);
        // The names asked for are counted at once; what is inside a folder as it is listed.
        for (const std::string &name : plan_.names)
        {
            Entry entry;
            if (from_.stat(path_join(plan_.from_dir, name), &entry) &&
                entry.kind == EntryKind::file)
                add_total(1, entry.size);
        }
        for (const std::string &name : plan_.names)
        {
            if (aborted())
                break;
            item(plan_.from_dir, plan_.to_dir, name);
        }
        std::lock_guard<std::mutex> lock(*mutex_);
        if (progress_->state != JobState::cancelled)
            progress_->state = fatal_ ? JobState::failed : JobState::done;
        progress_->current_name.clear();
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double mib = static_cast<double>(progress_->bytes_done) / (1024.0 * 1024.0);
        trace("job: end, state %d, %d done, %d skipped, %d failed; %.1f MiB in %.1f s = %.1f MiB/s",
              static_cast<int>(progress_->state), progress_->files_done, progress_->skipped,
              progress_->failed, mib, seconds, seconds > 0.05 ? mib / seconds : 0.0);
    }

  private:
    bool aborted()
    {
        if (fatal_)
            return true;
        std::lock_guard<std::mutex> lock(*mutex_);
        return progress_->state == JobState::cancelled;
    }

    void add_total(int files, std::uint64_t bytes)
    {
        std::lock_guard<std::mutex> lock(*mutex_);
        progress_->files_total += files;
        progress_->bytes_total += bytes;
    }

    void count_done(int files, std::uint64_t bytes)
    {
        std::lock_guard<std::mutex> lock(*mutex_);
        progress_->files_done += files;
        progress_->bytes_done += bytes;
    }

    void count_skipped()
    {
        std::lock_guard<std::mutex> lock(*mutex_);
        ++progress_->skipped;
    }

    void set_current(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(*mutex_);
        progress_->current_name = path;
    }

    void fail(int error, const std::string &path)
    {
        trace("job: failed %s: errno %d", path.c_str(), error);
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            ++progress_->failed;
            progress_->last_error = error;
            progress_->error_path = path;
        }
        if (is_fatal(error))
            fatal_ = true;
    }

    // What to do with a name that is already there. Ask blocks until the screen answers.
    ConflictPolicy resolve(const std::string &name)
    {
        if (plan_.policy != ConflictPolicy::ask)
            return plan_.policy;
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            progress_->state = JobState::waiting_conflict;
            progress_->conflict_name = name;
            progress_->pending_conflict = ConflictPolicy::ask;
        }
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            std::lock_guard<std::mutex> lock(*mutex_);
            if (progress_->state == JobState::cancelled)
                return ConflictPolicy::skip;
            if (progress_->pending_conflict != ConflictPolicy::ask)
            {
                const ConflictPolicy answer = progress_->pending_conflict;
                progress_->pending_conflict = ConflictPolicy::ask;
                progress_->state = JobState::running;
                progress_->conflict_name.clear();
                return answer;
            }
        }
    }

    // "a.txt" -> "a (2).txt", "pack" -> "pack (2)": the first that is free; "" when none is.
    std::string free_name(const std::string &dir, const std::string &name, bool is_dir)
    {
        std::string stem = name;
        std::string ext;
        const std::size_t dot = name.rfind('.');
        if (!is_dir && dot != std::string::npos && dot != 0)
        {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
        for (int n = 2; n < kMaxRenameTries; ++n)
        {
            const std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
            Entry unused;
            if (!to_.stat(path_join(dir, candidate), &unused))
                return candidate;
        }
        return std::string();
    }

    void item(const std::string &src_dir, const std::string &dst_dir, const std::string &name)
    {
        const std::string src = path_join(src_dir, name);
        Entry entry;
        if (!from_.stat(src, &entry))
        {
            fail(ENOENT, src);
            return;
        }
        if (plan_.kind == JobKind::remove)
        {
            remove_item(src, entry);
            return;
        }
        if (entry.kind == EntryKind::other)
        {
            count_skipped(); // a device or a broken link is not something to copy
            return;
        }
        const bool same_dir = path_normalized(src_dir) == path_normalized(dst_dir);
        if (same_dir && plan_.kind == JobKind::move)
            return; // already there
        if (entry.kind == EntryKind::directory && path_is_inside(dst_dir, src))
        {
            fail(EINVAL, src); // a folder into itself would never end
            return;
        }

        std::string dst = path_join(dst_dir, name);
        Entry there;
        bool exists = to_.stat(dst, &there);
        bool merge = false;
        if (exists)
        {
            if (entry.kind == EntryKind::directory && there.kind == EntryKind::directory &&
                !same_dir)
            {
                merge = true; // folders join; what is inside is judged name by name
            }
            else
            {
                // Copying into its own folder never replaces: it makes a numbered copy.
                const ConflictPolicy policy = same_dir ? ConflictPolicy::rename : resolve(name);
                if (aborted())
                    return;
                if (policy == ConflictPolicy::skip)
                {
                    count_skipped();
                    return;
                }
                if (policy == ConflictPolicy::overwrite)
                {
                    if (there.kind == EntryKind::directory || entry.kind == EntryKind::directory)
                    {
                        fail(EEXIST,
                             dst); // a folder is never replaced by a file, nor a file by one
                        return;
                    }
                }
                else // rename
                {
                    const std::string fresh =
                        free_name(dst_dir, name, entry.kind == EntryKind::directory);
                    if (fresh.empty())
                    {
                        fail(EEXIST, dst);
                        return;
                    }
                    dst = path_join(dst_dir, fresh);
                    exists = false;
                }
            }
        }

        if (entry.kind == EntryKind::directory)
            transfer_folder(src, dst, merge, exists);
        else
            transfer_file(src, dst, entry);
    }

    void transfer_folder(const std::string &src, const std::string &dst, bool merge, bool exists)
    {
        if (move_fast_ && !merge && !exists)
        {
            const int error = from_.rename(src, dst); // the whole folder in one step
            if (error == 0)
            {
                add_total(1, 0);
                count_done(1, 0);
                return;
            }
            if (error != EXDEV)
            {
                fail(error, src);
                return;
            }
        }
        if (!merge)
        {
            const int error = to_.make_directory(dst);
            if (error != 0)
            {
                fail(error, dst);
                return;
            }
        }
        ListResult listing = from_.list(src);
        if (!listing.ok)
        {
            fail(listing.error, src);
            return;
        }
        for (const Entry &child : listing.entries)
        {
            if (child.kind == EntryKind::file)
                add_total(1, child.size);
        }
        for (const Entry &child : listing.entries)
        {
            if (aborted())
                return;
            item(src, dst, child.name);
        }
        if (plan_.kind == JobKind::move && !aborted())
        {
            // Empty now unless something was skipped or failed: then it stays, and that is right.
            const int error = from_.remove_directory(src);
            if (error != 0 && error != ENOTEMPTY)
                fail(error, src);
        }
    }

    void transfer_file(const std::string &src, const std::string &dst, const Entry &entry)
    {
        set_current(src);
        if (move_fast_)
        {
            const int error = from_.rename(src, dst); // replaces a file that is there
            if (error == 0)
            {
                count_done(1, entry.size);
                return;
            }
            if (error != EXDEV)
            {
                fail(error, src);
                return;
            }
        }
        // Does it fit? Found out before the first byte, not after twenty gigabytes.
        const SpaceInfo space = to_.space(path_parent(dst));
        if (space.known && entry.size > space.free_bytes)
        {
            trace("job: %s needs %llu bytes, %llu free", src.c_str(),
                  static_cast<unsigned long long>(entry.size),
                  static_cast<unsigned long long>(space.free_bytes));
            fail(ENOSPC, src);
            return;
        }
        const std::string part = dst + kPartSuffix;
        std::uint64_t last = 0;
        int error = to_.copy_file(src, part, chunk_,
                                  [this, &last](std::uint64_t copied)
                                  {
                                      std::lock_guard<std::mutex> lock(*mutex_);
                                      progress_->bytes_done += copied - last;
                                      last = copied;
                                      return progress_->state != JobState::cancelled;
                                  });
        if (error != 0)
        {
            {
                std::lock_guard<std::mutex> lock(*mutex_);
                progress_->bytes_done -= last; // what it had counted of a file that did not arrive
            }
            if (error != ECANCELED)
                fail(error, src);
            return;
        }
        error = to_.rename(part, dst); // whole: now it gets its own name
        if (error != 0)
        {
            to_.remove_file(part);
            fail(error, dst);
            return;
        }
        if (plan_.kind == JobKind::move)
        {
            error = from_.remove_file(src); // only now that the copy is safe
            if (error != 0)
                fail(error, src);
        }
        count_done(1, 0); // the bytes were counted chunk by chunk
    }

    void remove_item(const std::string &path, const Entry &entry)
    {
        if (entry.kind == EntryKind::directory)
        {
            ListResult listing = from_.list(path);
            if (!listing.ok)
            {
                fail(listing.error, path);
                return;
            }
            for (const Entry &child : listing.entries)
            {
                if (child.kind != EntryKind::directory)
                    add_total(1, child.size);
            }
            for (const Entry &child : listing.entries)
            {
                if (aborted())
                    return;
                remove_item(path_join(path, child.name), child);
            }
            if (aborted())
                return;
            const int error = from_.remove_directory(path);
            if (error != 0)
                fail(error, path);
            return;
        }
        set_current(path);
        const int error = from_.remove_file(path);
        if (error != 0)
            fail(error, path);
        else
            count_done(1, entry.size);
    }

    Source &from_;
    Source &to_;
    const CopyPlan &plan_;
    CopyProgress *progress_;
    std::mutex *mutex_;
    std::size_t chunk_;
    bool move_fast_ = false;
    bool fatal_ = false; // only the job thread touches this
};

} // namespace

void run_copy_job(Source &from, Source &to, const CopyPlan &plan, CopyProgress *progress,
                  std::mutex *mutex, std::size_t chunk)
{
    Engine(from, to, plan, progress, mutex, chunk == 0 ? kCopyChunk : chunk).run();
}

void request_cancel(CopyProgress *progress, std::mutex *mutex)
{
    std::lock_guard<std::mutex> lock(*mutex);
    if (progress->state == JobState::queued || progress->state == JobState::running ||
        progress->state == JobState::waiting_conflict)
        progress->state = JobState::cancelled;
}

void answer_conflict(CopyProgress *progress, std::mutex *mutex, ConflictPolicy answer)
{
    if (answer == ConflictPolicy::ask)
        return;
    std::lock_guard<std::mutex> lock(*mutex);
    if (progress->state == JobState::waiting_conflict)
        progress->pending_conflict = answer;
}

} // namespace hui::explorer
