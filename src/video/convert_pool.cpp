// EliVier Explorer - One picture's conversion split over a few threads.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/convert_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <vector>

namespace hui::video
{

struct ConvertPool::Impl
{
    struct Worker
    {
        Impl *pool = nullptr;
        int band = 0;
        pthread_t thread{};
    };

    std::vector<std::unique_ptr<Worker>> workers;
    std::mutex mutex;
    std::condition_variable work;
    std::condition_variable done;
    bool stopping = false;
    unsigned generation = 0; // one more for every picture handed out
    int remaining = 0;       // worker bands of the current picture not finished yet

    // The current picture, valid while remaining > 0.
    const Frame *frame = nullptr;
    const ConvertTables *tables = nullptr;
    std::uint16_t *out = nullptr;
    std::size_t pitch = 0;
    int bands = 1;

    void convert_band(int band) const
    {
        const int rows = frame->height;
        convert_rows_rgba16(*frame, *tables, out, pitch, rows * band / bands,
                            rows * (band + 1) / bands);
    }

    static void *worker_main(void *argument)
    {
        Worker &worker = *static_cast<Worker *>(argument);
        Impl &pool = *worker.pool;
        unsigned seen = 0;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(pool.mutex);
                pool.work.wait(lock, [&] { return pool.stopping || pool.generation != seen; });
                if (pool.stopping)
                    return nullptr;
                seen = pool.generation;
            }
            pool.convert_band(worker.band);
            std::lock_guard<std::mutex> lock(pool.mutex);
            if (--pool.remaining == 0)
                pool.done.notify_one();
        }
    }
};

ConvertPool::ConvertPool(int threads) : impl_(std::make_unique<Impl>())
{
    const int wanted = std::clamp(threads, 1, 16) - 1;
    for (int i = 0; i < wanted; ++i)
    {
        auto worker = std::make_unique<Impl::Worker>();
        worker->pool = impl_.get();
        worker->band = static_cast<int>(impl_->workers.size()) + 1;
        // A small stack is enough: a band is a loop. Never name the thread: pthread_setname_np
        // hangs on the console.
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 256 * 1024);
        const int result =
            pthread_create(&worker->thread, &attributes, &Impl::worker_main, worker.get());
        pthread_attr_destroy(&attributes);
        if (result == 0)
            impl_->workers.push_back(std::move(worker));
    }
}

ConvertPool::~ConvertPool()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->work.notify_all();
    for (const auto &worker : impl_->workers)
        pthread_join(worker->thread, nullptr);
}

int ConvertPool::threads() const
{
    return static_cast<int>(impl_->workers.size()) + 1;
}

bool ConvertPool::convert_rgba16(const Frame &frame, ConvertTables &tables,
                                 std::uint16_t *out_rgba, std::size_t out_pitch_bytes)
{
    if (out_rgba == nullptr ||
        !prepare_conversion(frame, tables, out_pitch_bytes, sizeof(std::uint16_t)))
        return false;
    Impl &pool = *impl_;
    const int workers = static_cast<int>(pool.workers.size());
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        pool.frame = &frame;
        pool.tables = &tables;
        pool.out = out_rgba;
        pool.pitch = out_pitch_bytes;
        pool.bands = workers + 1;
        pool.remaining = workers;
        ++pool.generation;
    }
    pool.work.notify_all();
    pool.convert_band(0);
    std::unique_lock<std::mutex> lock(pool.mutex);
    pool.done.wait(lock, [&] { return pool.remaining == 0; });
    return true;
}

} // namespace hui::video
