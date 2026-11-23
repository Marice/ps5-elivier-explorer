// EliVier Explorer - Console probes: questions put to the PS5 before video is built on the answers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each probe answers one of the open questions of docs/decision-log.md, D41 and D44: how much
// direct memory a decoder can have and how fast it is, what a 4K frame costs to upload, which
// surface formats the EGL driver offers, whether the hardware decoder's library can be reached,
// and whether an HDR surface is accepted. Nothing here is played or shown to a user.
//
// Every probe logs each answer the moment it has it ("[HUI] probe ..."), so a crash halfway still
// leaves what was found. The ones that only read and measure can run together; an HDR attempt
// changes the display and runs alone (probe/plan.hpp).

#pragma once

#include "platform/ps5/display_egl.hpp"
#include "probe/plan.hpp"
#include "probe/report.hpp"

namespace hui::ps5
{

// The decoder-bind probe on its own, for the start of main(): EVO Player brings its decoders up
// before the HEN frees the app, so this one is called then, with no display and no GL. It loads
// sysmodule 207 as EVO does and then calls the one read-only function of the HW_DECODE=1 build.
// Every step is written to the trace first; a fault in the call leaves the step as the last line.
void run_decoder_bind_probe(probe::Report &report, const char *label);

// Runs the probes of `plan` in a fixed order (memory, upload, egl, decoder-load, then an HDR
// attempt) and adds the answers to `report`. The display must be open: the upload and egl probes
// use its GL context. An HDR attempt closes the display and leaves it closed; the caller opens a
// normal one again if it needs one.
void run_probes(const probe::ProbePlan &plan, Display &display, probe::Report &report);

} // namespace hui::ps5
