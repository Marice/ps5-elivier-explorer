// EliVier Explorer - The handlers that ship with the app.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/registry.hpp"

#include "viewer/text_viewer.hpp"

namespace hui::viewer
{

const HandlerRegistry &default_registry()
{
    static const HandlerRegistry registry = []
    {
        HandlerRegistry built;
        // Image, hex and sound handlers are added here as they are written.
        built.add(std::make_unique<TextHandler>());
        return built;
    }();
    return registry;
}

} // namespace hui::viewer
