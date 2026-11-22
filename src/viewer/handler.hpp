// EliVier Explorer - A file type is a handler: it says how well it knows a file and opens a viewer.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// To support a new type: write a FileHandler (what it matches, its icon, how it opens) and add it
// to default_registry(). Nothing in the explorer screen changes: the list icons, the hint under
// Cross and the "Open as" menu all come from the registry.
//
//   registry.add(std::make_unique<TextHandler>());
//   if (const FileHandler *handler = registry.best(probe))
//       viewer = handler->open(context, file);

#pragma once

#include "viewer/file_probe.hpp"

#include <memory>
#include <vector>

namespace hui::viewer
{

struct FileRef;
struct ViewerContext;
class Viewer;

// How well a handler knows a file. Higher wins; equal goes to the one added first.
enum class Match : std::uint8_t
{
    none,      // not mine
    fallback,  // can show anything, badly (the hex viewer): never counts as "supported"
    extension, // the extension says so
    content,   // the first bytes say so (beats a wrong or missing extension)
};

class FileHandler
{
  public:
    virtual ~FileHandler() = default;

    virtual const char *id() const = 0;    // "text", "image", "sound", "hex"
    virtual const char *label() const = 0; // "Text": in the "Open as" menu
    virtual const char *verb() const = 0;  // "View text", "Play": under Cross in the hint row
    virtual IconKind icon() const = 0;
    // Must only call probe.head() when probe.can_sniff(); a listing probe has no head.
    virtual Match match(const FileProbe &probe) const = 0;
    virtual std::unique_ptr<Viewer> open(ViewerContext &context, const FileRef &file) const = 0;
};

class HandlerRegistry
{
  public:
    void add(std::unique_ptr<FileHandler> handler);

    // The handler that knows the file best; null when none can open it at all.
    const FileHandler *best(const FileProbe &probe) const;
    // Every handler that can open it, best first (the "Open as" list).
    std::vector<const FileHandler *> all(const FileProbe &probe) const;
    // Whether a handler more specific than the fallback knows the file: the mark in the list.
    bool supported(const FileProbe &probe) const;
    // The icon for the list: the best specific handler's, else the extension's.
    IconKind icon_for(const FileProbe &probe) const;

    std::size_t size() const
    {
        return handlers_.size();
    }

  private:
    struct Scored
    {
        const FileHandler *handler;
        Match match;
    };
    std::vector<Scored> scored(const FileProbe &probe) const;

    std::vector<std::unique_ptr<FileHandler>> handlers_;
};

} // namespace hui::viewer
