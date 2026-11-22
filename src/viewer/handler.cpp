// EliVier Explorer - A file type is a handler: it says how well it knows a file and opens a viewer.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/handler.hpp"

#include <algorithm>
#include <utility>

namespace hui::viewer
{

void HandlerRegistry::add(std::unique_ptr<FileHandler> handler)
{
    handlers_.push_back(std::move(handler));
}

std::vector<HandlerRegistry::Scored> HandlerRegistry::scored(const FileProbe &probe) const
{
    std::vector<Scored> out;
    for (const std::unique_ptr<FileHandler> &handler : handlers_)
    {
        const Match match = handler->match(probe);
        if (match != Match::none)
            out.push_back({handler.get(), match});
    }
    // Best first; a stable sort keeps the order they were added in for equal matches.
    std::stable_sort(out.begin(), out.end(),
                     [](const Scored &a, const Scored &b) { return a.match > b.match; });
    return out;
}

const FileHandler *HandlerRegistry::best(const FileProbe &probe) const
{
    const std::vector<Scored> found = scored(probe);
    return found.empty() ? nullptr : found.front().handler;
}

std::vector<const FileHandler *> HandlerRegistry::all(const FileProbe &probe) const
{
    std::vector<const FileHandler *> out;
    for (const Scored &item : scored(probe))
        out.push_back(item.handler);
    return out;
}

bool HandlerRegistry::supported(const FileProbe &probe) const
{
    const std::vector<Scored> found = scored(probe);
    return !found.empty() && found.front().match >= Match::extension;
}

IconKind HandlerRegistry::icon_for(const FileProbe &probe) const
{
    const std::vector<Scored> found = scored(probe);
    if (!found.empty() && found.front().match >= Match::extension)
        return found.front().handler->icon();
    return icon_by_extension(probe.ext());
}

} // namespace hui::viewer
