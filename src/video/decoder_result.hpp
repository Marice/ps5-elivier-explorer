// EliVier Explorer - The answers a decoder gives, shared by the video and the audio decoder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace hui::video
{

enum class SendResult
{
    accepted,
    full,  // take output with receive(), then offer the packet again
    error, // the decoder cannot go on
};

enum class ReceiveResult
{
    frame, // a picture, or a chunk of sound
    need_input, // give it more packets
    end,        // after send_end(): everything has come out
    error,
};

} // namespace hui::video
