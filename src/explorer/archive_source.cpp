// EliVier Explorer - A stored (uncompressed) ZIP or RAR set, read as a folder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/archive_source.hpp"

#include "explorer/archive_formats.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace hui::explorer
{

namespace
{

std::string lower(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string name_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string folder_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

const ArchiveNode *find(const ArchiveNode &root, const std::string &path)
{
    const ArchiveNode *at = &root;
    for (const std::string &part : archive_split_path(path))
    {
        if (!at->is_folder)
            return nullptr;
        const auto found = at->children.find(lower(part));
        if (found == at->children.end())
            return nullptr;
        at = found->second.get();
    }
    return at;
}

Entry to_entry(const ArchiveNode &node)
{
    Entry entry;
    entry.name = node.name;
    entry.kind = node.is_folder ? EntryKind::directory : EntryKind::file;
    entry.size = node.size;
    entry.modified_unix = node.modified_unix;
    return entry;
}

} // namespace

std::vector<std::string> archive_split_path(const std::string &path)
{
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at < path.size())
    {
        if (path[at] == '/')
        {
            ++at;
            continue;
        }
        const std::size_t next = path.find('/', at);
        parts.push_back(path.substr(at, next == std::string::npos ? next : next - at));
        at = next == std::string::npos ? path.size() : next;
    }
    return parts;
}

void archive_add(ArchiveBuild &build, const std::string &path, ArchiveNode node)
{
    const std::vector<std::string> parts = archive_split_path(path);
    if (parts.empty())
        return;
    ArchiveNode *at = &build.root;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
    {
        auto &child = at->children[lower(parts[i])];
        if (!child)
        {
            child = std::make_unique<ArchiveNode>();
            child->name = parts[i];
            child->is_folder = true;
        }
        else if (!child->is_folder)
            return; // a file already sits where this entry needs a folder: a malformed index
        at = child.get();
    }
    node.name = parts.back();
    auto &slot = at->children[lower(parts.back())];
    if (!slot || (slot->is_folder && slot->segments.empty() && !node.is_folder))
        slot = std::make_unique<ArchiveNode>(std::move(node));
}

bool read_exact(Source &parent, const std::string &path, std::uint64_t offset, void *buffer,
                std::size_t size)
{
    auto *out = static_cast<std::uint8_t *>(buffer);
    std::size_t done = 0;
    while (done < size)
    {
        const std::int64_t got =
            parent.read_range(path, offset + done, out + done, size - done);
        if (got <= 0)
            return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}

std::vector<ArchiveSegment> span_volumes(int start_volume, std::uint64_t start_offset,
                                         std::uint64_t length,
                                         const std::vector<std::uint64_t> &volume_sizes)
{
    std::vector<ArchiveSegment> segments;
    int volume = start_volume;
    std::uint64_t offset = start_offset;
    std::uint64_t left = length;
    while (left > 0)
    {
        if (volume < 0 || static_cast<std::size_t>(volume) >= volume_sizes.size())
            return {}; // the set is shorter than this file claims: truncated or damaged
        const std::uint64_t size_here = volume_sizes[static_cast<std::size_t>(volume)];
        if (offset >= size_here)
            return {};
        const std::uint64_t here = std::min(left, size_here - offset);
        segments.push_back({volume, offset, here});
        left -= here;
        ++volume;
        offset = 0;
    }
    return segments;
}

struct ArchiveSource::Impl
{
    Source *parent = nullptr;
    ArchiveBuild build;
};

ArchiveSource::ArchiveSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

ArchiveSource::~ArchiveSource() = default;

std::unique_ptr<ArchiveSource> ArchiveSource::open(Source &parent,
                                                   const std::string &first_volume,
                                                   std::string *error)
{
    const auto fail = [error](const std::string &message) -> std::unique_ptr<ArchiveSource> {
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    unsigned char head[8] = {};
    const std::int64_t got = parent.read_range(first_volume, 0, head, sizeof(head));
    if (got < 4)
        return fail("Cannot read this file");
    const FormatReader reader{parent, folder_of(first_volume), name_of(first_volume)};
    auto impl = std::make_unique<Impl>();
    impl->parent = &parent;
    impl->build.root.is_folder = true;
    bool ok = false;
    if (head[0] == 'P' && head[1] == 'K' && (head[2] == 3 || head[2] == 5 || head[2] == 7))
    {
        impl->build.kind = ".zip";
        ok = read_zip(reader, impl->build, error);
    }
    else if (got >= 7 && std::memcmp(head, "Rar!\x1a\x07", 6) == 0)
    {
        impl->build.kind = ".rar";
        ok = read_rar(reader, impl->build, error);
    }
    else
    {
        return fail("Not a ZIP or RAR archive");
    }
    if (!ok)
        return nullptr; // *error* was filled by the format reader
    return std::unique_ptr<ArchiveSource>(new ArchiveSource(std::move(impl)));
}

const char *ArchiveSource::kind() const
{
    return impl_->build.kind;
}

int ArchiveSource::unreadable() const
{
    return impl_->build.unreadable;
}

std::string ArchiveSource::display_name() const
{
    return impl_->build.volumes.empty() ? "Archive" : name_of(impl_->build.volumes.front());
}

ListResult ArchiveSource::list(const std::string &path) const
{
    ListResult result;
    const ArchiveNode *folder = find(impl_->build.root, path);
    if (folder == nullptr || !folder->is_folder)
    {
        result.error = ENOENT;
        return result;
    }
    result.entries.reserve(folder->children.size());
    for (const auto &item : folder->children)
        result.entries.push_back(to_entry(*item.second));
    std::sort(result.entries.begin(), result.entries.end(),
             [](const Entry &a, const Entry &b) { return lower(a.name) < lower(b.name); });
    result.ok = true;
    return result;
}

bool ArchiveSource::stat(const std::string &path, Entry *out) const
{
    if (path.empty() || path == "/")
    {
        *out = Entry{};
        out->name = display_name();
        out->kind = EntryKind::directory;
        return true;
    }
    const ArchiveNode *node = find(impl_->build.root, path);
    if (node == nullptr)
        return false;
    *out = to_entry(*node);
    out->name = name_of(path);
    return true;
}

SpaceInfo ArchiveSource::space(const std::string & /*path*/) const
{
    return SpaceInfo{};
}

std::int64_t ArchiveSource::read_range(const std::string &path, std::uint64_t offset,
                                       void *buffer, std::size_t size) const
{
    const ArchiveNode *node = find(impl_->build.root, path);
    if (node == nullptr || node->is_folder)
        return -ENOENT;
    if (!node->stored)
        return -ENOTSUP; // compressed or encrypted: this project does not decompress (GPL/unrar)
    if (offset >= node->size)
        return 0;
    size = std::min<std::size_t>(size, static_cast<std::size_t>(node->size - offset));
    std::size_t done = 0;
    auto *out = static_cast<std::uint8_t *>(buffer);
    std::uint64_t at = 0;
    for (const ArchiveSegment &segment : node->segments)
    {
        if (done >= size)
            break;
        if (offset >= at + segment.length)
        {
            at += segment.length;
            continue;
        }
        const std::uint64_t within = offset > at ? offset - at : 0;
        const std::uint64_t available = segment.length - within;
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(available, size - done));
        if (segment.volume < 0 ||
            static_cast<std::size_t>(segment.volume) >= impl_->build.volumes.size())
            return done > 0 ? static_cast<std::int64_t>(done) : -EIO;
        const std::int64_t volume_got = impl_->parent->read_range(
            impl_->build.volumes[static_cast<std::size_t>(segment.volume)], segment.offset + within,
            out + done, want);
        if (volume_got <= 0)
            return done > 0 ? static_cast<std::int64_t>(done) : (volume_got < 0 ? volume_got : -EIO);
        done += static_cast<std::size_t>(volume_got);
        offset = at + within + static_cast<std::uint64_t>(volume_got);
        at += segment.length;
    }
    return static_cast<std::int64_t>(done);
}

int ArchiveSource::make_directory(const std::string & /*path*/)
{
    return -EROFS;
}

int ArchiveSource::rename(const std::string & /*from*/, const std::string & /*to*/)
{
    return -EROFS;
}

int ArchiveSource::remove_file(const std::string & /*path*/)
{
    return -EROFS;
}

int ArchiveSource::remove_directory(const std::string & /*path*/)
{
    return -EROFS;
}

int ArchiveSource::copy_file(const std::string & /*from*/, const std::string & /*to*/,
                             std::size_t /*chunk*/, const ProgressFn & /*on_progress*/)
{
    return -EROFS; // copying out of an archive is a follow-up (docs/decision-log.md)
}

bool ArchiveSource::is_plain_path(const std::string & /*path*/) const
{
    // Reused as the pane model's one gate for "may this be changed" (D83): an archive never is.
    return false;
}

bool ArchiveSource::same_device(const std::string & /*a*/, const std::string & /*b*/) const
{
    return false; // never the same device as anything: nothing moves in or out
}

const void *ArchiveSource::kind_tag() const
{
    static const int tag = 0;
    return &tag;
}

} // namespace hui::explorer
