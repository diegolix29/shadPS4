// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

enum class MemoryWriteSource : u8 {
    Cpu,
    CommandProcessor,
    GpuCompletion,
    Map,
    Unmap,
};

struct MemoryWriteWatch {
    VAddr page{};
    u64 id{};
    u64 epoch{};

    explicit operator bool() const noexcept {
        return id != 0;
    }
};

using MemoryWriteCallback =
    void (*)(void* user_data, VAddr page, u64 epoch, MemoryWriteSource source) noexcept;

struct MemoryWriteNotifyResult {
    u32 matched_pages{};
    u32 callbacks{};
    bool had_active_watches{};
};

class PageManager {
public:
    // Use the same page size as the tracker.
    static constexpr size_t PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// Arms a one-shot notification for writes touching the page that contains address. The
    /// callback must only update consumer-owned state and must not call back into PageManager.
    [[nodiscard]] MemoryWriteWatch ArmWriteWatch(VAddr address, MemoryWriteCallback callback,
                                                 void* user_data);

    /// Cancels a write watch. Once this returns, its callback can no longer be running.
    bool CancelWriteWatch(MemoryWriteWatch watch);

    /// Notifies one-shot observers after a guest-memory write becomes visible.
    MemoryWriteNotifyResult NotifyWrite(VAddr address, u64 size, MemoryWriteSource source);

    /// Updates watches in the pages touching the specified region.
    template <bool track, bool is_read = false>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Returns true if the page containing address has active read watchers.
    [[nodiscard]] bool HasReadWatcher(VAddr address) const;

    /// Lock-free variant over a range, for the guest copy engine.
    [[nodiscard]] bool HasReadWatchers(VAddr address, u64 size) const noexcept;

    /// Temporarily unprotects the page (e.g. for single-stepping after a fault).
    void TemporarilyUnprotect(VAddr address, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PAGE_SIZE);
    }

    static constexpr size_t GetPageSize() {
        return PAGE_SIZE;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore