// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/thread.h"

namespace Vulkan {

/// A pipeline compile that runs once, either on a worker or on the first thread that waits for it.
class PipelineBuild {
public:
    explicit PipelineBuild(std::function<void()> task_) : task{std::move(task_)} {}

    PipelineBuild(const PipelineBuild&) = delete;
    PipelineBuild& operator=(const PipelineBuild&) = delete;

    bool IsBuilt() const noexcept {
        return done.load(std::memory_order_acquire);
    }

    /// Runs the build on this thread unless another thread has claimed it.
    void Run() {
        if (claimed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        task();
        task = {};
        Finish();
    }

    /// Drops the build when no thread has started it yet.
    void Cancel() {
        if (claimed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        task = {};
        Finish();
    }

    /// Builds it here when no worker has started it, otherwise waits for the worker.
    void Wait() {
        Run();
        if (IsBuilt()) {
            return;
        }
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return done.load(std::memory_order_acquire); });
    }

private:
    void Finish() {
        std::scoped_lock lock{mutex};
        done.store(true, std::memory_order_release);
        cv.notify_all();
    }

    std::function<void()> task;
    std::atomic_bool claimed{};
    std::atomic_bool done{};
    std::mutex mutex;
    std::condition_variable cv;
};

/// Threads that run pipeline builds in the order they were submitted.
class PipelineBuildWorkers {
public:
    explicit PipelineBuildWorkers(size_t num_threads) {
        threads.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back([this](std::stop_token stop) { WorkerLoop(stop); });
        }
    }

    ~PipelineBuildWorkers() {
        for (auto& thread : threads) {
            thread.request_stop();
        }
        {
            std::scoped_lock lock{mutex};
            cv.notify_all();
        }
        threads.clear();
    }

    PipelineBuildWorkers(const PipelineBuildWorkers&) = delete;
    PipelineBuildWorkers& operator=(const PipelineBuildWorkers&) = delete;

    void Submit(std::shared_ptr<PipelineBuild> build) {
        {
            std::scoped_lock lock{mutex};
            queue.push_back(std::move(build));
        }
        cv.notify_one();
    }

    /// Waits until every submitted build has completed.
    void WaitIdle() {
        for (;;) {
            std::shared_ptr<PipelineBuild> build;
            {
                std::scoped_lock lock{mutex};
                if (queue.empty()) {
                    break;
                }
                build = std::move(queue.front());
                queue.pop_front();
            }
            build->Run();
        }
        std::unique_lock lock{mutex};
        idle_cv.wait(lock, [this] { return queue.empty() && running == 0; });
    }

    static size_t DefaultThreadCount() {
        const size_t hw = std::max(std::thread::hardware_concurrency(), 2u);
        return std::clamp<size_t>(hw / 2, 1, 8);
    }

private:
    void WorkerLoop(std::stop_token stop) {
        Common::SetCurrentThreadName("shadPS4:PipelineBuild");
        for (;;) {
            std::shared_ptr<PipelineBuild> build;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] { return stop.stop_requested() || !queue.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                build = std::move(queue.front());
                queue.pop_front();
                ++running;
            }
            build->Run();
            build.reset();
            {
                std::scoped_lock lock{mutex};
                --running;
                if (queue.empty() && running == 0) {
                    idle_cv.notify_all();
                }
            }
        }
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::deque<std::shared_ptr<PipelineBuild>> queue;
    size_t running{};
    std::vector<std::jthread> threads;
};

} // namespace Vulkan
