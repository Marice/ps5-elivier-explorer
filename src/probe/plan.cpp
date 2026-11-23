// EliVier Explorer - Which console probes a request asks for.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe/plan.hpp"

namespace hui::probe
{

namespace
{

constexpr std::size_t kMaxNameLength = 64;

bool fail(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
    return false;
}

} // namespace

bool parse_probe_plan(std::string_view name, ProbePlan &plan, std::string *error)
{
    plan = ProbePlan{};
    if (name.empty())
        return fail(error, "The probe name is empty");
    if (name.size() > kMaxNameLength)
        return fail(error, "The probe name is too long");

    bool alone_asked = false; // a probe that must be the only one
    int tokens = 0;
    std::size_t start = 0;
    while (start <= name.size())
    {
        std::size_t end = name.find('+', start);
        if (end == std::string_view::npos)
            end = name.size();
        const std::string_view token = name.substr(start, end - start);
        ++tokens;
        if (token == "safe")
        {
            plan.memory = plan.upload = plan.egl = plan.decoder_load = true;
        }
        else if (token == "memory")
        {
            plan.memory = true;
        }
        else if (token == "upload")
        {
            plan.upload = true;
        }
        else if (token == "convert")
        {
            plan.convert = true;
        }
        else if (token == "egl")
        {
            plan.egl = true;
        }
        else if (token == "decoder-load")
        {
            plan.decoder_load = true;
        }
        else if (token == "decoder-bind")
        {
            plan.decoder_bind = true;
            alone_asked = true;
        }
        else if (token == "decoder-query")
        {
            return fail(error, "decoder-query is not built yet");
        }
        else if (token.substr(0, 4) == "hdr:")
        {
            const std::string_view mode = token.substr(4);
            if (mode == "pq10")
                plan.hdr = HdrMode::pq10;
            else if (mode == "hlg10")
                plan.hdr = HdrMode::hlg10;
            else if (mode == "fp16")
                plan.hdr = HdrMode::fp16;
            else
                return fail(error, "Unknown HDR mode '" + std::string(mode) + "'");
            alone_asked = true;
        }
        else
        {
            return fail(error, "Unknown probe '" + std::string(token) + "'");
        }
        start = end + 1;
    }
    if (alone_asked && tokens != 1)
        return fail(error, "This probe changes the display or calls a system library and must "
                           "run alone");
    return true;
}

std::string describe(const ProbePlan &plan)
{
    std::string text;
    const auto add = [&text](bool on, const char *name) {
        if (!on)
            return;
        if (!text.empty())
            text += '+';
        text += name;
    };
    add(plan.memory, "memory");
    add(plan.upload, "upload");
    add(plan.convert, "convert");
    add(plan.egl, "egl");
    add(plan.decoder_load, "decoder-load");
    add(plan.decoder_bind, "decoder-bind");
    switch (plan.hdr)
    {
    case HdrMode::pq10:
        add(true, "hdr:pq10");
        break;
    case HdrMode::hlg10:
        add(true, "hdr:hlg10");
        break;
    case HdrMode::fp16:
        add(true, "hdr:fp16");
        break;
    case HdrMode::none:
        break;
    }
    return text;
}

} // namespace hui::probe
