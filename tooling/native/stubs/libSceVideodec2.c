/* EliVier Explorer - Import stub of libSceVideodec2: names only, no code (see tools/make-videodec2-stub.sh).
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The converter reads the module, library and symbol names of an import from a shared object
 * like this one (tooling/native/elf_object.cpp) and the console's loader binds them to the
 * system library when the app starts. Only the functions the app really calls are listed: a name
 * the firmware does not export makes the loader refuse the whole app. The name list follows the
 * calls EVO Player and VLC for PS5 both make on hardware (src/platform/ps5/videodec2.hpp has the
 * layouts); nothing is copied from them.
 */

void sceVideodec2QueryComputeMemoryInfo(void)
{
}

void sceVideodec2AllocateComputeQueue(void)
{
}

void sceVideodec2ReleaseComputeQueue(void)
{
}

void sceVideodec2QueryDecoderMemoryInfo(void)
{
}

void sceVideodec2CreateDecoder(void)
{
}

void sceVideodec2DeleteDecoder(void)
{
}

void sceVideodec2Reset(void)
{
}

void sceVideodec2Decode(void)
{
}

void sceVideodec2Flush(void)
{
}
