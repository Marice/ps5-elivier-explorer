// EliVier Explorer - What freeing the app from its sandbox did, for the Probe screen.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "explorer/access_report.hpp"

#include <utility>

namespace hui::explorer
{

namespace
{

AccessReport g_report;

} // namespace

const char *route_name(AccessRoute route)
{
    switch (route)
    {
    case AccessRoute::not_tried:
        return "not tried";
    case AccessRoute::already_free:
        return "already free";
    case AccessRoute::request_file:
        return "request file";
    case AccessRoute::port_9028:
        return "port 9028";
    case AccessRoute::port_9069:
        return "port 9069";
    case AccessRoute::none:
        return "still sandboxed";
    }
    return "?";
}

void set_access_report(AccessReport report)
{
    g_report = std::move(report);
}

const AccessReport &access_report()
{
    return g_report;
}

} // namespace hui::explorer
