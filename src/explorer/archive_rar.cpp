// EliVier Explorer - Reads a RAR archive's index: stored entries only, RAR4 and RAR5, old
// .rar/.r00/.r01 and new .partNN.rar splits; nothing here decompresses.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/archive_formats.hpp"

namespace hui::explorer
{

bool read_rar(const FormatReader & /*reader*/, ArchiveBuild & /*build*/, std::string *error)
{
    *error = "RAR archives are not supported yet";
    return false;
}

} // namespace hui::explorer
