// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "common/types.h"

namespace Core::FileSys {

struct File;

[[nodiscard]] constexpr bool IsSupportedReadBandwidth(u32 bandwidth_mibps) {
    switch (bandwidth_mibps) {
    case 0:
    case 75:
    case 100:
    case 125:
        return true;
    default:
        return false;
    }
}

// Unsupported non-zero values clamp to the nearest public profile so a hand-edited config
// asking for throttling never silently runs at native speed.
[[nodiscard]] constexpr u32 NormalizeReadBandwidth(u32 bandwidth_mibps) {
    if (IsSupportedReadBandwidth(bandwidth_mibps)) {
        return bandwidth_mibps;
    }
    u32 nearest = 75;
    u32 nearest_distance = std::numeric_limits<u32>::max();
    for (const u32 profile : {75u, 100u, 125u}) {
        const u32 distance =
            bandwidth_mibps > profile ? bandwidth_mibps - profile : profile - bandwidth_mibps;
        if (distance < nearest_distance) {
            nearest = profile;
            nearest_distance = distance;
        }
    }
    return nearest;
}

[[nodiscard]] constexpr u8 StoragePriorityIndex(s32 priority) {
    return static_cast<u8>(std::clamp(priority, -128, 127) + 128);
}

class StorageTimingModel {
public:
    static constexpr u64 MaxChunkSize = 512 * 1024;
    static constexpr auto AverageSeek = std::chrono::milliseconds{13};
    static constexpr auto AverageRotation = std::chrono::microseconds{5'556};

    explicit constexpr StorageTimingModel(u32 bandwidth_mibps) : bandwidth_mibps{bandwidth_mibps} {}

    [[nodiscard]] constexpr std::chrono::nanoseconds TransferDuration(u64 bytes) const {
        if (bandwidth_mibps == 0 || bytes == 0) {
            return {};
        }
        const u64 bytes_per_second = static_cast<u64>(bandwidth_mibps) * 1024ULL * 1024ULL;
        const u64 whole_seconds = bytes / bytes_per_second;
        const u64 remainder = bytes % bytes_per_second;
        return std::chrono::seconds{whole_seconds} +
               std::chrono::nanoseconds{remainder * 1'000'000'000ULL / bytes_per_second};
    }

    [[nodiscard]] constexpr std::chrono::nanoseconds ServiceDuration(u64 bytes,
                                                                     bool sequential) const {
        return TransferDuration(bytes) +
               (sequential ? std::chrono::nanoseconds::zero() : AverageSeek + AverageRotation);
    }

private:
    u32 bandwidth_mibps{};
};

struct StorageReadSpan {
    void* data{};
    u64 size{};
};

using StorageReadSpans = std::vector<StorageReadSpan>;

class StorageRequest {
public:
    [[nodiscard]] bool IsCanceled() const;

private:
    friend class StorageScheduler;
    void Cancel();
    std::atomic_bool canceled{};
};

using StorageRequestHandle = std::shared_ptr<StorageRequest>;
using StorageCompletion = std::function<void(s64 result, bool canceled)>;

struct StorageSchedulerStats {
    u64 bytes_read{};
    u64 chunks{};
    u64 sequential_chunks{};
    u64 positioned_chunks{};
    u64 modeled_wait_ns{};
    u64 timer_oversleep_ns{};
    u64 host_overrun_ns{};
    u64 host_wait_ns{};
    u64 prefetched_chunks{};
    u64 demand_chunks{};
    u64 max_staging_buffers{};
    u64 max_queue_depth{};
};

class StorageScheduler {
public:
    StorageScheduler();
    ~StorageScheduler();

    StorageScheduler(const StorageScheduler&) = delete;
    StorageScheduler& operator=(const StorageScheduler&) = delete;

    void Configure(u32 bandwidth_mibps);
    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] u32 GetBandwidthMiBps() const;

    StorageRequestHandle SubmitRead(std::shared_ptr<File> file, StorageReadSpans spans, u64 offset,
                                    s32 priority, StorageCompletion completion);
    s64 ReadBlocking(std::shared_ptr<File> file, std::span<const StorageReadSpan> spans, u64 offset,
                     s32 priority = 0);
    void Cancel(const StorageRequestHandle& request);
    [[nodiscard]] StorageSchedulerStats GetStats() const;

    // Called from the present thread on every real guest flip. When the emulator runs slower
    // than the game's target flip cadence, modeled I/O time is stretched by the same ratio so
    // the guest-perceived delivery rate per frame stays close to a real PS4 under load.
    void ReportGuestFlip(std::chrono::nanoseconds expected_flip_period);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

StorageScheduler& GetApp0StorageScheduler();

// True when reads from this file must be routed through the app0 storage scheduler.
[[nodiscard]] bool ShouldScheduleAppRead(const File& file);

} // namespace Core::FileSys
