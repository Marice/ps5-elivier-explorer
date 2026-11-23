/* EliVier Explorer - Import stub of libSceVideodec2: names only, no code (see tools/make-videodec2-stub.sh).
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The converter reads the module, library and symbol names of an import from a shared object
 * like this one (tooling/native/elf_object.cpp) and the console's loader binds them to the
 * system library when the app starts. Only the functions the app really calls are listed: a name
 * the firmware does not export makes the loader refuse the whole app. The name list follows the
 * calls EVO Player makes on hardware (its sce_videodec2.h); nothing is copied from it.
 */

void sceVideodec2QueryComputeMemoryInfo(void)
{
}
