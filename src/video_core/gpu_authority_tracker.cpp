// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include <boost/container/small_vector.hpp>

#include "common/elf_info.h"
#include "core/memory.h"
#include "video_core/gpu_authority_tracker.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {

namespace {

constexpr VAddr ReadWatchPageMask = 4095;

std::pair<VAddr, size_t> GetReadWatchRange(VAddr addr, size_t size) {
    const VAddr begin = addr & ~ReadWatchPageMask;
    const VAddr end = (addr + size + ReadWatchPageMask) & ~ReadWatchPageMask;
    return {begin, end - begin};
}

constexpr bool HasGpuAuthority(GpuAuthorityState state) noexcept {
    return state == GpuAuthorityState::GpuAuthoritative ||
           state == GpuAuthorityState::Materializing;
}

/// After the producer completed, materializing guest RAM no longer waits for the GPU. A shadow
/// that more command buffers than this keep reading is materialized once instead of being
/// copied again for every consumer.
constexpr u32 MaxGpuServesAfterReady = 16;

/// Ticks after its shadow completed before an authority nothing superseded counts as stale.
/// A frame submits a dozen command buffers, and a render target produced again next frame
/// supersedes its old authority well before this.
constexpr u64 RetireAfterTicks = 48;
/// Above this many live authorities the oldest completed ones are retired regardless of age.
constexpr size_t MaxLiveAuthorities = 64;
/// Bytes materialized per retirement pass beyond the first authority.
constexpr u64 RetireBudgetBytes = 4ULL << 20;

} // namespace

GpuAuthorityTracker& GpuAuthorityTracker::Instance() noexcept {
    static GpuAuthorityTracker instance;
    return instance;
}

void GpuAuthorityTracker::SetRasterizer(Vulkan::Rasterizer* rasterizer_) noexcept {
    std::scoped_lock lock{tracker_mutex};
    rasterizer = rasterizer_;
}

SHAD_NO_INLINE bool GpuAuthorityTracker::ResolveGow3FastpathActive() const noexcept {
    const bool active = Common::ElfInfo::Instance().GameSerial() == "CUSA01715";
    gow3_fastpath_state.store(active ? 2 : 1, std::memory_order_relaxed);
    return active;
}

GpuAuthorityIds GpuAuthorityTracker::AllocateIds(VAddr label_addr) {
    std::scoped_lock lock{tracker_mutex};
    return {
        .authority_seq = next_authority_seq++,
        .virtual_fence_seq = next_virtual_fence_seq++,
        .label_generation = ++label_generations[label_addr],
    };
}

u64 GpuAuthorityTracker::GetCurrentLabelGeneration(VAddr label_addr) const {
    std::scoped_lock lock{tracker_mutex};
    const auto it = label_generations.find(label_addr);
    return it != label_generations.end() ? it->second : 0;
}

void GpuAuthorityTracker::RegisterAuthority(const GpuAuthorityEntry& entry) {
    if (!IsGow3FastpathActive()) {
        return;
    }
    std::scoped_lock lock{tracker_mutex};
    for (auto& old_entry : authorities) {
        std::scoped_lock entry_lock{*old_entry->entry_mutex};
        if (!HasGpuAuthority(old_entry->state)) {
            continue;
        }
        if (entry.guest_begin <= old_entry->guest_begin &&
            entry.guest_end >= old_entry->guest_end) {
            old_entry->state = GpuAuthorityState::Superseded;
            if (old_entry->shadow) {
                old_entry->shadow->Release();
                old_entry->shadow.reset();
            }
        }
    }
    std::erase_if(authorities, [](const auto& old_entry) {
        std::scoped_lock entry_lock{*old_entry->entry_mutex};
        return !HasGpuAuthority(old_entry->state);
    });
    authorities.push_back(std::make_shared<GpuAuthorityEntry>(entry));

    const auto [watch_addr, watch_size] =
        GetReadWatchRange(entry.guest_begin, entry.download_size);
    boost::container::small_vector<std::pair<VAddr, size_t>, 2> ranges_to_arm;
    authority_read_watch_ranges.ForEachNotInRange(
        watch_addr, watch_size,
        [&](VAddr addr, size_t size) { ranges_to_arm.emplace_back(addr, size); });
    authority_read_watch_ranges.Add(watch_addr, watch_size);
    if (rasterizer) {
        for (const auto [addr, size] : ranges_to_arm) {
            rasterizer->ArmSemanticReadWatch(addr, size);
        }
    }
}

void GpuAuthorityTracker::RetireStaleAuthorities() {
    if (!IsGow3FastpathActive() || rasterizer == nullptr) {
        return;
    }
    const u64 known_tick = rasterizer->KnownGpuTick();
    const u64 current_tick = rasterizer->CurrentTick();
    boost::container::small_vector<std::pair<VAddr, u32>, 8> stale;
    {
        std::scoped_lock lock{tracker_mutex};
        const bool crowded = authorities.size() > MaxLiveAuthorities;
        u64 bytes = 0;
        // Oldest first.
        for (const auto& entry : authorities) {
            std::scoped_lock entry_lock{*entry->entry_mutex};
            if (entry->state != GpuAuthorityState::GpuAuthoritative || !entry->shadow) {
                continue;
            }
            const u64 ready_tick = entry->shadow->ReadyTick();
            if (known_tick < ready_tick ||
                (!crowded && current_tick < ready_tick + RetireAfterTicks)) {
                continue;
            }
            if (!stale.empty() && bytes + entry->download_size > RetireBudgetBytes) {
                break;
            }
            bytes += entry->download_size;
            stale.emplace_back(entry->guest_begin, entry->download_size);
        }
    }
    for (const auto [addr, size] : stale) {
        // Materializing a range also materializes authorities overlapping it; skip ranges where
        // one of them still waits for the GPU.
        const auto overlaps = FindOverlaps(addr, size);
        const bool all_ready = std::ranges::all_of(overlaps, [&](const auto& entry) {
            std::scoped_lock entry_lock{*entry->entry_mutex};
            return entry->state == GpuAuthorityState::GpuAuthoritative && entry->shadow &&
                   known_tick >= entry->shadow->ReadyTick();
        });
        if (overlaps.empty() || !all_ready) {
            continue;
        }
        ResolveForRamRead(addr, size);
    }
}

void GpuAuthorityTracker::RegisterVirtualFence(const VirtualGpuFence& fence) {
    if (!IsGow3FastpathActive()) {
        return;
    }
    std::scoped_lock lock{tracker_mutex};
    if (const auto it = active_virtual_fences.find(fence.label_addr);
        it != active_virtual_fences.end()) {
        RetireVirtualFenceLocked(it->second);
    }
    auto fence_ptr = std::make_shared<VirtualGpuFence>(fence);
    active_virtual_fences[fence.label_addr] = fence_ptr;
    virtual_fences_by_seq[fence.virtual_fence_seq] = fence_ptr;
}

void GpuAuthorityTracker::RetireVirtualFenceLocked(
    const std::shared_ptr<VirtualGpuFence>& fence) {
    if (!fence->gpu_complete) {
        return;
    }
    const auto generation = label_generations.find(fence->label_addr);
    const bool stale_generation =
        generation == label_generations.end() || generation->second != fence->label_generation;
    if (!fence->wait_consumed && !stale_generation) {
        return;
    }
    const auto seq_it = virtual_fences_by_seq.find(fence->virtual_fence_seq);
    if (seq_it == virtual_fences_by_seq.end() || seq_it->second != fence) {
        return;
    }
    virtual_fences_by_seq.erase(seq_it);
    const auto active_it = active_virtual_fences.find(fence->label_addr);
    if (active_it != active_virtual_fences.end() && active_it->second == fence) {
        active_virtual_fences.erase(active_it);
    }
}

std::vector<std::shared_ptr<GpuAuthorityEntry>> GpuAuthorityTracker::FindOverlaps(
    VAddr addr, size_t size) const {
    std::vector<std::shared_ptr<GpuAuthorityEntry>> result;
    if (!IsGow3FastpathActive()) {
        return result;
    }
    const VAddr query_end = addr + size;
    std::scoped_lock lock{tracker_mutex};
    for (const auto& entry : authorities) {
        // The range of an entry never changes after registration.
        if (std::max(entry->guest_begin, addr) >= std::min(entry->guest_end, query_end)) {
            continue;
        }
        std::scoped_lock entry_lock{*entry->entry_mutex};
        if (HasGpuAuthority(entry->state)) {
            result.push_back(entry);
        }
    }
    return result;
}

std::shared_ptr<GpuAuthorityEntry> GpuAuthorityTracker::GetAuthorityForImage(
    u64 image_uid, u64 version) const {
    if (!IsGow3FastpathActive()) {
        return nullptr;
    }
    std::scoped_lock lock{tracker_mutex};
    for (auto it = authorities.rbegin(); it != authorities.rend(); ++it) {
        const auto& entry = *it;
        std::scoped_lock entry_lock{*entry->entry_mutex};
        if (entry->image_uid == image_uid && entry->resource_version == version &&
            HasGpuAuthority(entry->state)) {
            return entry;
        }
    }
    return nullptr;
}

std::shared_ptr<GpuAuthorityEntry> GpuAuthorityTracker::GetAuthorityForRange(
    VAddr addr, size_t size) const {
    if (!IsGow3FastpathActive() || size == 0) {
        return nullptr;
    }
    std::scoped_lock lock{tracker_mutex};
    for (auto it = authorities.rbegin(); it != authorities.rend(); ++it) {
        const auto& entry = *it;
        if (addr < entry->guest_begin || size > entry->download_size ||
            addr - entry->guest_begin > entry->download_size - size) {
            continue;
        }
        std::scoped_lock entry_lock{*entry->entry_mutex};
        if (entry->state != GpuAuthorityState::GpuAuthoritative) {
            continue;
        }
        return entry;
    }
    return nullptr;
}

bool GpuAuthorityTracker::IsGpuServableLocked(const GpuAuthorityEntry& entry) const {
    if (entry.state != GpuAuthorityState::GpuAuthoritative || !entry.shadow) {
        return false;
    }
    auto& shadow = *entry.shadow;
    {
        std::scoped_lock shadow_lock{shadow.data_mutex};
        if (shadow.IsReleased() || shadow.owned_data || shadow.data == nullptr ||
            shadow.guest_addr != entry.guest_begin) {
            return false;
        }
    }
    if (entry.gpu_serves >= MaxGpuServesAfterReady && rasterizer &&
        rasterizer->KnownGpuTick() >= shadow.ReadyTick()) {
        return false;
    }
    return true;
}

bool GpuAuthorityTracker::CollectGpuShadowPieces(VAddr addr, size_t size, GpuShadowPieces& pieces) {
    pieces.clear();
    if (!IsGow3FastpathActive() || size == 0) {
        return false;
    }
    const VAddr end = addr + size;
    std::scoped_lock lock{tracker_mutex};
    for (const auto& entry : authorities) {
        if (std::max(entry->guest_begin, addr) >= std::min(entry->guest_end, end)) {
            continue;
        }
        std::scoped_lock entry_lock{*entry->entry_mutex};
        if (!HasGpuAuthority(entry->state)) {
            continue;
        }
        if (!IsGpuServableLocked(*entry)) {
            pieces.clear();
            return false;
        }
        // Materializing writes only the shadow bytes; the rest of the entry range reads the same
        // from guest RAM before and after.
        const auto& shadow = entry->shadow;
        const VAddr piece_begin = std::max(shadow->guest_addr, addr);
        const VAddr piece_end = std::min<VAddr>(shadow->guest_addr + shadow->size, end);
        if (piece_begin >= piece_end) {
            continue;
        }
        pieces.push_back(GpuShadowPiece{
            .addr = piece_begin,
            .size = piece_end - piece_begin,
            .buffer_offset = shadow->buffer_offset + (piece_begin - shadow->guest_addr),
            .entry = entry,
            .shadow = shadow,
        });
    }
    std::ranges::sort(pieces, {}, &GpuShadowPiece::addr);
    for (size_t i = 1; i < pieces.size(); ++i) {
        // Overlapping authorities materialize in registration order; keep that path for them.
        if (pieces[i].addr < pieces[i - 1].addr + pieces[i - 1].size) {
            pieces.clear();
            return false;
        }
    }
    return true;
}

void GpuAuthorityTracker::CommitGpuShadowPieces(const GpuShadowPieces& pieces, u64 consumer_tick) {
    for (const auto& piece : pieces) {
        piece.shadow->ExtendLifetime(consumer_tick);
        std::scoped_lock entry_lock{*piece.entry->entry_mutex};
        piece.entry->gpu_consumed = true;
        if (piece.entry->last_gpu_serve_tick != consumer_tick) {
            piece.entry->last_gpu_serve_tick = consumer_tick;
            ++piece.entry->gpu_serves;
        }
    }
}

std::shared_ptr<GpuAuthorityShadow> GpuAuthorityTracker::AcquireGpuShadowForImage(
    VAddr addr, size_t size) {
    if (!IsGow3FastpathActive() || size != 512) {
        return nullptr;
    }

    std::scoped_lock lock{tracker_mutex};
    for (auto it = authorities.rbegin(); it != authorities.rend(); ++it) {
        const auto& entry = *it;
        if (entry->guest_begin != addr || entry->download_size != size) {
            continue;
        }
        std::scoped_lock entry_lock{*entry->entry_mutex};
        if (entry->state != GpuAuthorityState::GpuAuthoritative || !entry->shadow) {
            continue;
        }
        std::scoped_lock shadow_lock{entry->shadow->data_mutex};
        if (entry->shadow->IsReleased() || entry->shadow->owned_data) {
            continue;
        }
        entry->shadow->ExtendLifetime(rasterizer ? rasterizer->CurrentTick()
                                                 : entry->shadow->Tick());
        entry->gpu_consumed = true;
        return entry->shadow;
    }
    return nullptr;
}

void GpuAuthorityTracker::RefreshAuthorityReadWatches(VAddr addr, size_t size) {
    const auto [watch_addr, watch_size] = GetReadWatchRange(addr, size);
    boost::container::small_vector<std::pair<VAddr, size_t>, 4> ranges_to_disarm;
    std::scoped_lock lock{tracker_mutex};
    RangeSet required_ranges;
    for (const auto& entry : authorities) {
        std::scoped_lock entry_lock{*entry->entry_mutex};
        if (!HasGpuAuthority(entry->state)) {
            continue;
        }
        const auto [required_addr, required_size] =
            GetReadWatchRange(entry->guest_begin, entry->download_size);
        required_ranges.Add(required_addr, required_size);
    }
    authority_read_watch_ranges.ForEachInRange(
        watch_addr, watch_size, [&](VAddr range_addr, VAddr range_end) {
            required_ranges.ForEachNotInRange(
                range_addr, range_end - range_addr, [&](VAddr gap_addr, size_t gap_size) {
                    ranges_to_disarm.emplace_back(gap_addr, gap_size);
                });
        });
    for (const auto [range_addr, range_size] : ranges_to_disarm) {
        authority_read_watch_ranges.Subtract(range_addr, range_size);
        if (rasterizer) {
            rasterizer->DisarmSemanticReadWatch(range_addr, range_size);
        }
    }
}

std::shared_ptr<VirtualGpuFence> GpuAuthorityTracker::MatchVirtualWait(VAddr label_addr, u32 ref,
                                                                       u32 mask, u32 function) {
    if (!IsGow3FastpathActive()) {
        return nullptr;
    }
    // GOW3 wait signature: memory wait, Equal (function == 3) or GreaterThanEqual (function == 5), ref == 1, mask == 0xFFFFFFFF
    if ((function != 3 && function != 5) || ref != 1 || mask != 0xFFFFFFFF) {
        return nullptr;
    }
    std::scoped_lock lock{tracker_mutex};
    const auto it = active_virtual_fences.find(label_addr);
    if (it == active_virtual_fences.end()) {
        return nullptr;
    }
    auto fence = it->second;
    const auto gen_it = label_generations.find(label_addr);
    const u64 current_gen = gen_it != label_generations.end() ? gen_it->second : 0;
    if (fence->label_generation != current_gen || fence->wait_consumed) {
        return nullptr;
    }
    fence->wait_consumed = true;
    RetireVirtualFenceLocked(fence);
    return fence;
}

void GpuAuthorityTracker::SignalAsyncLabel(u64 virtual_fence_seq, u64 producer_tick) {
    std::shared_ptr<VirtualGpuFence> fence;
    bool wrote_label{};
    {
        std::scoped_lock lock{tracker_mutex};
        const auto it = virtual_fences_by_seq.find(virtual_fence_seq);
        if (it == virtual_fences_by_seq.end()) {
            return;
        }
        fence = it->second;
        if (fence->gpu_complete) {
            RetireVirtualFenceLocked(fence);
            return;
        }
        const auto gen_it = label_generations.find(fence->label_addr);
        const u64 current_gen = gen_it != label_generations.end() ? gen_it->second : 0;
        fence->gpu_complete = true;
        if (fence->label_generation == current_gen) {
            *reinterpret_cast<u32*>(fence->label_addr) = fence->expected_value;
            fence->host_label_written = true;
            wrote_label = true;
        }
    }

    if (wrote_label && rasterizer) {
        rasterizer->NotifyMemoryWrite(fence->label_addr, sizeof(u32),
                                      VideoCore::MemoryWriteSource::CommandProcessor);
    }
    {
        std::scoped_lock lock{tracker_mutex};
        RetireVirtualFenceLocked(fence);
    }
}

void GpuAuthorityTracker::EnsureVirtualFenceComplete(u64 virtual_fence_seq, VAddr dying_addr,
                                                     u64 dying_size) {
    if (!IsGow3FastpathActive()) {
        return;
    }
    std::shared_ptr<VirtualGpuFence> fence;
    {
        std::scoped_lock lock{tracker_mutex};
        const auto it = virtual_fences_by_seq.find(virtual_fence_seq);
        if (it == virtual_fences_by_seq.end()) {
            return;
        }
        fence = it->second;
        if (fence->gpu_complete) {
            RetireVirtualFenceLocked(fence);
            return;
        }
    }
    if (rasterizer) {
        const u64 current_tick = rasterizer->CurrentTick();
        if (fence->producer_tick >= current_tick && !rasterizer->IsGpuThread()) {
            // Ending the command buffer from here would race with the command processor, and
            // waiting for it to do so could deadlock: unmaps hold the page manager lock.
            if (fence->label_addr >= dying_addr && fence->label_addr - dying_addr < dying_size) {
                std::scoped_lock lock{tracker_mutex};
                fence->gpu_complete = true;
                RetireVirtualFenceLocked(fence);
            }
            return;
        }
        if (fence->producer_tick >= current_tick) {
            rasterizer->Flush();
        }
        if (rasterizer->KnownGpuTick() < fence->producer_tick) {
            rasterizer->WaitTick(fence->producer_tick);
        }
    }
    SignalAsyncLabel(fence->virtual_fence_seq, fence->producer_tick);
}

void GpuAuthorityTracker::EnsureAllVirtualFencesComplete(VAddr dying_addr, u64 dying_size) {
    if (!IsGow3FastpathActive()) {
        return;
    }
    std::vector<u64> pending_seqs;
    {
        std::scoped_lock lock{tracker_mutex};
        for (const auto& [seq, fence] : virtual_fences_by_seq) {
            if (!fence->gpu_complete) {
                pending_seqs.push_back(seq);
            }
        }
    }
    for (u64 seq : pending_seqs) {
        EnsureVirtualFenceComplete(seq, dying_addr, dying_size);
    }
}

namespace {
thread_local u64 tls_active_materializing_authority_seq{0};

class ScopedAuthorityMaterialization {
public:
    explicit ScopedAuthorityMaterialization(u64 authority_seq) noexcept
        : prev_seq(tls_active_materializing_authority_seq) {
        tls_active_materializing_authority_seq = authority_seq;
    }
    ~ScopedAuthorityMaterialization() noexcept {
        tls_active_materializing_authority_seq = prev_seq;
    }
    ScopedAuthorityMaterialization(const ScopedAuthorityMaterialization&) = delete;
    ScopedAuthorityMaterialization& operator=(const ScopedAuthorityMaterialization&) = delete;

private:
    u64 prev_seq{0};
};

[[nodiscard]] bool IsCurrentThreadMaterializing(u64 authority_seq) noexcept {
    return tls_active_materializing_authority_seq != 0 &&
           tls_active_materializing_authority_seq == authority_seq;
}
} // anonymous namespace

bool GpuAuthorityTracker::ResolveForRamRead(VAddr addr, size_t size, bool keep_gpu_servable) {
    if (!IsGow3FastpathActive()) {
        return true;
    }
    const auto overlaps = FindOverlaps(addr, size);
    if (overlaps.empty()) {
        return true;
    }
    bool all_succeeded = true;

    for (const auto& entry : overlaps) {
        const VAddr overlap_begin = std::max(entry->guest_begin, addr);
        const VAddr overlap_end = std::min(entry->guest_end, addr + size);
        const u64 overlap_size = overlap_end > overlap_begin ? overlap_end - overlap_begin : 0;
        if (overlap_size == 0) {
            continue;
        }

        std::unique_lock entry_lk{*entry->entry_mutex};

        // Materializing writes only the downloaded bytes. A read of the rest of the entry range
        // (row padding past the download) finds guest RAM current already.
        if (std::max<VAddr>(entry->guest_begin, addr) >=
            std::min<VAddr>(entry->guest_begin + entry->download_size, addr + size)) {
            continue;
        }

        if (IsCurrentThreadMaterializing(entry->authority_seq)) {
            // Internal access from this authority's own materializer: bypass self-wait
            continue;
        }

        if (entry->state == GpuAuthorityState::HostCurrent) {
            continue;
        }

        if (entry->state == GpuAuthorityState::Superseded || entry->state == GpuAuthorityState::Failed) {
            all_succeeded = false;
            continue;
        }

        if (entry->state == GpuAuthorityState::Materializing) {
            entry->cv->wait(entry_lk, [&] { return entry->state != GpuAuthorityState::Materializing; });
            if (entry->state != GpuAuthorityState::HostCurrent) {
                all_succeeded = false;
            }
            continue;
        }

        if (entry->state == GpuAuthorityState::GpuAuthoritative) {
            if (keep_gpu_servable && IsGpuServableLocked(*entry)) {
                continue;
            }
            entry->state = GpuAuthorityState::Materializing;
            const VAddr g_begin = entry->guest_begin;
            const u32 dl_size = entry->download_size;
            const u64 auth_seq = entry->authority_seq;
            auto authority_shadow = entry->shadow;

            // The shadow copy was recorded when the authority was promoted. Waiting for its
            // timeline never requires a synchronous command on the GCP thread.
            entry_lk.unlock();
            if (rasterizer && authority_shadow) {
                rasterizer->GetTextureCache().WaitGpuAuthorityShadow(authority_shadow);
            }
            entry_lk.lock();

            s8 val_equal = -1;
            bool success = false;
            if (entry->state == GpuAuthorityState::Materializing) {
                ScopedAuthorityMaterialization scoped_mat{auth_seq};
                if (rasterizer && authority_shadow) {
                    success = rasterizer->GetTextureCache().MaterializeGpuAuthority(
                        authority_shadow, g_begin, dl_size, &val_equal);
                }
                entry->state = success ? GpuAuthorityState::HostCurrent
                                       : GpuAuthorityState::Failed;
                entry->host_current = success;
            }
            if (entry->shadow) {
                entry->shadow->Release();
                entry->shadow.reset();
            }
            entry->cv->notify_all();
            if (!success) {
                all_succeeded = false;
            }
        }
    }
    for (const auto& entry : overlaps) {
        RefreshAuthorityReadWatches(entry->guest_begin, entry->download_size);
    }
    return all_succeeded;
}

bool GpuAuthorityTracker::HandleCpuRead(VAddr fault_addr, size_t size) {
    if (!IsGow3FastpathActive()) {
        return false;
    }
    const auto [watch_addr, watch_size] =
        GetReadWatchRange(fault_addr, size > 0 ? size : 4);
    const auto overlaps = FindOverlaps(watch_addr, watch_size);
    if (overlaps.empty()) {
        RefreshAuthorityReadWatches(watch_addr, watch_size);
        return false;
    }
    for (const auto& entry : overlaps) {
        if (IsCurrentThreadMaterializing(entry->authority_seq)) {
            // Internal read during materialization of this authority: unprotect and continue
            if (rasterizer) {
                rasterizer->DisarmSemanticReadWatch(fault_addr, size > 0 ? size : 4);
            }
            return true;
        }
    }
    return ResolveForRamRead(watch_addr, watch_size);
}

void GpuAuthorityTracker::HandleCpuWrite(VAddr addr, size_t size) {
    if (!IsGow3FastpathActive()) {
        return;
    }
    const size_t access_size = size > 0 ? size : 4;
    const auto [watch_addr, watch_size] = GetReadWatchRange(addr, access_size);
    const auto overlaps = FindOverlaps(watch_addr, watch_size);
    const bool external_overlap = std::ranges::any_of(overlaps, [](const auto& entry) {
        return !IsCurrentThreadMaterializing(entry->authority_seq);
    });
    if (external_overlap) {
        ResolveForRamRead(watch_addr, watch_size);
    }
}

void GpuAuthorityTracker::HandleUnmap(VAddr addr, size_t size) {
    if (!IsGow3FastpathActive()) {
        return;
    }
    EnsureAllVirtualFencesComplete(addr, size);
    const auto overlaps = FindOverlaps(addr, size > 0 ? size : 4);
    for (const auto& entry : overlaps) {
        std::unique_lock entry_lk{*entry->entry_mutex};
        entry->state = GpuAuthorityState::Superseded;
        if (entry->shadow) {
            entry->shadow->Release();
            entry->shadow.reset();
        }
        entry->cv->notify_all();
    }
    for (const auto& entry : overlaps) {
        RefreshAuthorityReadWatches(entry->guest_begin, entry->download_size);
    }
}

} // namespace VideoCore
