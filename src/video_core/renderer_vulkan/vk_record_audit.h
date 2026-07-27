// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

/// Checks which thread records each command buffer. Every command buffer entry point of the
/// default dispatcher is routed through a check of the calling thread against the thread that
/// claimed the command buffer. Diagnostic only: it slows every recorded command down.
namespace Vulkan::RecordAudit {

/// Set while the audit runs; checked before every recorded command.
extern std::atomic<bool> watch_producers;

/// Counts a command recorded through a scheduler with a recording thread by the calling thread.
void NoteProducer() noexcept;

/// True when SHADPS4_VK_RECORD_AUDIT=1 asks for the audit.
[[nodiscard]] bool RequestedByEnvironment();

/// Installs the checks. Idempotent. Call after the device dispatcher is initialized and before
/// anything records.
void Install();

[[nodiscard]] bool IsInstalled() noexcept;

/// Declares that the calling thread alone records cmdbuf from now on.
void ClaimCommandBuffer(vk::CommandBuffer cmdbuf);

struct Stats {
    /// Calls on claimed command buffers from the thread that claimed them.
    u64 owner_calls{};
    /// Calls on claimed command buffers from any other thread.
    u64 foreign_calls{};
    /// Calls on command buffers nobody claimed (presentation, overlay uploads).
    u64 unclaimed_calls{};
    /// Entry point of the first foreign call.
    std::string first_foreign;
    /// Threads that recorded commands for the recording thread, and how many each.
    std::vector<std::pair<u64, u64>> producers;
};

[[nodiscard]] Stats GetStats();

void LogStats(std::string_view context);

} // namespace Vulkan::RecordAudit
