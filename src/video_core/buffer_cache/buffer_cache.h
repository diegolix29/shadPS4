// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <tsl/robin_map.h>
#include "common/hash.h"
#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/guest_copy_engine.h"
#include "video_core/multi_level_page_table.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
}

namespace VideoCore {

using BufferId = Common::SlotId;

class TextureCache;
struct Image;
class MemoryTracker;
class PageManager;

class BufferCache {
public:
    static constexpr u32 CACHING_PAGEBITS = 14;
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;
    static constexpr u64 DEVICE_PAGESIZE = 16_KB;
    static constexpr u64 CACHING_NUMPAGES = u64{1} << (40 - CACHING_PAGEBITS);
    static constexpr u64 BDA_PAGETABLE_SIZE = CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

    // Default values for garbage collection
    static constexpr s64 DEFAULT_TRIGGER_GC_MEMORY = 1_GB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 2_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    struct PageData {
        BufferId buffer_id{};
    };

    struct Traits {
        using Entry = PageData;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 16;
        static constexpr size_t PageBits = CACHING_PAGEBITS;
    };
    using PageTable = MultiLevelPageTable<Traits>;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        VAddr begin;
        VAddr end;
        bool has_stream_leap = false;
    };

    enum class StreamCopySource : u8 {
        Guest,
        Host,
        Zero,
    };

    struct StreamCopyRequest {
        StreamCopySource source_type{};
        VAddr guest_address{};
        const u8* host_address{};
        u32 size{};
        u64 alignment{1};
        bool deduplicate{true};
    };

    struct StreamCopyResult {
        Buffer* buffer{};
        u64 offset{};
    };

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         AmdGpu::Liverpool* liverpool, TextureCache& texture_cache,
                         PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return &bda_pagetable_buffer;
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager.GetFaultBuffer();
    }

    /// Retrieves the buffer with the specified id.
    [[nodiscard]] Buffer& GetBuffer(BufferId id) {
        return slot_buffers[id];
    }

    /// Retrieves a utility buffer optimized for specified memory usage.
    StreamBuffer& GetUtilityBuffer(MemoryUsage usage) noexcept {
        if (usage == MemoryUsage::Stream) {
            return stream_buffer;
        } else if (usage == MemoryUsage::Download) {
            return download_buffer;
        } else if (usage == MemoryUsage::DeviceLocal) {
            return device_buffer;
        } else {
            return staging_buffer;
        }
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size);

    /// A CPU write faulted after the page was invalidated. When the write extends a run of
    /// written pages, opens the pages after it too, so sequential writers fault once per run
    /// instead of once per page. Any thread.
    void OnCpuWriteFault(VAddr fault_addr);

    /// Keeps later draws of the submission from reusing the transient copies that overlap a
    /// range that was just written. Any thread.
    void InvalidateTransientReuse(VAddr device_addr, u64 size);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false);

    void PrepareVertexIndexBuffers(const Vulkan::GraphicsPipeline& pipeline, bool bind_index_buffer,
                                   u32 index_offset);

    void FinalizeVertexIndexBuffers(
        boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers);

    /// Writes a value to GPU buffer. (uses command buffer to temporarily store the data)
    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);

    /// Performs buffer to buffer data copy on the GPU.
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);

    /// Obtains a buffer for the specified region.
    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(VAddr gpu_addr, u32 size, bool is_written,
                                                       bool is_texel_buffer = false,
                                                       BufferId buffer_id = {});

    struct SyncRange {
        VAddr device_addr;
        u32 size;
    };

    /// Obtains a buffer for the specified region that is synchronized only in the given
    /// ranges, which must lie inside it. Reads upload all their ranges at once.
    [[nodiscard]] std::pair<Buffer*, u32> ObtainBufferForRanges(VAddr device_addr, u32 size,
                                                                std::span<const SyncRange> ranges,
                                                                bool is_written);

    /// Until EndUploadBarrierBatch, uploads leave their barriers to the caller. The caller must
    /// order earlier accesses before the uploads, unless needs_pre_barrier asks the first upload
    /// to do it, and must make the uploads visible when EndUploadBarrierBatch returns true.
    void BeginUploadBarrierBatch(bool needs_pre_barrier) noexcept {
        upload_barrier_batch = UploadBarrierBatch{.needs_pre_barrier = needs_pre_barrier};
    }

    /// Returns whether an upload was recorded since BeginUploadBarrierBatch.
    [[nodiscard]] bool EndUploadBarrierBatch() noexcept {
        const bool recorded = upload_barrier_batch && upload_barrier_batch->recorded;
        upload_barrier_batch.reset();
        return recorded;
    }

    void BeginStreamCopyBatch() noexcept;

    [[nodiscard]] u16 QueueStreamCopy(const StreamCopyRequest& request) {
        const bool invalid =
            stream_copy_finalized | (stream_copy_request_count >= MaxStreamCopyRequests) |
            (request.size == 0) | !std::has_single_bit(request.alignment) |
            (static_cast<u8>(request.source_type) > static_cast<u8>(StreamCopySource::Zero)) |
            ((request.source_type == StreamCopySource::Host) & (request.host_address == nullptr));
        if (invalid) [[unlikely]] {
            RejectStreamCopyRequest(request);
        }
        const u16 index = stream_copy_request_count++;
        stream_copy_requests[index] = request;
        return index;
    }

    void FinalizeStreamCopyBatch();

    [[nodiscard]] const StreamCopyResult& GetStreamCopyResult(u16 index) const {
        if (!stream_copy_finalized || index >= stream_copy_request_count) [[unlikely]] {
            RejectStreamCopyResult(index);
        }
        return stream_copy_results[index];
    }

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<Buffer*, u32> ObtainBufferForImage(VAddr gpu_addr, u32 size);

    /// Protected copy resolver of the guest copy engine. Writes the parts of op that GPU
    /// authority shadows hold into op.dst_buffer with GPU copies, so the command processor
    /// never reads (and faults on) guest RAM that the GPU still owns, and reads the other bytes
    /// of protected pages through the backing view. The bytes are the ones materializing guest
    /// RAM would produce. Returns false when the copy has to take the regular path.
    bool ServeGuestCopyFromGpuShadows(
        const GuestCopyEngine::Op& op,
        std::span<GuestCopyEngine::Op, GuestCopyEngine::MaxResolverRemainder> remainder,
        u32& remainder_count, u64& gpu_bytes, const PageManager& page_manager);

    /// Return true when a region is registered on the cache
    [[nodiscard]] bool IsRegionRegistered(VAddr addr, size_t size);

    /// Return true when a CPU region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Returns true when the newest contents are resident in either a buffer or an aliased image.
    [[nodiscard]] bool HasGpuReadSource(VAddr addr, size_t size);

    /// Marks a linear image as the newest GPU source without changing buffer-cache topology.
    [[nodiscard]] bool TrackImageReadback(Image& image, u32 copy_size);

    /// Marks RAM as current after a deferred image readback is materialized.
    void CompleteImageReadback(VAddr addr, u32 size);

    /// Return buffer id for the specified region
    BufferId FindBuffer(VAddr device_addr, u32 size);

    [[nodiscard]] bool IsBufferCacheEntryValid(BufferId id, u64 uid, VAddr address, u64 size) const;

    [[nodiscard]] u64 GetBufferUid(BufferId id) const;

    [[nodiscard]] u64 TopologyEpoch() const noexcept {
        return topology_epoch.load(std::memory_order_relaxed);
    }

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers in the specified range.
    void SynchronizeBuffersInRange(VAddr device_addr, u64 size);

    /// Synchronizes all buffers neede for DMA.
    void SynchronizeDmaBuffers();

    /// Runs the garbage collector.
    void RunGarbageCollector();

private:
    template <typename Func>
    void ForEachBufferInRange(VAddr device_addr, u64 size, Func&& func) {
        buffer_ranges.ForEachInRange(device_addr, size,
                                     [&](u64 page_start, u64 page_end, BufferId id) {
                                         Buffer& buffer = slot_buffers[id];
                                         func(id, buffer);
                                     });
    }

    inline bool IsBufferInvalid(BufferId buffer_id) const {
        return !buffer_id || slot_buffers[buffer_id].is_deleted;
    }

    template <bool async>
    void DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size);

    [[nodiscard]] OverlapResult ResolveOverlaps(VAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    BufferId CreateBuffer(VAddr device_addr, u32 wanted_size);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    bool SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// Uploads the CPU modified pages of several read ranges of a buffer in one transfer.
    void SynchronizeBufferRanges(Buffer& buffer, std::span<const SyncRange> ranges);

    /// Records the transfer of staged copies into the buffer.
    void RecordBufferUpload(Buffer& buffer, vk::Buffer src_buffer,
                            std::span<const vk::BufferCopy> copies, VAddr device_addr,
                            size_t total_size_bytes);

    vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                            size_t total_size_bytes);

    bool SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size);

    void WriteDataBuffer(Buffer& buffer, VAddr address, const void* value, u32 num_bytes);

    void TouchBuffer(const Buffer& buffer);

    void DeleteBuffer(BufferId buffer_id);

    /// Allocates transient read bytes; the range reaches the device copy with the submission.
    [[nodiscard]] std::pair<u8*, u64> MapTransient(u64 size, u64 alignment);

    /// Starts reuse lookups for the current submission: drops the copies of an earlier one and
    /// applies the writes reported since the last lookup.
    void BeginTransientReuse();
    /// Offset of a copy of the guest range made earlier in the submission, if still valid.
    [[nodiscard]] std::optional<u64> FindTransientReuse(VAddr address, u32 size,
                                                        u64 alignment) const;
    void RecordTransientReuse(VAddr address, u32 size, u64 offset);

    void ExecuteStreamCopyBatch(std::span<const StreamCopyRequest> requests,
                                std::span<StreamCopyResult> results);
    void ExecuteStreamCopySingle(const StreamCopyRequest& request, StreamCopyResult& result,
                                 bool defer_copies);

    struct StreamCopyScratch;
    struct StreamSliceReuseState;
    struct StreamBatchReuseState;
    struct VertexIndexState;
    /// Rebuilds the vertex input plan of a pipeline whose cached plans did not match.
    void RebuildVertexIndexPlan(u8 plan_index, const Vulkan::GraphicsPipeline& pipeline,
                                bool dynamic_input);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    FaultManager fault_manager;
    std::unique_ptr<MemoryTracker> memory_tracker;
    StreamBuffer staging_buffer;
    StreamBuffer stream_buffer;
    /// Host ring the CPU fills with the transient reads of draws and dispatches.
    StreamBuffer transient_read_buffer;
    /// Device copy of transient_read_buffer at the same offsets, which the shaders read. Each
    /// submission copies the ranges written for it first (see Scheduler::PrologueCopies).
    Buffer transient_device_buffer;
    /// Ranges of transient_read_buffer written since the last submission.
    boost::container::small_vector<vk::BufferCopy, 4> transient_uploads;
    /// Copies of guest ranges made by the current submission, keyed by address and size. With
    /// deferred copies the bytes cannot be compared, so a draw that reads the same range reuses
    /// the copy until something reports a write to it.
    struct TransientReuseEntry {
        u64 offset{};
        u64 generation{};
    };
    tsl::robin_map<u64, TransientReuseEntry, IntegerKeyHash> transient_reuse;
    /// Reusable copies of the submission that read each guest page, so a reported write only
    /// visits the copies of the pages it touched.
    tsl::robin_map<u64, boost::container::small_vector<u64, 4>, IntegerKeyHash>
        transient_reuse_pages;
    u64 transient_reuse_tick{};
    std::mutex transient_invalidation_mutex;
    std::vector<std::pair<VAddr, u64>> transient_invalidations;
    std::atomic<bool> transient_invalidation_pending{};
    StreamBuffer download_buffer;
    StreamBuffer device_buffer;
    Buffer gds_buffer;
    Buffer bda_pagetable_buffer;
    Common::SlotVector<Buffer> slot_buffers;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<BufferId, u64> lru_cache;
    RangeSet gpu_modified_ranges;
    struct UploadBarrierBatch {
        bool needs_pre_barrier{};
        bool recorded{};
    };
    std::optional<UploadBarrierBatch> upload_barrier_batch;
    RangeSet image_alias_ranges;
    RangeSet pending_image_readback_ranges;
    struct ImageSyncState {
        u64 buffer_uid{};
        u64 buffer_generation{};
        u64 image_uid{};
        u64 image_epoch{};

        bool operator==(const ImageSyncState&) const noexcept = default;
    };
    struct ImageSyncRecord {
        ImageSyncState state;
        /// Bytes of the image, from its start, synced at these versions.
        u32 begin{};
        u32 end{};
    };
    /// Buffer and image versions of the last image-to-buffer sync, keyed by image address.
    tsl::robin_map<VAddr, ImageSyncRecord, IntegerKeyHash> image_sync_states;
    SplitRangeMap<BufferId> buffer_ranges;
    PageTable page_table;
    std::atomic<u64> topology_epoch{1};

    /// Out of line checks of QueueStreamCopy and GetStreamCopyResult.
    void RejectStreamCopyRequest(const StreamCopyRequest& request) const;
    void RejectStreamCopyResult(u16 index) const;

    static constexpr size_t MaxStreamCopyRequests = 128;
    std::array<StreamCopyRequest, MaxStreamCopyRequests> stream_copy_requests{};
    std::array<StreamCopyResult, MaxStreamCopyRequests> stream_copy_results{};
    u16 stream_copy_request_count{};
    bool stream_copy_finalized{};
    std::unique_ptr<StreamCopyScratch> stream_copy_scratch;
    std::unique_ptr<StreamSliceReuseState> stream_slice_reuse;
    std::unique_ptr<StreamBatchReuseState> stream_batch_reuse;
    std::unique_ptr<VertexIndexState> vertex_index_state;
};

} // namespace VideoCore
