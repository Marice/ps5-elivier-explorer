// EliVier Explorer - A line in the trace when the app crashes: signal, address, where it was.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   hui::ps5::catch_crashes(reinterpret_cast<const void *>(&anchor_function));
//
// The build has no frame pointers to walk. The line gives the crash address and every word on
// the stack that points near `anchor` (a function in the app), as offsets from it; map them with
// nm on the same build: address = nm's address for the anchor + offset.

#pragma once

namespace hui::ps5
{

void catch_crashes(const void *anchor);

} // namespace hui::ps5
