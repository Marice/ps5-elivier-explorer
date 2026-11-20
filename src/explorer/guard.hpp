// EliVier Explorer - Folders the explorer will not change: the console's own files.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app runs as root, so one wrong delete in /system could end the console. A protected folder
// can be read and copied from, never changed: nothing is renamed, made, deleted or written in it.
// Checked in the copy engine and the pane model themselves, so no screen can forget it.
//
// Protected: "/" and "/mnt" (what is in them is system folders and mount points); everything
// inside /system, /system_data, /system_ex, /preinst, /preinst2, /boot, /dev and /rescue; and the
// mounts under /mnt that are not drives (anything but usbN and extN).

#pragma once

#include "explorer/path.hpp"

#include <string>

namespace hui::explorer
{

inline bool is_protected_folder(const std::string &folder)
{
    const std::string path = path_normalized(folder);
    if (path == "/" || path == "/mnt")
        return true;
    static const char *const kTrees[] = {"/system",   "/system_data", "/system_ex", "/preinst",
                                         "/preinst2", "/boot",        "/dev",       "/rescue"};
    for (const char *tree : kTrees)
    {
        if (path_is_inside(path, tree))
            return true;
    }
    if (path_is_inside(path, "/mnt"))
    {
        const std::string rest = path.substr(5); // after "/mnt/"
        const std::string first = rest.substr(0, rest.find('/'));
        return first.rfind("usb", 0) != 0 && first.rfind("ext", 0) != 0;
    }
    return false;
}

} // namespace hui::explorer
