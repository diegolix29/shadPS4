// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/types.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/stream_buffer_pin.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

enum class GpuAuthorityState : u8 {
    GpuAuthoritative,
    Materializing,
    HostCurrent,
    Superseded,
    Failed,
};

struct GpuAuthorityShadow final : StreamBufferPin {
    GpuAuthorityShadow(u8* data_, VAddr guest_addr_, u64 buffer_offset_, u32 size_, u64 tick_)
        : data{data_}, guest_addr{guest_addr_}, buffer_offset{buffer_offset_}, size{size_},
          ready_tick{tick_}, tick{tick_} {}

    ~GpuAuthorityShadow() override {
        Release();
    }

    void Reclaim() noexcept override {
        if (IsReleased()) {
            return;
        }
        std::scoped_lock lock{data_mutex};
        if (IsReleased() || owned_data) {
            return;
        }
        auto storage = std::unique_ptr<u8[]>{new (std::nothrow) u8[size]};
        if (!storage) {
            return;
        }
        std::memcpy(storage.get(), data, size);
        data = storage.get();
        owned_data = std::move(storage);
        Release();
    }

    void ExtendLifetime(u64 required_tick) noexcept {
        u64 current_tick = tick.load(std::memory_order_relaxed);
        while (current_tick < required_tick &&
               !tick.compare_exchange_weak(current_tick, required_tick,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
        }
    }

    /// Tick until which the download buffer region must stay reserved. GPU consumers of the
    /// shadow extend it.
    [[nodiscard]] u64 Tick() const noexcept {
        return tick.load(std::memory_order_acquire);
    }

    /// Tick after which the shadow bytes are valid.
    [[nodiscard]] u64 ReadyTick() const noexcept {
        return ready_tick;
    }

    [[nodiscard]] u64 RequiredTick(u64 allocation_tick) const noexcept override {
        return std::max(allocation_tick, Tick());
    }

    std::mutex data_mutex;
    u8* data{};
    VAddr guest_addr{};
    u64 buffer_offset{};
    u32 size{};
    const u64 ready_tick{};
    std::atomic<u64> tick{};
    std::unique_ptr<u8[]> owned_data;
};

struct GpuAuthorityEntry {
    u64 authority_seq{0};
    u32 image_id{0};
    u64 image_uid{0};
    u64 resource_version{0};
    VAddr guest_begin{0};
    VAddr guest_end{0};
    u32 download_size{0};
    u64 producer_tick{0};
    u64 virtual_fence_seq{0};
    VAddr label_addr{0};
    u32 label_value{0};
    u64 label_generation{0};
    GpuAuthorityState state{GpuAuthorityState::GpuAuthoritative};
    bool gpu_complete{false};
    bool host_current{false};
    bool gpu_consumed{false};
    /// Command buffers that read the shadow instead of materialized guest RAM.
    u32 gpu_serves{0};
    u64 last_gpu_serve_tick{0};
    std::shared_ptr<GpuAuthorityShadow> shadow;
    std::shared_ptr<std::mutex> entry_mutex{std::make_shared<std::mutex>()};
    std::shared_ptr<std::condition_variable> cv{std::make_shared<std::condition_variable>()};
};

struct VirtualGpuFence {
    u64 virtual_fence_seq{0};
    u64 authority_seq{0};
    VAddr label_addr{0};
    u64 label_generation{0};
    u32 expected_value{0};
    u64 producer_tick{0};
    bool gpu_complete{false};
    bool host_label_written{false};
    bool wait_consumed{false};
};

/// Part of a guest range whose current bytes live in an authority shadow on the GPU.
struct GpuShadowPiece {
    VAddr addr{};
    u64 size{};
    /// Offset of the bytes at addr inside the download buffer.
    u64 buffer_offset{};
    std::shared_ptr<GpuAuthorityEntry> entry;
    std::shared_ptr<GpuAuthorityShadow> shadow;
};

using GpuShadowPieces = boost::container::small_vector<GpuShadowPiece, 4>;

struct GpuAuthorityIds {
    u64 authority_seq{};
    u64 virtual_fence_seq{};
    u64 label_generation{};
};

class GpuAuthorityTracker {
public:
    static GpuAuthorityTracker& Instance() noexcept;

    void SetRasterizer(Vulkan::Rasterizer* rasterizer_) noexcept;

    [[nodiscard]] bool IsGow3FastpathActive() const noexcept {
        const u8 state = gow3_fastpath_state.load(std::memory_order_relaxed);
        if (state == 0) [[unlikely]] {
            return ResolveGow3FastpathActive();
        }
        return state == 2;
    }

    [[nodiscard]] GpuAuthorityIds AllocateIds(VAddr label_addr);
    [[nodiscard]] u64 GetCurrentLabelGeneration(VAddr label_addr) const;

    void RegisterAuthority(const GpuAuthorityEntry& entry);

    /// Materializes authorities whose shadow completed a while ago and that nothing superseded
    /// or read since. Nothing waits for the GPU. Keeps the tracker, its read watches and the
    /// pinned shadows small once GPU consumers stop materializing authorities by accident.
    /// Command processor thread, no cache lock held.
    void RetireStaleAuthorities();
    void RegisterVirtualFence(const VirtualGpuFence& fence);

    [[nodiscard]] std::vector<std::shared_ptr<GpuAuthorityEntry>> FindOverlaps(VAddr addr, size_t size) const;
    [[nodiscard]] std::shared_ptr<GpuAuthorityEntry> GetAuthorityForImage(u64 image_uid, u64 version) const;
    [[nodiscard]] std::shared_ptr<GpuAuthorityEntry> GetAuthorityForRange(VAddr addr,
                                                                         size_t size) const;
    [[nodiscard]] std::shared_ptr<GpuAuthorityShadow> AcquireGpuShadowForImage(
        VAddr addr, size_t size);

    [[nodiscard]] std::shared_ptr<VirtualGpuFence> MatchVirtualWait(VAddr label_addr, u32 ref,
                                                                    u32 mask, u32 function);

    void SignalAsyncLabel(u64 virtual_fence_seq, u64 producer_tick);

    /// Completes a virtual fence now. Only the command processor can submit the fence's
    /// command buffer; on other threads a fence that was not submitted yet is left to complete
    /// on its own, or cancelled when its label lies in [dying_addr, dying_addr + dying_size),
    /// memory about to be unmapped that nobody can read the label from anymore.
    void EnsureVirtualFenceComplete(u64 virtual_fence_seq, VAddr dying_addr = 0,
                                    u64 dying_size = 0);
    void EnsureAllVirtualFencesComplete(VAddr dying_addr = 0, u64 dying_size = 0);

    /// Makes guest RAM current for a read of [addr, addr + size), materializing GPU
    /// authoritative ranges. With keep_gpu_servable, authorities whose shadow a GPU consumer can
    /// read directly (see CollectGpuShadowPieces) are left alone: the caller must then serve
    /// those bytes from the shadow instead of reading guest RAM.
    bool ResolveForRamRead(VAddr addr, size_t size, bool keep_gpu_servable = false);

    /// Collects the parts of [addr, addr + size) covered by GPU authoritative ranges whose shadow
    /// the GPU can copy from, so a GPU consumer gets the bytes without the command processor
    /// waiting for the producer. Returns false when an overlapping authority cannot be served
    /// that way (no live shadow, being materialized, overlapping another authority). The pieces
    /// are sorted and disjoint, and empty when no downloaded bytes are in the range. Nothing
    /// changes until CommitGpuShadowPieces.
    [[nodiscard]] bool CollectGpuShadowPieces(VAddr addr, size_t size, GpuShadowPieces& pieces);

    /// Keeps the shadows of the pieces alive until consumer_tick completes.
    void CommitGpuShadowPieces(const GpuShadowPieces& pieces, u64 consumer_tick);

    bool HandleCpuRead(VAddr fault_addr, size_t size);
    void HandleCpuWrite(VAddr addr, size_t size);
    void HandleUnmap(VAddr addr, size_t size);

private:
    /// Compares the serial of the running game once, which never changes while it runs.
    [[nodiscard]] bool ResolveGow3FastpathActive() const noexcept;
    void RetireVirtualFenceLocked(const std::shared_ptr<VirtualGpuFence>& fence);
    /// Entry mutex held.
    [[nodiscard]] bool IsGpuServableLocked(const GpuAuthorityEntry& entry) const;
    void RefreshAuthorityReadWatches(VAddr addr, size_t size);

    mutable std::recursive_mutex tracker_mutex;
    /// 0 until the serial was compared, then 1 for other games and 2 for God of War III.
    mutable std::atomic<u8> gow3_fastpath_state{0};
    Vulkan::Rasterizer* rasterizer{nullptr};
    std::vector<std::shared_ptr<GpuAuthorityEntry>> authorities;
    RangeSet authority_read_watch_ranges;
    std::unordered_map<VAddr, u64> label_generations;
    std::unordered_map<VAddr, std::shared_ptr<VirtualGpuFence>> active_virtual_fences;
    std::unordered_map<u64, std::shared_ptr<VirtualGpuFence>> virtual_fences_by_seq;
    u64 next_authority_seq{1};
    u64 next_virtual_fence_seq{1};
};

} // namespace VideoCore
