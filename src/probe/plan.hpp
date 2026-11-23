// EliVier Explorer - Which console probes a request asks for.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A probe is a small measurement or question put to the console before a feature is built on
// the answer (docs/decision-log.md, D41 and D44). The request is one word or several joined with
// '+', for example "memory+upload", or "safe". Probes that only read and measure can be combined;
// a probe that changes the display (an HDR surface) ends the launch and always runs alone, so
// that what went wrong, if anything, is known. So does `decoder-bind`, which calls into a system
// library of which only the names are known.

#pragma once

#include <string>
#include <string_view>

namespace hui::probe
{

enum class HdrMode
{
    none,
    pq10,  // 10-bit surface in the BT.2020 PQ colour space (HDR10)
    hlg10, // 10-bit surface, BT.2020 HLG
    fp16,  // half-float surface, linear scRGB
    videoout, // the runtime's scanout buffers retyped to HDR10 (D68), test bars packed by hand
};

struct ProbePlan
{
    bool memory = false;       // direct and flexible memory: how much, how fast
    bool upload = false;       // what it costs to upload a 4K video frame to textures
    bool convert = false;      // what the CPU conversion of a 1080p picture costs, and its upload
    bool egl = false;          // EGL extensions and surface formats the driver offers
    bool decoder_load = false; // can libSceVideodec2 be loaded and its functions found
    bool decoder_bind = false; // call the one read-only function the HW_DECODE=1 build imports
    bool speakers = false;     // a tone on each of the eight surround channels in turn
    bool hwdec = false;        // bring a hardware decoder up before the HEN, decode one picture after
    HdrMode hdr = HdrMode::none;
};

// "safe" is memory+upload+egl+decoder-load. False and `error` for an unknown name, an empty one,
// one over 64 characters, or an HDR attempt combined with anything else.
bool parse_probe_plan(std::string_view name, ProbePlan &plan, std::string *error);

// The plan back as a request name ("memory+upload", "hdr:pq10"), for logs and the report.
std::string describe(const ProbePlan &plan);

} // namespace hui::probe
