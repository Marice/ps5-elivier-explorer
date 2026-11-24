// EliVier Explorer - A stored (uncompressed) ZIP or RAR set, read as a folder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the index of a ZIP or RAR archive once and answers Source calls over its virtual tree,
// without decompressing anything: only entries stored with no compression can be read, which a
// scene release's video (its own codec already compresses it) usually is. A compressed entry
// lists normally but fails to read, with a clear reason - unrar's licence does not fit this
// project's GPL-3.0-or-later, so there is no decompressor here, by design, not by oversight.
//
// A multi-volume set (old RAR4's .rar/.r00/.r01, RAR5's .part01.rar/.part02.rar, or ZIP's
// .zip/.z01/.z02) is opened from any one of its volumes: open() finds the others beside it
// through `parent`, in the order the format demands, and a file's bytes may span more than one.
//
//   auto archive = ArchiveSource::open(parent, "/data/Film.mkv.part01.rar", &error);
//   archive->list("/");                 // the top of the archive
//   archive->read_range("/Film.mkv", 0, buffer, size);

#pragma once

#include "explorer/source.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hui::explorer
{

// Where one stretch of a file's bytes live: which volume, and where in it. Stored (uncompressed)
// entries always decode to exactly their own bytes, so a read is a direct copy from here.
struct ArchiveSegment
{
    int volume = 0;         // index into Archive::volumes
    std::uint64_t offset = 0; // byte offset in that volume
    std::uint64_t length = 0;
};

class ArchiveSource final : public Source
{
  public:
    ~ArchiveSource() override;
    ArchiveSource(const ArchiveSource &) = delete;
    ArchiveSource &operator=(const ArchiveSource &) = delete;

    // `first_volume` is any one file of the set (the plain .zip/.rar, or one of its numbered
    // parts): the others are found beside it through `parent`. Null and `error` filled when the
    // file is not a recognised, readable archive (RAR5 with no part1 found, no index at all,
    // every entry damaged beyond use, more entries or volumes than kMaxEntries/kMaxVolumes
    // allow). Reading `parent` only; nothing here writes.
    static std::unique_ptr<ArchiveSource> open(Source &parent, const std::string &first_volume,
                                               std::string *error);

    // ".zip", ".rar": what kind this turned out to be, for a label or a trace line.
    const char *kind() const;
    // How many entries could not be read (unknown compression, a damaged header): 0 is a clean
    // archive. Those entries do not appear in list() or stat() at all.
    int unreadable() const;

    std::string display_name() const override;
    ListResult list(const std::string &path) const override;
    bool stat(const std::string &path, Entry *out) const override;
    SpaceInfo space(const std::string &path) const override;
    std::int64_t read_range(const std::string &path, std::uint64_t offset, void *buffer,
                            std::size_t size) const override;
    int make_directory(const std::string &path) override;
    int rename(const std::string &from, const std::string &to) override;
    int remove_file(const std::string &path) override;
    int remove_directory(const std::string &path) override;
    int copy_file(const std::string &from, const std::string &to, std::size_t chunk,
                  const ProgressFn &on_progress) override;
    bool is_plain_path(const std::string &path) const override;
    bool same_device(const std::string &a, const std::string &b) const override;
    const void *kind_tag() const override;

    struct Impl;

  private:
    explicit ArchiveSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::explorer
