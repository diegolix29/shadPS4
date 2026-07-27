// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

namespace VideoCore {

/// Counts the cache flushes the guest issues with EVENT_WRITE and ACQUIRE_MEM.
///
/// The packets record no barrier. A resource remembers the epoch of its last write, and an
/// access that its state alone would not separate from that write gets a barrier when a flush
/// happened in between.
class FlushEpoch {
public:
    [[nodiscard]] static u64 Current() noexcept {
        return epoch.load(std::memory_order_relaxed);
    }

    static void Advance() noexcept {
        epoch.fetch_add(1, std::memory_order_relaxed);
    }

private:
    static inline std::atomic<u64> epoch{1};
};

} // namespace VideoCore
