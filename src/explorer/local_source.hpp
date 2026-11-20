// EliVier Explorer - The console's own drives (and the PC's, in tests): plain POSIX.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   hui::explorer::LocalSource drives;
//   hui::explorer::ListResult listing = drives.list("/mnt/usb0");

#pragma once

#include "explorer/source.hpp"

namespace hui::explorer
{

class LocalSource final : public Source
{
  public:
    std::string display_name() const override;

    ListResult list(const std::string &path) const override;
    bool stat(const std::string &path, Entry *out) const override;
    SpaceInfo space(const std::string &path) const override;

    int make_directory(const std::string &path) override;
    int rename(const std::string &from, const std::string &to) override;
    int remove_file(const std::string &path) override;
    int remove_directory(const std::string &path) override;
    int copy_file(const std::string &from, const std::string &to, std::size_t chunk,
                  const ProgressFn &on_progress) override;

    const void *kind_tag() const override;
};

} // namespace hui::explorer
