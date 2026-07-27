// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include "common/types.h"

namespace VideoCore {

class StreamBufferPin {
public:
    virtual ~StreamBufferPin() = default;

    virtual void Reclaim() noexcept = 0;

    [[nodiscard]] virtual u64 RequiredTick(u64 allocation_tick) const noexcept {
        return allocation_tick;
    }

    [[nodiscard]] bool IsReleased() const noexcept {
        return released.load(std::memory_order_acquire);
    }

    void Release() noexcept {
        released.store(true, std::memory_order_release);
    }

private:
    std::atomic_bool released{false};
};

using StreamBufferPinHandle = std::shared_ptr<StreamBufferPin>;

} // namespace VideoCore
