// EliVier Explorer - The handlers that ship with the app.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "viewer/handler.hpp"

namespace hui::viewer
{

// Every built-in handler, in the order they win ties. A new file type is one line in registry.cpp.
const HandlerRegistry &default_registry();

} // namespace hui::viewer
