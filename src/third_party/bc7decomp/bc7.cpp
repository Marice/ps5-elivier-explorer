// EliVier Explorer - The single compilation unit for the vendored bc7decomp (BC7 block decoder).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The vendored code uses anonymous structs in unions and a few unused locals; they are left as
// upstream wrote them and the warnings are silenced for this file only.

#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "bc7decomp.inc"
