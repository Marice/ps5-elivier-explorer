// EliVier Explorer - One picture's conversion split over a few threads.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A 1080p picture takes about 10 ms to convert on one console core (docs/decision-log.md, D53); a
// 4K one four times that, more than a 24 fps film allows. The rows are independent, so the
// picture is cut into bands, one per thread, and the calling thread does a band itself. The
// workers live as long as the pool: no thread is made per picture.
//
//   ConvertPool pool(4);                      // the caller and three workers
//   pool.convert_rgba16(frame, tables, out, pitch);

#pragma once

#include "video/convert.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hui::video
{

class ConvertPool
{
  public:
    // `threads` bands in all, the caller's included; 1 converts on the calling thread only. A
    // worker that cannot be started is simply left out.
    explicit ConvertPool(int threads);
    ~ConvertPool();
    ConvertPool(const ConvertPool &) = delete;
    ConvertPool &operator=(const ConvertPool &) = delete;

    // As convert_to_rgba16, and returns when every band is done. One caller at a time.
    bool convert_rgba16(const Frame &frame, ConvertTables &tables, std::uint16_t *out_rgba,
                        std::size_t out_pitch_bytes);

    // The bands a picture is cut into: the workers that started, plus the caller.
    int threads() const;

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::video
