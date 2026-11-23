// ps5-homebrew-ui - Factories of every UI design in the app.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hui::viewer
{
class HandlerRegistry;
class HdrOutput;
class TextureHost;
} // namespace hui::viewer
namespace hui::audio
{
class Mixer;
class SurroundSink;
}

namespace hui::concepts
{

// One function per screen, each defined in its own file in this directory.
// To add one: write the file, declare its factory here and list it in
// registry.cpp. These are EliVier Explorer's screens, in L1 / R1 order.
std::unique_ptr<app::Concept> make_explorer(app::Context &context);
std::unique_ptr<app::Concept> make_about(app::Context &context);
// The About screen's QR code: a PNG and how to make it a texture (GL work, done by the app).
void set_about_qr(std::uint32_t (*load_png)(const std::string &path), std::string path);
// What the file viewers use: where textures come from (GL, so the app supplies it), the mixer, and
// an eight-channel output for films with surround sound (the console's; null elsewhere: those
// films then play in stereo). Null in tests; a viewer that needs one says so on screen.
void set_explorer_services(hui::viewer::TextureHost *textures, hui::audio::Mixer *mixer,
                           hui::audio::SurroundSink *surround = nullptr,
                           hui::viewer::HdrOutput *hdr = nullptr);
// A handler registry other than the built-in one (tests); null brings the built-in one back.
void set_explorer_registry(const hui::viewer::HandlerRegistry *registry);
// The folders the explorer's two panes open in (the console's /data and /mnt; temporary ones in
// tests).
void set_explorer_start(std::string left, std::string right);

} // namespace hui::concepts
