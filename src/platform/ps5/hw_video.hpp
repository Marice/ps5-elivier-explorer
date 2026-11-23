// EliVier Explorer - The console's hardware video decoders, made once at start-up.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libSceVideodec2 answers only when it is brought up before the HEN frees the app, and a decoder
// made then keeps working afterwards (docs/decision-log.md, D49 and D60). So the decoders are
// resident: one for H.264 up to 1920 x 1088 and one for HEVC Main 10 up to 3840 x 2176, made at
// the start of main() and kept for the whole run. A film borrows one (video::HardwareBackend)
// and gives it back when it closes; a second film at the same time decodes in software.
//
//   hui::ps5::hw_video_bring_up();   // in main, before hen_prepare()

#pragma once

namespace hui::ps5
{

// Makes the decoders and lends them to the video module (video::set_hardware_provider). Every
// step goes to the trace first. Does nothing in a build without HW_DECODE=1. A decoder that
// cannot be made is left out; the video module then decodes that codec in software.
void hw_video_bring_up();

} // namespace hui::ps5
