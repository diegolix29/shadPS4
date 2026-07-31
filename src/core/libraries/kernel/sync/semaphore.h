// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>

#include "common/assert.h"
#include "common/types.h"

#ifdef _WIN64
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore>
#endif

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
namespace Core::Fex {
void FlushPendingGuestOrbisSignal() noexcept;
} // namespace Core::Fex
#endif

namespace Libraries::Kernel {

template <s64 max>
class Semaphore {
public:
    Semaphore(s32 initialCount)
#if !defined(_WIN64) && !defined(__APPLE__)
        : sem{initialCount}
#endif
    {
#ifdef _WIN64
        sem = CreateSemaphore(nullptr, initialCount, max, nullptr);
        ASSERT(sem);
#elif defined(__APPLE__)
        sem = dispatch_semaphore_create(initialCount);
        ASSERT(sem);
#endif
    }

    ~Semaphore() {
#ifdef _WIN64
        CloseHandle(sem);
#elif defined(__APPLE__)
        dispatch_release(sem);
#endif
    }

    void release() {
#ifdef _WIN64
        ReleaseSemaphore(sem, 1, nullptr);
#elif defined(__APPLE__)
        dispatch_semaphore_signal(sem);
#else
        sem.release();
#endif
    }

    void acquire() {
#ifdef _WIN64
        for (;;) {
            u64 res = WaitForSingleObjectEx(sem, INFINITE, true);
            if (res == WAIT_OBJECT_0) {
                return;
            }
        }
#elif defined(__APPLE__)
        for (;;) {
            const auto res = dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            if (res == 0) {
                return;
            }
        }
#else
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
        for (;;) {
            if (sem.try_acquire_for(std::chrono::milliseconds{25})) {
                Core::Fex::FlushPendingGuestOrbisSignal();
                return;
            }
            Core::Fex::FlushPendingGuestOrbisSignal();
        }
#else
        sem.acquire();
#endif
#endif
    }

    bool try_acquire() {
#ifdef _WIN64
        return WaitForSingleObjectEx(sem, 0, true) == WAIT_OBJECT_0;
#elif defined(__APPLE__)
        return dispatch_semaphore_wait(sem, DISPATCH_TIME_NOW) == 0;
#else
        return sem.try_acquire();
#endif
    }

    template <class Rep, class Period>
    bool try_acquire_for(const std::chrono::duration<Rep, Period>& rel_time) {
#ifdef _WIN64
        auto rel_time_ms = std::chrono::ceil<std::chrono::milliseconds>(rel_time);
        do {
            const auto start_time = std::chrono::high_resolution_clock::now();
            u64 timeout_ms = static_cast<u64>(rel_time_ms.count());
            u64 res = WaitForSingleObjectEx(sem, timeout_ms, true);
            if (res == WAIT_OBJECT_0) {
                return true;
            } else if (res == WAIT_IO_COMPLETION) {
                auto elapsed_time = std::chrono::high_resolution_clock::now() - start_time;
                rel_time_ms -= std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time);
            } else {
                return false;
            }
        } while (rel_time_ms.count() > 0);

        return false;
#elif defined(__APPLE__)
        const auto rel_time_ns = std::chrono::ceil<std::chrono::nanoseconds>(rel_time);
        const auto timeout = dispatch_time(DISPATCH_TIME_NOW, rel_time_ns.count());
        return dispatch_semaphore_wait(sem, timeout) == 0;
#else
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
        const auto deadline = std::chrono::steady_clock::now() + rel_time;
        // Handle non-positive duration: immediate try then flush.
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                const bool acquired = sem.try_acquire();
                Core::Fex::FlushPendingGuestOrbisSignal();
                return acquired;
            }
        }
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                Core::Fex::FlushPendingGuestOrbisSignal();
                return false;
            }
            auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            auto slice = std::min(remaining,
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds{25}));
            if (sem.try_acquire_for(slice)) {
                Core::Fex::FlushPendingGuestOrbisSignal();
                return true;
            }
            Core::Fex::FlushPendingGuestOrbisSignal();
        }
#else
        return sem.try_acquire_for(rel_time);
#endif
#endif
    }

    template <class Clock, class Duration>
    bool try_acquire_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        const auto current = Clock::now();
        if (current >= abs_time) {
            return try_acquire();
        }
        return try_acquire_for(abs_time - current);
    }

private:
#ifdef _WIN64
    HANDLE sem;
#elif defined(__APPLE__)
    dispatch_semaphore_t sem;
#else
    std::counting_semaphore<max> sem;
#endif
};

using BinarySemaphore = Semaphore<1>;
using CountingSemaphore = Semaphore<0x7FFFFFFF /*ORBIS_KERNEL_SEM_VALUE_MAX*/>;

} // namespace Libraries::Kernel
