// EliVier Explorer - The handlers that ship with the app.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/registry.hpp"

namespace hui::viewer
{

const HandlerRegistry &default_registry()
{
    static const HandlerRegistry registry = []
    {
        HandlerRegistry built;
        // Text, image, hex and sound handlers are added here as they are written.
        return built;
    }();
    return registry;
}

} // namespace hui::viewer
