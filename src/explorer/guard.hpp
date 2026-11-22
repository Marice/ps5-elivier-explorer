// EliVier Explorer - Where the explorer may change things: /data and the drives, nowhere else.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app runs as root, so one wrong delete in /system could end the console. Reading and copying
// from any folder is fine; changing one is only allowed in an allow-list, so a folder nobody
// thought of is protected by default. "Changing" is everything that writes or removes: delete, move
// away, rename, new folder, copy into, and later editing a file.
//
// Allowed: /data and everything in it, and the drives: /mnt/usbN and /mnt/extN and everything in
// them. Not allowed: all the rest, including "/", "/mnt" (the mount points themselves), /data's own
// entry in "/", /user, /download0, /tmp and the system folders.
//
// This is by name. Two things a name cannot see are handled where the work is done: a symbolic link
// is never followed when deleting, and a path that goes through one counts as protected (see
// Source::is_plain_path); a directory on another device than its parent is not deleted through.
//
// Checked in the copy engine and the pane model themselves, so no screen can forget it.

#pragma once

#include "explorer/path.hpp"

#include <string>
#include <vector>

namespace hui::explorer
{

class ChangePolicy
{
  public:
    // Whether `folder` is a place where things may be changed.
    bool allows(const std::string &folder) const;

    // More folders that are allowed (tests, with a temporary folder); not part of the console's
    // rules.
    void allow_also(std::string root);
    void clear_extra();

  private:
    std::vector<std::string> extra_;
};

// The policy the app uses.
ChangePolicy &change_policy();

inline bool is_protected_folder(const std::string &folder)
{
    return !change_policy().allows(folder);
}

} // namespace hui::explorer
