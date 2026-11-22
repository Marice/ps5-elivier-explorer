// EliVier Explorer - What freeing the app from its sandbox did, for the trace and the log.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace hui::explorer
{

enum class AccessRoute
{
    not_tried,    // a build without a HEN (the PC)
    already_free, // the app saw the whole console when it started
    request_file, // a daemon took a request file in /download0
    port_9028,    // etaHEN's legacy command port
    port_9069,    // the SharpProspero daemon's port
    none,         // nobody freed the app
};

const char *route_name(AccessRoute route);

struct AccessReport
{
    AccessRoute route = AccessRoute::not_tried;
    std::string request_taken; // which request file a daemon took, if one did
    bool root_put_back = false;
    bool app_folder = true; // /app0 is there after freeing
    int uid_before = -1;
    int uid_after = -1;
    int milliseconds = 0;
    std::vector<std::string> steps; // what happened, in order
};

// Set once by the app at start; kept for the trace and the log.
void set_access_report(AccessReport report);
const AccessReport &access_report();

} // namespace hui::explorer
