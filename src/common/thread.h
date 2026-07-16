// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include "common/types.h"

namespace Common {

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

void SetCurrentThreadRealtime(std::chrono::nanoseconds period_ns);

void SetCurrentThreadPriority(ThreadPriority new_priority);

void SetCurrentThreadName(const char* name);

void SetThreadName(void* thread, const char* name);

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible);

class AccurateTimer {
    std::chrono::nanoseconds target_interval{};
    std::chrono::nanoseconds max_timing_debt{};
    std::chrono::nanoseconds total_wait{};

    std::chrono::steady_clock::time_point start_time;

public:
    explicit AccurateTimer(std::chrono::nanoseconds target_interval,
                           u32 max_catch_up_intervals = 2);

    void Start();

    void End();

    /// Applies a bounded correction to the next wake-up. This is intended for clock
    /// discipline; it never changes the nominal interval.
    void Adjust(std::chrono::nanoseconds correction);

    /// Drops accumulated timing debt after an external clock discontinuity.
    void Reset();

    std::chrono::nanoseconds GetTotalWait() const {
        return total_wait;
    }
};

std::string GetCurrentThreadName();

} // namespace Common
