// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {

constexpr u64 STALL_LOG_INTERVAL = 100'000;

/// Returns true if the iteration count is a non-zero exact multiple of the log interval.
constexpr bool ShouldLogStallIteration(u64 iteration) {
    return iteration != 0 && (iteration % STALL_LOG_INTERVAL == 0);
}

} // namespace AmdGpu
