// EliVier Explorer - Numbers as text for people.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace hui::explorer
{

// "512 B", "4.2 KB", "512 MB", "1.3 GB", "2.0 TB": binary units, one decimal under 100.
std::string format_bytes(std::uint64_t bytes);

} // namespace hui::explorer
