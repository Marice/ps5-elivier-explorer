// EliVier Explorer - The handlers that ship with the app.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/registry.hpp"

#include "viewer/hex_viewer.hpp"
#include "viewer/image_viewer.hpp"
#include "viewer/sound_viewer.hpp"
#include "viewer/text_viewer.hpp"

namespace hui::viewer
{

const HandlerRegistry &default_registry()
{
    static const HandlerRegistry registry = []
    {
        HandlerRegistry built;
        // Image, hex and sound handlers are added here as they are written.
        built.add(std::make_unique<ImageHandler>());
        built.add(std::make_unique<SoundHandler>());
        built.add(std::make_unique<TextHandler>());
        built.add(std::make_unique<HexHandler>());
        return built;
    }();
    return registry;
}

} // namespace hui::viewer
