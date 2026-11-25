// EliVier Explorer - Shared pieces for the ZIP and RAR index parsers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Not part of ArchiveSource's public interface: archive_zip.cpp and archive_rar.cpp each fill an
// ArchiveBuild from their own format, and archive_source.cpp turns the finished one into an
// ArchiveSource. Kept out of archive_source.hpp so that nothing outside this module needs to know
// a tree of ArchiveNode is how it is done inside.

#pragma once

#include "explorer/archive_source.hpp"
#include "explorer/source.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hui::explorer
{

// A file is untrusted input: these bound how much of it is trusted before a format parser gives
// up (shared so archive_source.cpp can report the same numbers in a refusal message).
constexpr int kMaxArchiveEntries = 200000;
constexpr int kMaxArchiveVolumes = 300;
constexpr std::uint64_t kMaxCentralDirectoryBytes = 64u * 1024u * 1024u;
constexpr std::uint64_t kMaxEocdSearchBytes = 4u * 1024u * 1024u; // a ZIP comment can be this long

struct ArchiveNode
{
    std::string name;
    bool is_folder = false;
    std::uint64_t size = 0;
    std::int64_t modified_unix = 0;
    bool stored = false; // readable: no compression, no encryption
    std::vector<ArchiveSegment> segments;
    std::map<std::string, std::unique_ptr<ArchiveNode>> children; // folders only, by lower(name)
};

// What a format parser builds; archive_source.cpp turns one of these into an ArchiveSource.
struct ArchiveBuild
{
    std::vector<std::string> volumes; // full paths, in read order
    ArchiveNode root;
    const char *kind = "";
    int unreadable = 0; // entries found but not added: unknown compression, a damaged header
};

std::vector<std::string> archive_split_path(const std::string &path);

// Adds one entry at `path` (implying folders along the way); `node`'s name and children are set
// here, everything else must already be filled in. A later add() for the same path (a folder
// entry some formats also list explicitly, after a file under it already implied one, or the
// other way round) keeps whichever already carries more.
void archive_add(ArchiveBuild &build, const std::string &path, ArchiveNode node);

// Reads the archive's own list of volumes (the folder the given file sits in, through `parent`);
// null parsers never write to `parent`.
struct FormatReader
{
    Source &parent;
    std::string folder;   // the folder the archive's files sit in
    std::string name;     // the file name given to open(), without the folder
};

// Reads exactly `size` bytes at `offset` in `path`; false (nothing written) on any short read or
// error. Every format parser's bounds-checked way of reading a header.
bool read_exact(Source &parent, const std::string &path, std::uint64_t offset, void *buffer,
                std::size_t size);

// A file's bytes, starting at `start_offset` in `volumes[start_volume]`, split across however
// many of the following volumes it needs. `volume_sizes` is parallel to `volumes` (each one's
// real size, from Source::stat). Empty when it runs past the last volume: a truncated set.
std::vector<ArchiveSegment> span_volumes(int start_volume, std::uint64_t start_offset,
                                         std::uint64_t length,
                                         const std::vector<std::uint64_t> &volume_sizes);

// Fills `build` from a ZIP (central directory, ZIP64 extensions, old-style .zip/.z01/.z02 splits)
// starting from any one volume of the set. False and `error` filled on anything that stops this
// archive being read; true means build is ready to become an ArchiveSource.
bool read_zip(const FormatReader &reader, ArchiveBuild &build, std::string *error);

// Fills `build` from a RAR archive (RAR4 or RAR5, old .rar/.r00/.r01 or new .partNN.rar splits).
bool read_rar(const FormatReader &reader, ArchiveBuild &build, std::string *error);

} // namespace hui::explorer
