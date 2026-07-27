// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

#include <boost/container/static_vector.hpp>
#include "common/alignment.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/gpu_authority_tracker.h"
#include "video_core/guest_copy_engine.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {

static constexpr size_t DataShareBufferSize = 64_KB;
static constexpr size_t StagingBufferSize = 512_MB;
static constexpr size_t DownloadBufferSize = 256_MB;
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;
/// Smaller zero fills are cheaper inline than as a deferred copy operation.
static constexpr u64 DeferredZeroFillThreshold = 4_KB;

static SHAD_NO_INLINE void ValidateStreamCopyRequest(
    const bool finalized, const u16 request_count, const size_t max_requests,
    const BufferCache::StreamCopyRequest& request) {
    ASSERT(!finalized);
    ASSERT(request_count < max_requests);
    ASSERT(request.size != 0);
    ASSERT(request.alignment != 0 && std::has_single_bit(request.alignment));
    ASSERT(request.source_type == BufferCache::StreamCopySource::Guest ||
           request.source_type == BufferCache::StreamCopySource::Host ||
           request.source_type == BufferCache::StreamCopySource::Zero);
    ASSERT(request.source_type != BufferCache::StreamCopySource::Host ||
           request.host_address != nullptr);
}

static SHAD_NO_INLINE void ValidateStreamCopyBatch(const size_t request_count,
                                                   const size_t result_count,
                                                   const size_t max_requests) {
    ASSERT(request_count == result_count);
    ASSERT(request_count <= max_requests);
}

static SHAD_NO_INLINE void ValidateStreamCopyNotFinalized(const bool finalized) {
    ASSERT(!finalized);
}

static SHAD_NO_INLINE void ValidateStreamCopyResult(const bool finalized, const u16 index,
                                                    const u16 request_count) {
    ASSERT(finalized);
    ASSERT(index < request_count);
}

static SHAD_NO_INLINE void ValidateStreamCopyDestination(const void* destination) {
    ASSERT(destination != nullptr);
}

static SHAD_NO_INLINE void ValidateStreamCopyMapping(const u16 canonical,
                                                     const u16 no_canonical_copy) {
    ASSERT(canonical != no_canonical_copy);
}

static SHAD_NO_INLINE void ValidateVertexRangeSize(u64 size) {
    ASSERT(size <= std::numeric_limits<u32>::max());
}

static SHAD_NO_INLINE void ValidateVertexIndexState(bool prepared, bool stream_copy_finalized) {
    ASSERT(prepared);
    ASSERT(stream_copy_finalized);
}

static SHAD_NO_INLINE void ValidateVertexIndexBuffer(const void* buffer) {
    ASSERT(buffer != nullptr);
}

struct BufferCache::StreamCopyScratch {
    static constexpr size_t HashTableSize = 256;
    static constexpr u16 NoCanonicalCopy = std::numeric_limits<u16>::max();
    static_assert(std::has_single_bit(HashTableSize));

    struct SourceKey {
        u64 address{};
        u32 size{};
        StreamCopySource type{};

        bool operator==(const SourceKey&) const = default;
    };

    struct CanonicalSource {
        SourceKey key{};
        u64 alignment{1};
        bool deduplicate{};
    };

    struct CanonicalPlacement {
        u64 relative_offset{};
        u64 absolute_offset{};
        u32 capture_offset{};
        u16 reuse_set{};
        u8 reuse_way{std::numeric_limits<u8>::max()};
        bool captured{};
        bool compare_candidate{};
        bool reused{};
    };

    struct RequestMap {
        u16 canonical{NoCanonicalCopy};
        u32 source_offset{};
    };

    struct HashSlot {
        u16 generation{};
        u16 canonical{};
    };

    std::array<CanonicalSource, MaxStreamCopyRequests> canonical_sources{};
    std::array<CanonicalPlacement, MaxStreamCopyRequests> canonical_placements{};
    std::array<RequestMap, MaxStreamCopyRequests> request_map{};
    std::array<HashSlot, HashTableSize> hash_table{};
    std::array<Core::MemoryManager::SparseCopyRequest, MaxStreamCopyRequests> guest_copies{};
    std::array<GuestCopyEngine::Op, MaxStreamCopyRequests> deferred_copies{};
    u16 hash_generation{1};
};

struct BufferCache::StreamSliceReuseState {
    static constexpr size_t WayCount = 2;
    static constexpr size_t SetCount = 64;
    static constexpr size_t EntryCount = WayCount * SetCount;
    static_assert(std::has_single_bit(SetCount));

    struct Entry {
        VAddr address{};
        u64 generation{};
        u64 tick{};
        u32 size{};
        u32 offset{};
        bool valid{};
    };

    struct Set {
        std::array<Entry, WayCount> ways{};
        u8 next_replacement{};
    };

    std::array<Set, SetCount> sets{};
    std::unique_ptr<u8[]> shadow =
        std::make_unique_for_overwrite<u8[]>(EntryCount * CACHING_PAGESIZE);
    std::array<u8, CACHING_PAGESIZE> scratch{};

    [[nodiscard]] u8* Shadow(size_t set_index, size_t way) noexcept {
        return shadow.get() + (set_index * WayCount + way) * CACHING_PAGESIZE;
    }
};

struct BufferCache::StreamBatchReuseState {
    static constexpr size_t WayCount = 2;
    static constexpr size_t SetCount = 64;
    static constexpr u8 NoWay = std::numeric_limits<u8>::max();
    static constexpr u8 MismatchThreshold = 2;
    static constexpr u8 CooldownLength = 8;
    static_assert(std::has_single_bit(SetCount));

    struct Entry {
        VAddr address{};
        u64 generation{};
        u64 tick{};
        u32 size{};
        u32 offset{};
        u8 mismatch_streak{};
        u8 cooldown{};
        bool shadow_valid{};
        bool valid{};
    };

    struct Set {
        std::array<Entry, WayCount> ways{};
        u8 next_replacement{};
    };

    [[nodiscard]] static size_t SetIndex(VAddr address, u64 size) noexcept {
        u64 value = (address >> 4) ^ (address >> 31) ^ (size * 0x9E3779B185EBCA87ULL);
        value ^= value >> 29;
        return static_cast<size_t>(value) & (SetCount - 1);
    }

    [[nodiscard]] Entry* Find(VAddr address, u64 size, size_t& set_index,
                              size_t& way_index) noexcept {
        set_index = SetIndex(address, size);
        auto& set = sets[set_index];
        for (way_index = 0; way_index < WayCount; ++way_index) {
            auto& entry = set.ways[way_index];
            if (entry.valid && entry.address == address && entry.size == size) {
                return &entry;
            }
        }
        way_index = NoWay;
        return nullptr;
    }

    [[nodiscard]] Entry& Select(VAddr address, u64 size) noexcept {
        size_t set_index{};
        size_t way_index{};
        if (Entry* entry = Find(address, size, set_index, way_index)) {
            return *entry;
        }
        auto& set = sets[set_index];
        for (auto& entry : set.ways) {
            if (!entry.valid) {
                return entry;
            }
        }
        auto& entry = set.ways[set.next_replacement];
        set.next_replacement = (set.next_replacement + 1) % WayCount;
        return entry;
    }

    [[nodiscard]] u8* Shadow(size_t set_index, size_t way_index) noexcept {
        return shadow.get() + (set_index * WayCount + way_index) * CACHING_PAGESIZE;
    }

    std::array<Set, SetCount> sets{};
    std::unique_ptr<u8[]> shadow =
        std::make_unique_for_overwrite<u8[]>(SetCount * WayCount * CACHING_PAGESIZE);
    std::unique_ptr<u8[]> capture =
        std::make_unique_for_overwrite<u8[]>(MaxStreamCopyRequests * CACHING_PAGESIZE);
    std::array<Core::MemoryManager::SparseCopyRequest, MaxStreamCopyRequests> capture_copies{};
    std::array<u16, MaxStreamCopyRequests> capture_canonicals{};
};

struct BufferCache::VertexIndexState {
    static constexpr u16 NoStreamCopy = std::numeric_limits<u16>::max();

    struct BufferToken {
        BufferId id{};
        u64 uid{};
        u64 topology_epoch{};
    };

    struct BufferRange {
        VAddr base_address{};
        VAddr end_address{};
        BufferToken token{};
        vk::Buffer vk_buffer{};
        Buffer* buffer{};
        u64 offset{};
        u32 binding_mask{};
        u32 size{};
        u16 stream_index{NoStreamCopy};
        bool was_gpu_modified{};

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    struct IndexBinding {
        Buffer* buffer{};
        VAddr address{};
        u64 offset{};
        u32 size{};
        u16 stream_index{NoStreamCopy};
        vk::IndexType type{vk::IndexType::eUint32};
        bool was_gpu_modified{};
    };

    struct Plan {
        Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
        Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
        Vulkan::VertexInputs<AmdGpu::Buffer> guest_buffers;
        Vulkan::VertexInputs<BufferRange> ranges_merged;
        Vulkan::VertexInputs<u16> input_keys;
        u64 identity{};
        u32 step_rate_0{};
        u32 step_rate_1{};
        bool valid{};
    };
    std::array<Plan, 2> plans{};
    u8 active_plan{};
    BufferToken index_token{};
    IndexBinding index{};
    bool bind_index_buffer{};
    bool prepared{};

    u64 emitted_tick{std::numeric_limits<u64>::max()};
    bool vertex_input_valid{};
    bool vertex_buffers_valid{};
    bool index_buffer_valid{};
    u32 emitted_attribute_count{};
    u32 emitted_binding_count{};
    u32 emitted_buffer_count{};
    std::array<vk::VertexInputAttributeDescription2EXT, Vulkan::MaxVertexBufferCount>
        emitted_attributes{};
    std::array<vk::VertexInputBindingDescription2EXT, Vulkan::MaxVertexBufferCount>
        emitted_bindings{};
    std::array<vk::Buffer, Vulkan::MaxVertexBufferCount> emitted_buffers{};
    std::array<vk::DeviceSize, Vulkan::MaxVertexBufferCount> emitted_offsets{};
    std::array<vk::DeviceSize, Vulkan::MaxVertexBufferCount> emitted_sizes{};
    std::array<vk::DeviceSize, Vulkan::MaxVertexBufferCount> emitted_strides{};
    std::array<vk::Buffer, Vulkan::MaxVertexBufferCount> host_buffers{};
    std::array<vk::DeviceSize, Vulkan::MaxVertexBufferCount> host_offsets{};
    std::array<vk::DeviceSize, Vulkan::MaxVertexBufferCount> host_sizes{};
    std::array<vk::DeviceSize, Vulkan::MaxVertexBufferCount> host_strides{};
    vk::Buffer emitted_index_buffer{};
    vk::DeviceSize emitted_index_offset{};
    vk::IndexType emitted_index_type{vk::IndexType::eUint32};
};

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         AmdGpu::Liverpool* liverpool_, TextureCache& texture_cache_,
                         PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      fault_manager{instance, scheduler, *this, CACHING_PAGEBITS, CACHING_NUMPAGES},
      staging_buffer{instance, scheduler, MemoryUsage::Upload, StagingBufferSize},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      // The CPU writes transient reads to host memory, where its writes are cheapest, and the
      // shaders read them from device memory, which they would otherwise reach across the bus.
      transient_read_buffer{instance, scheduler, MemoryUsage::Upload, UboStreamBufferSize, true},
      transient_device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                              UboStreamBufferSize, true},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, MemoryUsage::DeviceLocal,
                           0,        AllFlags,  BDA_PAGETABLE_SIZE} {
    Vulkan::SetObjectName(instance.GetDevice(), gds_buffer.Handle(), "GDS Buffer");
    Vulkan::SetObjectName(instance.GetDevice(), transient_read_buffer.Handle(),
                          "Transient Read Stream");
    Vulkan::SetObjectName(instance.GetDevice(), transient_device_buffer.Handle(),
                          "Transient Read Device");
    scheduler.SetPrologueCollector(
        [](void* context, Vulkan::Scheduler::PrologueCopies& copies) {
            auto& cache = *static_cast<BufferCache*>(context);
            if (cache.transient_uploads.empty()) {
                return;
            }
            copies.src = cache.transient_read_buffer.Handle();
            copies.dst = cache.transient_device_buffer.Handle();
            copies.regions = cache.transient_uploads;
            cache.transient_uploads.clear();
        },
        this);
    Vulkan::SetObjectName(instance.GetDevice(), bda_pagetable_buffer.Handle(),
                          "BDA Page Table Buffer");

    memory_tracker = std::make_unique<MemoryTracker>(tracker);
    stream_copy_scratch = std::make_unique<StreamCopyScratch>();
    stream_slice_reuse = std::make_unique<StreamSliceReuseState>();
    stream_batch_reuse = std::make_unique<StreamBatchReuseState>();
    vertex_index_state = std::make_unique<VertexIndexState>();

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    // Set up garbage collection parameters
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = DEFAULT_TRIGGER_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    trigger_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_TRIGGER_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
}

BufferCache::~BufferCache() {
    scheduler.SetPrologueCollector(nullptr, nullptr);
}

std::pair<u8*, u64> BufferCache::MapTransient(u64 size, u64 alignment) {
    const auto mapping = transient_read_buffer.Map(size, alignment);
    const u64 begin = mapping.second;
    const u64 end = begin + size;
    // The ring allocates forward until it wraps, so ranges mostly extend the last one.
    if (!transient_uploads.empty() && transient_uploads.back().srcOffset <= begin) {
        auto& last = transient_uploads.back();
        last.size = std::max(last.srcOffset + last.size, end) - last.srcOffset;
    } else {
        transient_uploads.push_back(vk::BufferCopy{
            .srcOffset = begin,
            .dstOffset = begin,
            .size = size,
        });
    }
    // A submission large enough to wrap onto its own ranges needs them merged: copy regions
    // may not overlap.
    for (size_t index = 0; index + 1 < transient_uploads.size();) {
        auto& added = transient_uploads.back();
        const auto& other = transient_uploads[index];
        if (other.srcOffset < added.srcOffset + added.size &&
            added.srcOffset < other.srcOffset + other.size) {
            const u64 merged_begin = std::min(other.srcOffset, added.srcOffset);
            const u64 merged_end =
                std::max(other.srcOffset + other.size, added.srcOffset + added.size);
            added.srcOffset = merged_begin;
            added.dstOffset = merged_begin;
            added.size = merged_end - merged_begin;
            transient_uploads.erase(transient_uploads.begin() + index);
            continue;
        }
        ++index;
    }
    return mapping;
}

/// Page granularity of the write checks on reusable transient copies.
static constexpr u32 TransientReusePageBits = 12;

[[nodiscard]] static constexpr u64 TransientReuseKey(VAddr address, u32 size) noexcept {
    // Guest addresses fit in 48 bits and reusable copies in 16.
    return (address << 16) | size;
}

[[nodiscard]] static constexpr VAddr TransientReuseKeyAddress(u64 key) noexcept {
    return key >> 16;
}

[[nodiscard]] static constexpr u64 TransientReuseKeySize(u64 key) noexcept {
    return key & 0xFFFF;
}

void BufferCache::InvalidateTransientReuse(VAddr device_addr, u64 size) {
    std::scoped_lock lock{transient_invalidation_mutex};
    transient_invalidations.emplace_back(device_addr, size);
    transient_invalidation_pending.store(true, std::memory_order_release);
}

void BufferCache::BeginTransientReuse() {
    const u64 tick = scheduler.CurrentTick();
    if (tick != transient_reuse_tick) {
        transient_reuse_tick = tick;
        transient_reuse.clear();
        transient_reuse_pages.clear();
    }
    if (!transient_invalidation_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{transient_invalidation_mutex};
    transient_invalidation_pending.store(false, std::memory_order_relaxed);
    for (const auto& [address, size] : transient_invalidations) {
        const VAddr end = address + std::max<u64>(size, 1);
        // A copy that overlaps the write reads one of its pages. Only the copies whose bytes
        // were written are dropped; the others stay reusable.
        for (u64 page = address >> TransientReusePageBits;
             page <= (end - 1) >> TransientReusePageBits; ++page) {
            const auto page_it = transient_reuse_pages.find(page);
            if (page_it == transient_reuse_pages.end()) {
                continue;
            }
            auto& keys = page_it.value();
            for (size_t index = 0; index < keys.size();) {
                const u64 key = keys[index];
                const VAddr copy_address = TransientReuseKeyAddress(key);
                const VAddr copy_end = copy_address + TransientReuseKeySize(key);
                if (copy_address < end && address < copy_end) {
                    transient_reuse.erase(key);
                    keys[index] = keys.back();
                    keys.pop_back();
                } else {
                    ++index;
                }
            }
            if (keys.empty()) {
                transient_reuse_pages.erase(page_it);
            }
        }
    }
    transient_invalidations.clear();
}

std::optional<u64> BufferCache::FindTransientReuse(VAddr address, u32 size,
                                                   u64 alignment) const {
    const auto it = transient_reuse.find(TransientReuseKey(address, size));
    if (it == transient_reuse.end() ||
        it->second.generation != transient_read_buffer.Generation() ||
        !Common::IsAligned(it->second.offset, alignment)) {
        return std::nullopt;
    }
    return it->second.offset;
}

void BufferCache::RecordTransientReuse(VAddr address, u32 size, u64 offset) {
    const u64 key = TransientReuseKey(address, size);
    const TransientReuseEntry entry{
        .offset = offset,
        .generation = transient_read_buffer.Generation(),
    };
    if (!transient_reuse.insert_or_assign(key, entry).second) {
        // The pages already list the key.
        return;
    }
    const u64 last_page = (address + size - 1) >> TransientReusePageBits;
    for (u64 page = address >> TransientReusePageBits; page <= last_page; ++page) {
        transient_reuse_pages[page].push_back(key);
    }
}

void BufferCache::BeginStreamCopyBatch() noexcept {
    stream_copy_request_count = 0;
    stream_copy_finalized = false;
    vertex_index_state->prepared = false;
}

SHAD_NO_INLINE void BufferCache::RejectStreamCopyRequest(const StreamCopyRequest& request) const {
    ValidateStreamCopyRequest(stream_copy_finalized, stream_copy_request_count,
                              MaxStreamCopyRequests, request);
}

void BufferCache::FinalizeStreamCopyBatch() {
    if (stream_copy_finalized) [[unlikely]] {
        ValidateStreamCopyNotFinalized(stream_copy_finalized);
    }
    if (stream_copy_request_count != 0) {
        ExecuteStreamCopyBatch(
            std::span<const StreamCopyRequest>{stream_copy_requests.data(),
                                               stream_copy_request_count},
            std::span<StreamCopyResult>{stream_copy_results.data(), stream_copy_request_count});
    }
    stream_copy_finalized = true;
}

SHAD_NO_INLINE void BufferCache::RejectStreamCopyResult(u16 index) const {
    ValidateStreamCopyResult(stream_copy_finalized, index, stream_copy_request_count);
}

SHAD_NO_INLINE void BufferCache::ExecuteStreamCopySingle(const StreamCopyRequest& request,
                                                         StreamCopyResult& result,
                                                         bool defer_copies) {
    auto& reuse = *stream_batch_reuse;
    auto& copy_engine = GuestCopyEngine::Instance();
    const bool reusable = defer_copies && request.deduplicate &&
                          request.source_type == StreamCopySource::Guest &&
                          request.size <= CACHING_PAGESIZE;
    if (reusable) {
        if (const auto offset =
                FindTransientReuse(request.guest_address, request.size, request.alignment)) {
            result = {.buffer = &transient_device_buffer, .offset = *offset};
            return;
        }
    }
    StreamBatchReuseState::Entry* reuse_entry{};
    bool captured{};
    {
        if (!defer_copies && request.deduplicate &&
            request.source_type == StreamCopySource::Guest &&
            request.size <= CACHING_PAGESIZE) {
            size_t set_index{};
            size_t way_index{};
            auto* entry =
                reuse.Find(request.guest_address, request.size, set_index, way_index);
            if (entry != nullptr && entry->generation == transient_read_buffer.Generation() &&
                entry->tick == scheduler.CurrentTick() &&
                Common::IsAligned(entry->offset, request.alignment)) {
                reuse_entry = entry;
                if (entry->cooldown != 0) {
                    --entry->cooldown;
                } else {
                    const Core::MemoryManager::SparseCopyRequest copy{
                        .source = request.guest_address,
                        .destination = reuse.capture.get(),
                        .size = request.size,
                    };
                    {
                        memory->CopySparseMemoryBatch(
                            std::span<const Core::MemoryManager::SparseCopyRequest>{&copy, 1},
                            request.size, false);
                    }
                    captured = true;
                    if (!entry->shadow_valid) {
                        entry->mismatch_streak = 0;
                    } else {
                        if (std::memcmp(reuse.capture.get(),
                                        reuse.Shadow(set_index, way_index),
                                        request.size) == 0) {
                            entry->mismatch_streak = 0;
                            entry->cooldown = 0;
                            result = {
                                .buffer = &transient_device_buffer,
                                .offset = entry->offset,
                            };
                            return;
                        }
                        if (++entry->mismatch_streak >=
                            StreamBatchReuseState::MismatchThreshold) {
                            entry->mismatch_streak = 0;
                            entry->cooldown = StreamBatchReuseState::CooldownLength;
                        }
                    }
                }
            }
        }
    }
    const auto [destination, offset] =
        MapTransient(request.size, request.alignment);
    if (destination == nullptr) [[unlikely]] {
        ValidateStreamCopyDestination(destination);
    }
    switch (request.source_type) {
    case StreamCopySource::Guest:
        if (captured) {
            std::memcpy(destination, reuse.capture.get(), request.size);
        } else if (defer_copies) {
            const GuestCopyEngine::Op op{
                .source = request.guest_address,
                .destination = destination,
                .size = request.size,
                .dst_buffer = GuestCopyEngine::BufferId(transient_device_buffer.Handle()),
                .dst_offset = offset,
            };
            copy_engine.Enqueue(std::span{&op, 1});
        } else {
            const Core::MemoryManager::SparseCopyRequest copy{
                .source = request.guest_address,
                .destination = destination,
                .size = request.size,
            };
            {
                memory->CopySparseMemoryBatch(
                    std::span<const Core::MemoryManager::SparseCopyRequest>{&copy, 1},
                    request.size);
            }
        }
        break;
    case StreamCopySource::Host:
        std::memcpy(destination, request.host_address, request.size);
        break;
    case StreamCopySource::Zero:
        if (defer_copies && request.size >= DeferredZeroFillThreshold) {
            const GuestCopyEngine::Op op{
                .destination = destination,
                .size = request.size,
                .kind = GuestCopyEngine::OpKind::Zero,
            };
            copy_engine.Enqueue(std::span{&op, 1});
        } else {
            std::memset(destination, 0, request.size);
        }
        break;
    }
    transient_read_buffer.Commit();
    if (reusable) {
        RecordTransientReuse(request.guest_address, request.size, offset);
    }
    if (!defer_copies && request.deduplicate &&
        request.source_type == StreamCopySource::Guest &&
        request.size <= CACHING_PAGESIZE) {
        if (reuse_entry == nullptr) {
            reuse_entry = &reuse.Select(request.guest_address, request.size);
            reuse_entry->mismatch_streak = 0;
            reuse_entry->cooldown = 0;
        }
        const size_t set_index =
            StreamBatchReuseState::SetIndex(request.guest_address, request.size);
        const size_t way_index =
            reuse_entry == &reuse.sets[set_index].ways[0] ? 0 : 1;
        *reuse_entry = {
            .address = request.guest_address,
            .generation = transient_read_buffer.Generation(),
            .tick = scheduler.CurrentTick(),
            .size = static_cast<u32>(request.size),
            .offset = static_cast<u32>(offset),
            .mismatch_streak = reuse_entry->mismatch_streak,
            .cooldown = reuse_entry->cooldown,
            .shadow_valid = captured,
            .valid = true,
        };
        if (captured) {
            std::memcpy(reuse.Shadow(set_index, way_index), reuse.capture.get(),
                        request.size);
        }
    }
    result = {.buffer = &transient_device_buffer, .offset = offset};
    return;
}

void BufferCache::ExecuteStreamCopyBatch(std::span<const StreamCopyRequest> requests,
                                         std::span<StreamCopyResult> results) {
    if (requests.size() != results.size() || requests.size() > MaxStreamCopyRequests) [[unlikely]] {
        ValidateStreamCopyBatch(requests.size(), results.size(), MaxStreamCopyRequests);
    }
    if (requests.empty()) {
        return;
    }
    auto& scratch = *stream_copy_scratch;
    // Deferred copies make the content-based reuse below impossible: it compares the bytes being
    // uploaded now against a shadow. The copy engine moves the bytes instead of skipping them.
    auto& copy_engine = GuestCopyEngine::Instance();
    const bool defer_copies = copy_engine.CanDefer() && transient_read_buffer.is_coherent;
    if (++scratch.hash_generation == 0) {
        for (auto& slot : scratch.hash_table) {
            slot.generation = 0;
        }
        scratch.hash_generation = 1;
    }
    if (defer_copies) {
        BeginTransientReuse();
    }

    if (requests.size() == 1) {
        ExecuteStreamCopySingle(requests.front(), results.front(), defer_copies);
        return;
    }

    auto& reuse = *stream_batch_reuse;
    const auto source_key = [](const StreamCopyRequest& request) noexcept -> u64 {
        switch (request.source_type) {
        case StreamCopySource::Guest:
            return request.guest_address;
        case StreamCopySource::Host:
            return reinterpret_cast<uintptr_t>(request.host_address);
        case StreamCopySource::Zero:
            return 0;
        }
        std::unreachable();
    };
    const auto hash_key = [](const StreamCopyScratch::SourceKey& key) {
        u64 value = key.address ^ (static_cast<u64>(key.size) << 17) ^
                    (static_cast<u64>(key.type) << 61);
        value ^= value >> 29;
        value *= 0x9E3779B185EBCA87ULL;
        value ^= value >> 32;
        return static_cast<size_t>(value) & (StreamCopyScratch::HashTableSize - 1);
    };

    u16 canonical_count = 0;
    const bool use_hash = requests.size() > 4;
    {
        for (u16 request_index = 0; request_index < requests.size(); ++request_index) {
            const auto& request = requests[request_index];
            const StreamCopyScratch::SourceKey key{
                .address = source_key(request),
                .size = request.size,
                .type = request.source_type,
            };
            auto& mapping = scratch.request_map[request_index];
            mapping = {};

            const bool hashable = request.deduplicate && key.type != StreamCopySource::Zero;
            const size_t hash_slot = use_hash && hashable ? hash_key(key) : 0;
            s32 canonical_index = -1;
            if (hashable) {
                if (use_hash) {
                    size_t slot_index = hash_slot;
                    for (size_t probe = 0; probe < StreamCopyScratch::HashTableSize; ++probe) {
                        const auto& slot = scratch.hash_table[slot_index];
                        if (slot.generation != scratch.hash_generation) {
                            break;
                        }
                        if (scratch.canonical_sources[slot.canonical].key == key) {
                            canonical_index = slot.canonical;
                            break;
                        }
                        slot_index =
                            (slot_index + 1) & (StreamCopyScratch::HashTableSize - 1);
                    }
                } else {
                    for (u16 candidate = 0; candidate < canonical_count; ++candidate) {
                        if (scratch.canonical_sources[candidate].key == key) {
                            canonical_index = candidate;
                            break;
                        }
                    }
                }

                if (canonical_index < 0) {
                    for (u16 candidate = 0; candidate < canonical_count; ++candidate) {
                        auto& canonical = scratch.canonical_sources[candidate];
                        if (!canonical.deduplicate ||
                            canonical.key.type != key.type ||
                            canonical.key.type == StreamCopySource::Zero) {
                            continue;
                        }
                        if (key.address < canonical.key.address) {
                            continue;
                        }
                        const u64 source_offset = key.address - canonical.key.address;
                        if (source_offset > canonical.key.size ||
                            key.size > canonical.key.size - source_offset ||
                            !Common::IsAligned(source_offset, request.alignment)) {
                            continue;
                        }
                        canonical.alignment = std::max(canonical.alignment, request.alignment);
                        canonical_index = candidate;
                        mapping.source_offset = static_cast<u32>(source_offset);
                        break;
                    }
                }
            }

            if (canonical_index < 0) {
                canonical_index = canonical_count++;
                scratch.canonical_sources[canonical_index] = {
                    .key = key,
                    .alignment = request.alignment,
                    .deduplicate = request.deduplicate,
                };
                scratch.canonical_placements[canonical_index] = {};
                if (use_hash && hashable) {
                    size_t slot_index = hash_slot;
                    while (scratch.hash_table[slot_index].generation ==
                           scratch.hash_generation) {
                        slot_index =
                            (slot_index + 1) & (StreamCopyScratch::HashTableSize - 1);
                    }
                    scratch.hash_table[slot_index] = {
                        .generation = scratch.hash_generation,
                        .canonical = static_cast<u16>(canonical_index),
                    };
                }
            } else {
                auto& canonical = scratch.canonical_sources[canonical_index];
                canonical.alignment = std::max(canonical.alignment, request.alignment);
            }
            mapping.canonical = static_cast<u16>(canonical_index);
        }
    }

    const u64 reuse_generation = transient_read_buffer.Generation();
    const u64 reuse_tick = scheduler.CurrentTick();
    u16 reuse_candidate_count{};
    u64 reuse_capture_size{};
    u16 provisional_reuse_hits{};
    {
        for (u16 canonical_index = 0; canonical_index < canonical_count; ++canonical_index) {
            const auto& source = scratch.canonical_sources[canonical_index];
            auto& placement = scratch.canonical_placements[canonical_index];
            if (defer_copies && source.deduplicate &&
                source.key.type == StreamCopySource::Guest &&
                source.key.size <= CACHING_PAGESIZE) {
                if (const auto offset = FindTransientReuse(source.key.address, source.key.size,
                                                           source.alignment)) {
                    placement.reused = true;
                    placement.absolute_offset = *offset;
                    ++provisional_reuse_hits;
                }
                continue;
            }
            if (defer_copies || !source.deduplicate ||
                source.key.type != StreamCopySource::Guest ||
                source.key.size > CACHING_PAGESIZE) {
                continue;
            }

            size_t set_index{};
            size_t way_index{};
            auto* entry = reuse.Find(source.key.address, source.key.size, set_index, way_index);
            if (entry == nullptr || entry->generation != reuse_generation ||
                entry->tick != reuse_tick || !Common::IsAligned(entry->offset, source.alignment)) {
                continue;
            }
            placement.reuse_set = static_cast<u16>(set_index);
            placement.reuse_way = static_cast<u8>(way_index);
            if (entry->cooldown != 0) {
                --entry->cooldown;
                continue;
            }

            placement.capture_offset = static_cast<u32>(reuse_capture_size);
            placement.captured = true;
            placement.compare_candidate = entry->shadow_valid;
            reuse.capture_canonicals[reuse_candidate_count] = canonical_index;
            reuse.capture_copies[reuse_candidate_count++] = {
                .source = source.key.address,
                .destination = reuse.capture.get() + reuse_capture_size,
                .size = source.key.size,
            };
            reuse_capture_size += source.key.size;
        }

        {
            memory->CopySparseMemoryBatch(
                std::span<const Core::MemoryManager::SparseCopyRequest>{reuse.capture_copies.data(),
                                                                        reuse_candidate_count},
                reuse_capture_size, false);
        }
        for (u16 candidate = 0; candidate < reuse_candidate_count; ++candidate) {
            const u16 canonical_index = reuse.capture_canonicals[candidate];
            const auto& source = scratch.canonical_sources[canonical_index];
            auto& placement = scratch.canonical_placements[canonical_index];
            auto& entry = reuse.sets[placement.reuse_set].ways[placement.reuse_way];
            const auto size = source.key.size;
            const auto* captured = reuse.capture.get() + placement.capture_offset;
            if (!placement.compare_candidate) {
                entry.mismatch_streak = 0;
                continue;
            }
            if (std::memcmp(captured,
                            reuse.Shadow(placement.reuse_set, placement.reuse_way), size) == 0) {
                placement.reused = true;
                placement.absolute_offset = entry.offset;
                entry.mismatch_streak = 0;
                entry.cooldown = 0;
                ++provisional_reuse_hits;
            } else {
                if (++entry.mismatch_streak >= StreamBatchReuseState::MismatchThreshold) {
                    entry.mismatch_streak = 0;
                    entry.cooldown = StreamBatchReuseState::CooldownLength;
                }
            }
        }
    }

    u64 total_size = 0;
    u64 max_alignment = 1;
    const auto layout_copies = [&] {
        total_size = 0;
        max_alignment = 1;
        for (u16 canonical_index = 0; canonical_index < canonical_count; ++canonical_index) {
            const auto& source = scratch.canonical_sources[canonical_index];
            auto& placement = scratch.canonical_placements[canonical_index];
            if (placement.reused) {
                continue;
            }
            max_alignment = std::max(max_alignment, source.alignment);
            total_size = Common::AlignUp(total_size, source.alignment);
            placement.relative_offset = total_size;
            total_size += source.key.size;
        }
    };
    u8* destination{};
    u64 base_offset{};
    {
        layout_copies();
        if (total_size != 0) {
            std::tie(destination, base_offset) =
                MapTransient(total_size, max_alignment);
        }

        if (provisional_reuse_hits != 0 &&
            (transient_read_buffer.Generation() != reuse_generation ||
             scheduler.CurrentTick() != reuse_tick)) {
            for (u16 canonical_index = 0; canonical_index < canonical_count;
                 ++canonical_index) {
                scratch.canonical_placements[canonical_index].reused = false;
            }
            provisional_reuse_hits = 0;
            layout_copies();
            std::tie(destination, base_offset) =
                MapTransient(total_size, max_alignment);
        }
    }
    if (total_size != 0 && destination == nullptr) [[unlikely]] {
        ValidateStreamCopyDestination(destination);
    }
    for (u16 canonical_index = 0; canonical_index < canonical_count; ++canonical_index) {
        auto& placement = scratch.canonical_placements[canonical_index];
        if (!placement.reused) {
            placement.absolute_offset = base_offset + placement.relative_offset;
        }
    }

    u16 guest_copy_count = 0;
    u64 guest_copy_size = 0;
    u16 deferred_copy_count = 0;
    {
        {
            for (u16 canonical_index = 0; canonical_index < canonical_count; ++canonical_index) {
                const auto& source = scratch.canonical_sources[canonical_index];
                const auto& placement = scratch.canonical_placements[canonical_index];
                if (placement.reused) {
                    continue;
                }
                const auto size = source.key.size;
                u8* const copy_destination = destination + placement.relative_offset;
                switch (source.key.type) {
                case StreamCopySource::Guest:
                    if (placement.captured) {
                        std::memcpy(copy_destination,
                                    reuse.capture.get() + placement.capture_offset, size);
                    } else if (defer_copies) {
                        scratch.deferred_copies[deferred_copy_count++] = GuestCopyEngine::Op{
                            .source = source.key.address,
                            .destination = copy_destination,
                            .size = size,
                            .dst_buffer =
                                GuestCopyEngine::BufferId(transient_device_buffer.Handle()),
                            .dst_offset = base_offset + placement.relative_offset,
                        };
                    } else {
                        scratch.guest_copies[guest_copy_count++] =
                            Core::MemoryManager::SparseCopyRequest{
                                .source = source.key.address,
                                .destination = copy_destination,
                                .size = size,
                            };
                        guest_copy_size += size;
                    }
                    break;
                case StreamCopySource::Host:
                    std::memcpy(
                        copy_destination,
                        reinterpret_cast<const u8*>(static_cast<uintptr_t>(source.key.address)),
                        size);
                    break;
                case StreamCopySource::Zero:
                    if (defer_copies && size >= DeferredZeroFillThreshold) {
                        scratch.deferred_copies[deferred_copy_count++] = GuestCopyEngine::Op{
                            .destination = copy_destination,
                            .size = size,
                            .kind = GuestCopyEngine::OpKind::Zero,
                        };
                    } else {
                        std::memset(copy_destination, 0, size);
                    }
                    break;
                }
            }
        }
        {
            memory->CopySparseMemoryBatch(
                std::span<const Core::MemoryManager::SparseCopyRequest>{scratch.guest_copies.data(),
                                                                        guest_copy_count},
                guest_copy_size);
        }
        if (deferred_copy_count != 0) {
            copy_engine.Enqueue(std::span<const GuestCopyEngine::Op>{
                scratch.deferred_copies.data(), deferred_copy_count});
        }
        if (total_size != 0) {
            transient_read_buffer.Commit();
        }
    }

    const u64 committed_generation = transient_read_buffer.Generation();
    const u64 committed_tick = scheduler.CurrentTick();
    if (defer_copies) {
        for (u16 canonical_index = 0; canonical_index < canonical_count; ++canonical_index) {
            const auto& source = scratch.canonical_sources[canonical_index];
            const auto& placement = scratch.canonical_placements[canonical_index];
            if (!placement.reused && source.deduplicate &&
                source.key.type == StreamCopySource::Guest &&
                source.key.size <= CACHING_PAGESIZE) {
                RecordTransientReuse(source.key.address, source.key.size,
                                     placement.absolute_offset);
            }
        }
    }
    for (u16 canonical_index = 0; canonical_index < canonical_count; ++canonical_index) {
        const auto& source = scratch.canonical_sources[canonical_index];
        const auto& placement = scratch.canonical_placements[canonical_index];
        if (defer_copies || placement.reused || !source.deduplicate ||
            source.key.type != StreamCopySource::Guest || source.key.size > CACHING_PAGESIZE) {
            continue;
        }

        StreamBatchReuseState::Entry* entry{};
        if (placement.reuse_way != StreamBatchReuseState::NoWay) {
            entry = &reuse.sets[placement.reuse_set].ways[placement.reuse_way];
        } else {
            entry = &reuse.Select(source.key.address, source.key.size);
            entry->mismatch_streak = 0;
            entry->cooldown = 0;
        }
        entry->address = source.key.address;
        entry->generation = committed_generation;
        entry->tick = committed_tick;
        entry->size = source.key.size;
        entry->offset = static_cast<u32>(placement.absolute_offset);
        entry->shadow_valid = placement.captured;
        entry->valid = true;
        if (placement.captured) {
            std::memcpy(reuse.Shadow(placement.reuse_set, placement.reuse_way),
                        reuse.capture.get() + placement.capture_offset, source.key.size);
        }
    }

    {
        for (u16 request_index = 0; request_index < requests.size(); ++request_index) {
            const auto& mapping = scratch.request_map[request_index];
            if (mapping.canonical == StreamCopyScratch::NoCanonicalCopy) [[unlikely]] {
                ValidateStreamCopyMapping(mapping.canonical, StreamCopyScratch::NoCanonicalCopy);
            }
            const auto& placement = scratch.canonical_placements[mapping.canonical];
            results[request_index] = {
                .buffer = &transient_device_buffer,
                .offset = placement.absolute_offset + mapping.source_offset,
            };
        }
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    if (!IsRegionRegistered(device_addr, size)) {
        return;
    }
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

void BufferCache::OnCpuWriteFault(VAddr fault_addr) {
    // Pages opened at most per fault, a 256 KiB run.
    static constexpr size_t MaxPagesAhead = 64;
    const auto [opened_addr, opened_size] =
        memory_tracker->OpenWriteRun(fault_addr, MaxPagesAhead);
    if (opened_size == 0) {
        return;
    }
    // The pages count as written now; later writes to them no longer fault.
    InvalidateTransientReuse(opened_addr, opened_size);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    liverpool->SendCommand<true>([this, device_addr, size, is_write] {
        Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];
        // GPU-modified ranges come as many small scattered islands, so the download
        // is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr buf_start = buffer.CpuAddr();
        const VAddr buf_end = buf_start + buffer.SizeBytes();
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), buf_start);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), buf_end);
        DownloadBufferMemory<false>(buffer, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    });
}

template <bool async>
void BufferCache::DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            const VAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](VAddr start, VAddr end) {
                const u64 new_offset = start - buffer_addr;
                const u64 new_size = end - start;
                copies.push_back(vk::BufferCopy{
                    .srcOffset = new_offset,
                    .dstOffset = total_size_bytes,
                    .size = new_size,
                });
                // Align up to avoid cache conflicts
                constexpr u64 align = 64ULL;
                constexpr u64 mask = ~(align - 1ULL);
                total_size_bytes += (new_size + align - 1) & mask;
            };
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, add_download);
            gpu_modified_ranges.Subtract(device_addr_out, range_size);
        });
    if (total_size_bytes == 0) {
        return;
    }
    const auto [download, offset] = download_buffer.Map(total_size_bytes);
    for (auto& copy : copies) {
        // Modify copies to have the staging offset in mind
        copy.dstOffset += offset;
    }
    download_buffer.Commit();
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    // Synchronize prior GPU writes to this buffer before the transfer read
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer.buffer,
        .offset = 0,
        .size = buffer.SizeBytes(),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(buffer.buffer, download_buffer.Handle(), copies);
    const auto write_data = [&]() {
        auto* memory = Core::Memory::Instance();
        for (const auto& copy : copies) {
            const VAddr copy_device_addr = buffer.CpuAddr() + copy.srcOffset;
            const u64 dst_offset = copy.dstOffset - offset;
            memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr), download + dst_offset,
                                    copy.size, Core::MemoryWriteOrigin::GpuCompletion);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
    };
    if constexpr (async) {
        scheduler.DeferOperation(write_data);
    } else {
        scheduler.Finish();
        write_data();
    }
}

SHAD_NO_INLINE void BufferCache::RebuildVertexIndexPlan(u8 plan_index,
                                                        const Vulkan::GraphicsPipeline& pipeline,
                                                        bool dynamic_input) {
    auto& plan = vertex_index_state->plans[plan_index];
    const auto& regs = liverpool->regs;
    const auto vertex_buffers = pipeline.GetVertexBuffers();
    plan.attributes.clear();
    plan.bindings.clear();
    plan.guest_buffers.clear();
    plan.ranges_merged.clear();
    plan.input_keys.clear();
    if (dynamic_input) {
        Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> unused_divisors;
        pipeline.GetVertexInputs(plan.attributes, plan.bindings, unused_divisors,
                                 plan.guest_buffers, regs.vgt_instance_step_rate_0,
                                 regs.vgt_instance_step_rate_1);
        if (const auto& fetch = pipeline.GetFetchShader()) {
            for (const auto& attribute : fetch->attributes) {
                plan.input_keys.push_back(static_cast<u16>(attribute.semantic) |
                                          (static_cast<u16>(attribute.GetStepRate()) << 8));
            }
        }
    } else {
        plan.guest_buffers.assign(vertex_buffers.begin(), vertex_buffers.end());
    }

    Vulkan::VertexInputs<VertexIndexState::BufferRange> ranges;
    for (u32 binding_index = 0; binding_index < plan.guest_buffers.size(); ++binding_index) {
        const auto& buffer = plan.guest_buffers[binding_index];
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(VertexIndexState::BufferRange{
                .base_address = buffer.base_address,
                .end_address = buffer.base_address + buffer.GetSize(),
                .binding_mask = 1U << binding_index,
            });
        }
    }

    if (!ranges.empty()) {
        const auto less_by_address = [](const auto& lhs, const auto& rhs) {
            return lhs.base_address < rhs.base_address;
        };
        if (ranges.size() <= 8) {
            for (u32 range_index = 1; range_index < ranges.size(); ++range_index) {
                auto range = ranges[range_index];
                u32 insert_index = range_index;
                while (insert_index != 0 && less_by_address(range, ranges[insert_index - 1])) {
                    ranges[insert_index] = ranges[insert_index - 1];
                    --insert_index;
                }
                ranges[insert_index] = range;
            }
        } else {
            std::ranges::sort(ranges, less_by_address);
        }
        plan.ranges_merged.emplace_back(ranges.front());
        for (u32 range_index = 1; range_index < ranges.size(); ++range_index) {
            const auto& range = ranges[range_index];
            auto& previous = plan.ranges_merged.back();
            if (previous.end_address < range.base_address) {
                plan.ranges_merged.emplace_back(range);
            } else {
                previous.end_address = std::max(previous.end_address, range.end_address);
                previous.binding_mask |= range.binding_mask;
            }
        }
    }
    plan.identity = pipeline.VertexPlanIdentity();
    plan.step_rate_0 = regs.vgt_instance_step_rate_0;
    plan.step_rate_1 = regs.vgt_instance_step_rate_1;
    plan.valid = true;
}

void BufferCache::PrepareVertexIndexBuffers(const Vulkan::GraphicsPipeline& pipeline,
                                            bool bind_index_buffer, u32 index_offset) {
    auto& state = *vertex_index_state;
    const auto& regs = liverpool->regs;

    state.index = {};
    state.bind_index_buffer = bind_index_buffer;
    state.prepared = true;

    const auto vertex_buffers = pipeline.GetVertexBuffers();
    const bool dynamic_input = instance.IsVertexInputDynamicState();
    const auto matches_plan = [&](const VertexIndexState::Plan& plan) {
        if (!plan.valid || plan.step_rate_0 != regs.vgt_instance_step_rate_0 ||
            plan.step_rate_1 != regs.vgt_instance_step_rate_1 ||
            !std::ranges::equal(vertex_buffers, plan.guest_buffers)) {
            return false;
        }
        if (plan.identity == pipeline.VertexPlanIdentity() || !dynamic_input) {
            return true;
        }
        const auto& fetch = pipeline.GetFetchShader();
        if (!fetch) {
            return plan.input_keys.empty();
        }
        const auto& attributes = fetch->attributes;
        if (attributes.size() != plan.input_keys.size()) {
            return false;
        }
        for (u32 i = 0; i < attributes.size(); ++i) {
            const u16 key = static_cast<u16>(attributes[i].semantic) |
                            (static_cast<u16>(attributes[i].GetStepRate()) << 8);
            if (plan.input_keys[i] != key) {
                return false;
            }
        }
        return true;
    };
    u8 selected_plan = state.active_plan;
    bool hit = matches_plan(state.plans[selected_plan]);
    if (!hit) {
        const u8 other_plan = selected_plan ^ 1;
        if (matches_plan(state.plans[other_plan])) {
            selected_plan = other_plan;
            hit = true;
        } else if (state.plans[selected_plan].valid) {
            selected_plan = other_plan;
        }
    }
    auto& plan = state.plans[selected_plan];
    state.active_plan = selected_plan;
    if (!hit) {
        RebuildVertexIndexPlan(selected_plan, pipeline, dynamic_input);
    }

    for (auto& range : plan.ranges_merged) {
        range.stream_index = VertexIndexState::NoStreamCopy;
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        if (size > std::numeric_limits<u32>::max()) [[unlikely]] {
            ValidateVertexRangeSize(size);
        }
        range.size = static_cast<u32>(size);
        range.was_gpu_modified = HasGpuReadSource(range.base_address, size);
        if (!range.was_gpu_modified && size <= CACHING_PAGESIZE) {
            range.stream_index = QueueStreamCopy(StreamCopyRequest{
                .source_type = StreamCopySource::Guest,
                .guest_address = range.base_address,
                .size = range.size,
                .alignment = instance.UniformMinAlignment(),
            });
        }
    }

    if (!bind_index_buffer) {
        return;
    }

    auto& index = state.index;
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    index.type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    index.address = regs.index_base_address.Address<VAddr>() + index_offset * index_size;
    index.size = regs.num_indices * index_size;
    index.was_gpu_modified = HasGpuReadSource(index.address, index.size);
    if (index.size != 0 && !index.was_gpu_modified && index.size <= CACHING_PAGESIZE) {
        index.stream_index = QueueStreamCopy(StreamCopyRequest{
            .source_type = StreamCopySource::Guest,
            .guest_address = index.address,
            .size = index.size,
            .alignment = std::max<u64>(index_size, instance.UniformMinAlignment()),
        });
    }
}

void BufferCache::FinalizeVertexIndexBuffers(
    boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers) {
    auto& state = *vertex_index_state;
    auto& plan = state.plans[state.active_plan];
    if (!state.prepared || !stream_copy_finalized) [[unlikely]] {
        ValidateVertexIndexState(state.prepared, stream_copy_finalized);
    }

    const auto resolve_token = [&](VertexIndexState::BufferToken& token, VAddr address, u32 size) {
        if (token.topology_epoch != TopologyEpoch() ||
            pending_image_readback_ranges.Contains(address, size) ||
            !IsBufferCacheEntryValid(token.id, token.uid, address, size)) {
            token.id = FindBuffer(address, size);
            token.uid = GetBufferUid(token.id);
            token.topology_epoch = TopologyEpoch();
        }
        return token.id;
    };

    for (auto& range : plan.ranges_merged) {
        if (range.stream_index != VertexIndexState::NoStreamCopy) {
            const auto& result = GetStreamCopyResult(range.stream_index);
            range.buffer = result.buffer;
            range.vk_buffer = result.buffer->Handle();
            range.offset = result.offset;
        } else {
            const BufferId buffer_id =
                resolve_token(range.token, range.base_address, range.size);
            range.buffer = &slot_buffers[buffer_id];
            SynchronizeBuffer(*range.buffer, range.base_address, range.size, false, false);
            range.vk_buffer = range.buffer->Handle();
            range.offset = range.buffer->Offset(range.base_address);
        }
        if (range.buffer == nullptr) [[unlikely]] {
            ValidateVertexIndexBuffer(range.buffer);
        }
        if (range.was_gpu_modified) {
            if (auto barrier =
                    range.buffer->GetBarrier(vk::AccessFlagBits2::eVertexAttributeRead,
                                             vk::PipelineStageFlagBits2::eVertexAttributeInput)) {
                barriers.emplace_back(*barrier);
            }
        }
    }

    auto& index = state.index;
    if (state.bind_index_buffer) {
        if (index.size == 0) {
            const auto [buffer, offset] = ObtainBuffer(index.address, 0, false);
            index.buffer = buffer;
            index.offset = offset;
        } else if (index.stream_index != VertexIndexState::NoStreamCopy) {
            const auto& result = GetStreamCopyResult(index.stream_index);
            index.buffer = result.buffer;
            index.offset = result.offset;
        } else {
            const BufferId buffer_id =
                resolve_token(state.index_token, index.address, index.size);
            index.buffer = &slot_buffers[buffer_id];
            SynchronizeBuffer(*index.buffer, index.address, index.size, false, false);
            index.offset = index.buffer->Offset(index.address);
        }
        if (index.buffer == nullptr) [[unlikely]] {
            ValidateVertexIndexBuffer(index.buffer);
        }
        if (index.was_gpu_modified) {
            if (auto barrier = index.buffer->GetBarrier(vk::AccessFlagBits2::eIndexRead,
                                                        vk::PipelineStageFlagBits2::eIndexInput)) {
                barriers.emplace_back(*barrier);
            }
        }
    }

    const u64 current_tick = scheduler.CurrentTick();
    if (state.emitted_tick != current_tick) {
        state.emitted_tick = current_tick;
        state.vertex_input_valid = false;
        state.vertex_buffers_valid = false;
        state.index_buffer_valid = false;
    }

    const auto attributes_equal = [](const auto& lhs, const auto& rhs, u32 rhs_count) {
        if (lhs.size() != rhs_count) {
            return false;
        }
        for (u32 i = 0; i < rhs_count; ++i) {
            const auto& left = lhs[i];
            const auto& right = rhs[i];
            const u32 different =
                (left.location ^ right.location) | (left.binding ^ right.binding) |
                (static_cast<u32>(left.format) ^ static_cast<u32>(right.format)) |
                (left.offset ^ right.offset);
            if (different != 0) {
                return false;
            }
        }
        return true;
    };
    const auto bindings_equal = [](const auto& lhs, const auto& rhs, u32 rhs_count) {
        if (lhs.size() != rhs_count) {
            return false;
        }
        for (u32 i = 0; i < rhs_count; ++i) {
            const auto& left = lhs[i];
            const auto& right = rhs[i];
            const u32 different =
                (left.binding ^ right.binding) | (left.stride ^ right.stride) |
                (static_cast<u32>(left.inputRate) ^ static_cast<u32>(right.inputRate)) |
                (left.divisor ^ right.divisor);
            if (different != 0) {
                return false;
            }
        }
        return true;
    };

    const auto cmdbuf = scheduler.CommandBuffer();
    if (instance.IsVertexInputDynamicState() &&
        (!state.vertex_input_valid ||
         !attributes_equal(plan.attributes, state.emitted_attributes,
                           state.emitted_attribute_count) ||
         !bindings_equal(plan.bindings, state.emitted_bindings, state.emitted_binding_count))) {
        cmdbuf.setVertexInputEXT(plan.bindings, plan.attributes);
        state.emitted_attribute_count = static_cast<u32>(plan.attributes.size());
        state.emitted_binding_count = static_cast<u32>(plan.bindings.size());
        std::ranges::copy(plan.attributes, state.emitted_attributes.begin());
        std::ranges::copy(plan.bindings, state.emitted_bindings.begin());
        state.vertex_input_valid = true;
    }

    if (!plan.guest_buffers.empty()) {
        const u32 num_buffers = static_cast<u32>(plan.guest_buffers.size());
        auto& host_buffers = state.host_buffers;
        auto& host_offsets = state.host_offsets;
        auto& host_sizes = state.host_sizes;
        auto& host_strides = state.host_strides;
        for (u32 i = 0; i < num_buffers; ++i) {
            const auto& buffer = plan.guest_buffers[i];
            host_buffers[i] = VK_NULL_HANDLE;
            host_offsets[i] = 0;
            host_sizes[i] = buffer.GetSize();
            host_strides[i] = buffer.GetStride();
        }

        for (const auto& range : plan.ranges_merged) {
            u32 binding_mask = range.binding_mask;
            while (binding_mask != 0) {
                const u32 binding_index = std::countr_zero(binding_mask);
                binding_mask &= binding_mask - 1;
                const auto& buffer = plan.guest_buffers[binding_index];
                host_buffers[binding_index] = range.vk_buffer;
                host_offsets[binding_index] =
                    range.offset + buffer.base_address - range.base_address;
            }
        }

        const bool dynamic_vertex_input = instance.IsVertexInputDynamicState();
        const auto vertex_buffers_equal = [&] {
            u32 i = 0;
            for (; i + 4 <= num_buffers; i += 4) {
                const u64 different =
                    (std::bit_cast<u64>(host_buffers[i]) ^
                     std::bit_cast<u64>(state.emitted_buffers[i])) |
                    (std::bit_cast<u64>(host_buffers[i + 1]) ^
                     std::bit_cast<u64>(state.emitted_buffers[i + 1])) |
                    (std::bit_cast<u64>(host_buffers[i + 2]) ^
                     std::bit_cast<u64>(state.emitted_buffers[i + 2])) |
                    (std::bit_cast<u64>(host_buffers[i + 3]) ^
                     std::bit_cast<u64>(state.emitted_buffers[i + 3])) |
                    (host_offsets[i] ^ state.emitted_offsets[i]) |
                    (host_offsets[i + 1] ^ state.emitted_offsets[i + 1]) |
                    (host_offsets[i + 2] ^ state.emitted_offsets[i + 2]) |
                    (host_offsets[i + 3] ^ state.emitted_offsets[i + 3]);
                if (different != 0) {
                    return false;
                }
            }
            for (; i < num_buffers; ++i) {
                const u64 different =
                    (std::bit_cast<u64>(host_buffers[i]) ^
                     std::bit_cast<u64>(state.emitted_buffers[i])) |
                    (host_offsets[i] ^ state.emitted_offsets[i]);
                if (different != 0) {
                    return false;
                }
            }
            if (!dynamic_vertex_input) {
                for (u32 i = 0; i < num_buffers; ++i) {
                    if (((host_sizes[i] ^ state.emitted_sizes[i]) |
                         (host_strides[i] ^ state.emitted_strides[i])) != 0) {
                        return false;
                    }
                }
            }
            return true;
        };
        const bool same_buffers = state.vertex_buffers_valid &&
                                  state.emitted_buffer_count == num_buffers &&
                                  vertex_buffers_equal();
        if (!same_buffers) {
            if (dynamic_vertex_input) {
                cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
            } else {
                cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                          host_sizes.data(), host_strides.data());
            }
            state.emitted_buffer_count = num_buffers;
            std::copy_n(host_buffers.begin(), num_buffers, state.emitted_buffers.begin());
            std::copy_n(host_offsets.begin(), num_buffers, state.emitted_offsets.begin());
            std::copy_n(host_sizes.begin(), num_buffers, state.emitted_sizes.begin());
            std::copy_n(host_strides.begin(), num_buffers, state.emitted_strides.begin());
            state.vertex_buffers_valid = true;
        }
    }

    if (state.bind_index_buffer) {
        const vk::Buffer handle = index.buffer->Handle();
        if (!state.index_buffer_valid || state.emitted_index_buffer != handle ||
            state.emitted_index_offset != index.offset || state.emitted_index_type != index.type) {
            cmdbuf.bindIndexBuffer(handle, index.offset, index.type);
            state.emitted_index_buffer = handle;
            state.emitted_index_offset = index.offset;
            state.emitted_index_type = index.type;
            state.index_buffer_valid = true;
        }
    }

    state.prepared = false;
}

void BufferCache::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0, "GDS offset must be dword aligned");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!HasGpuReadSource(address, num_bytes)) {
            GuestCopyEngine::Instance().WaitForGuestWrite(address, num_bytes);
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + num_bytes / sizeof(u32), value);
            return;
        }
    }
    Buffer* buffer = [&] {
        if (is_gds) {
            return &gds_buffer;
        }
        const auto [buffer, offset] = ObtainBuffer(address, num_bytes, true);
        return buffer;
    }();
    buffer->Fill(buffer->Offset(address), num_bytes, value);
}

void BufferCache::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds && !HasGpuReadSource(dst, num_bytes)) {
        if (!src_gds && !HasGpuReadSource(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            GuestCopyEngine::Instance().WaitForGuestWrite(dst, num_bytes);
            memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
        // Without a readback there's nothing we can do with this
        // Fallback to creating dst buffer on GPU to at least have this data there
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    auto& src_buffer = [&] -> const Buffer& {
        if (src_gds) {
            return gds_buffer;
        }
        const auto buffer_id = FindBuffer(src, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, src, num_bytes, false, true);
        return buffer;
    }();
    auto& dst_buffer = [&] -> const Buffer& {
        if (dst_gds) {
            return gds_buffer;
        }
        const auto buffer_id = FindBuffer(dst, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, dst, num_bytes, true, true);
        gpu_modified_ranges.Add(dst, num_bytes);
        return buffer;
    }();
    const vk::BufferCopy region = {
        .srcOffset = src_buffer.Offset(src),
        .dstOffset = dst_buffer.Offset(dst),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 buf_barriers_before[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_before,
    });
    cmdbuf.copyBuffer(src_buffer.Handle(), dst_buffer.Handle(), region);
    const vk::BufferMemoryBarrier2 buf_barriers_after[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_after,
    });
}

std::pair<Buffer*, u32> BufferCache::ObtainBuffer(VAddr device_addr, u32 size, bool is_written,
                                                  bool is_texel_buffer, BufferId buffer_id) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= CACHING_PAGESIZE && !HasGpuReadSource(device_addr, size)) {
        u64 offset = 0;
        {
            const bool resolved =
                VideoCore::GpuAuthorityTracker::Instance().ResolveForRamRead(device_addr, size);
            if (resolved) {
                offset = stream_buffer.Copy(device_addr, size, instance.UniformMinAlignment());
            }
        }
        return {&stream_buffer, static_cast<u32>(offset)};
    }
    if (IsBufferInvalid(buffer_id) || pending_image_readback_ranges.Contains(device_addr, size)) {
        buffer_id = FindBuffer(device_addr, size);
    }
    Buffer& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        if (buffer.has_image_alias && image_alias_ranges.Intersects(device_addr, size)) {
            texture_cache.InvalidateMemoryFromGPU(device_addr, size);
        }
    }
    return {&buffer, buffer.Offset(device_addr)};
}

std::pair<Buffer*, u32> BufferCache::ObtainBufferForRanges(VAddr device_addr, u32 size,
                                                           std::span<const SyncRange> ranges,
                                                           bool is_written) {
    Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];
    if (!is_written) {
        SynchronizeBufferRanges(buffer, ranges);
        return {&buffer, buffer.Offset(device_addr)};
    }
    for (const auto& [range_addr, range_size] : ranges) {
        SynchronizeBuffer(buffer, range_addr, range_size, true, false);
        gpu_modified_ranges.Add(range_addr, range_size);
        if (buffer.has_image_alias && image_alias_ranges.Intersects(range_addr, range_size)) {
            texture_cache.InvalidateMemoryFromGPU(range_addr, range_size);
        }
    }
    return {&buffer, buffer.Offset(device_addr)};
}

std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(VAddr gpu_addr, u32 size) {
    if (const auto shadow =
            GpuAuthorityTracker::Instance().AcquireGpuShadowForImage(gpu_addr, size)) {
        ASSERT(shadow->buffer_offset <= std::numeric_limits<u32>::max());
        return {&download_buffer, static_cast<u32>(shadow->buffer_offset)};
    }
    // Check if any buffer contains the full requested range.
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            SynchronizeBuffer(buffer, gpu_addr, size, false, false);
            return {&buffer, buffer.Offset(gpu_addr)};
        }
    }
    // If some buffer within was GPU modified create a full buffer to avoid losing GPU data.
    if (HasGpuReadSource(gpu_addr, size)) {
        return ObtainBuffer(gpu_addr, size, false, false);
    }
    // In all other cases, just do a CPU copy to the staging buffer.
    const auto [data, offset] = staging_buffer.Map(size, instance.StorageMinAlignment());
    {
        auto& copy_engine = GuestCopyEngine::Instance();
        const bool defer_copy = copy_engine.CanDefer() && staging_buffer.is_coherent;
        // A deferred copy serves bytes the GPU still owns from their shadow on the GPU, which
        // spares the wait for the producer that materializing guest RAM needs.
        const bool resolved = VideoCore::GpuAuthorityTracker::Instance().ResolveForRamRead(
            gpu_addr, size, defer_copy && copy_engine.HasProtectedCopyResolver());
        if (resolved) {
            if (defer_copy) {
                const GuestCopyEngine::Op op{
                    .source = gpu_addr,
                    .destination = data,
                    .size = size,
                    .dst_buffer = GuestCopyEngine::BufferId(staging_buffer.Handle()),
                    .dst_offset = offset,
                };
                copy_engine.Enqueue(std::span{&op, 1});
            } else {
                memory->CopySparseMemory(gpu_addr, data, size);
            }
        }
    }
    staging_buffer.Commit();
    return {&staging_buffer, offset};
}

bool BufferCache::ServeGuestCopyFromGpuShadows(
    const GuestCopyEngine::Op& op,
    std::span<GuestCopyEngine::Op, GuestCopyEngine::MaxResolverRemainder> remainder,
    u32& remainder_count, u64& gpu_bytes, const PageManager& page_manager) {
    if (op.kind != GuestCopyEngine::OpKind::Guest) {
        return false;
    }
    auto& authority_tracker = GpuAuthorityTracker::Instance();
    GpuShadowPieces pieces;
    if (!authority_tracker.CollectGpuShadowPieces(op.source, op.size, pieces)) {
        return false;
    }

    // Every other byte of the range is current in guest RAM. Authorities start and end in the
    // middle of pages, so those bytes can still sit on a page that denies reads for a neighbour;
    // reading them there would fault and materialize the neighbour. Such runs are read through
    // the backing view instead.
    remainder_count = 0;
    const auto add_remainder = [&](VAddr begin, VAddr end) {
        if (begin >= end) {
            return true;
        }
        if (remainder_count == remainder.size()) {
            return false;
        }
        auto kind = GuestCopyEngine::OpKind::Guest;
        if (page_manager.HasReadWatchers(begin, end - begin)) {
            if (!memory->IsBackedRange(begin, end - begin)) {
                return false;
            }
            kind = GuestCopyEngine::OpKind::Backing;
        }
        const u64 delta = begin - op.source;
        remainder[remainder_count++] = GuestCopyEngine::Op{
            .source = begin,
            .destination = op.destination + delta,
            .size = end - begin,
            .kind = kind,
            .dst_buffer = op.dst_buffer,
            .dst_offset = op.dst_offset + delta,
        };
        return true;
    };
    VAddr cursor = op.source;
    for (const auto& piece : pieces) {
        if (!add_remainder(cursor, piece.addr)) {
            return false;
        }
        cursor = piece.addr + piece.size;
    }
    if (!add_remainder(cursor, op.source + op.size)) {
        return false;
    }
    gpu_bytes = 0;
    if (pieces.empty()) {
        return true;
    }

    authority_tracker.CommitGpuShadowPieces(pieces, scheduler.CurrentTick());
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    for (const auto& piece : pieces) {
        copies.push_back(vk::BufferCopy{
            .srcOffset = piece.buffer_offset,
            .dstOffset = op.dst_offset + (piece.addr - op.source),
            .size = piece.size,
        });
        gpu_bytes += piece.size;
    }

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    // Orders the copy after the transfer that wrote the shadow and after earlier readers of the
    // destination, and the consumers recorded next after the copy.
    const vk::MemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    const vk::MemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(download_buffer.Handle(),
                      GuestCopyEngine::ToHandle<vk::Buffer>(op.dst_buffer), copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &post_barrier,
    });
    return true;
}

bool BufferCache::IsRegionRegistered(VAddr addr, size_t size) {
    // Check if we are missing some edge case here
    return buffer_ranges.Intersects(addr, size);
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

bool BufferCache::HasGpuReadSource(VAddr addr, size_t size) {
    if (pending_image_readback_ranges.Contains(addr, size) &&
        texture_cache.FindImageContainingRange(addr, size)) {
        return true;
    }
    return IsRegionGpuModified(addr, size);
}

bool BufferCache::TrackImageReadback(Image& image, u32 copy_size) {
    const VAddr device_addr = image.info.guest_address;
    if (image.info.props.is_tiled || image.info.resources.levels != 1 || device_addr == 0 ||
        copy_size == 0 || copy_size > image.info.guest_size || (device_addr & 3) != 0) {
        return false;
    }

    // Any persistent buffer for this range now contains an older image epoch. Keep the image as
    // the authoritative source until a real buffer consumer synchronizes it through the normal
    // cache path. This avoids creating/merging/deleting buffers while EOS is being recorded.
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, copy_size);
    gpu_modified_ranges.Subtract(device_addr, copy_size);
    pending_image_readback_ranges.Add(device_addr, copy_size);
    image_alias_ranges.Add(device_addr, copy_size);
    return true;
}

void BufferCache::CompleteImageReadback(VAddr addr, u32 size) {
    pending_image_readback_ranges.Subtract(addr, size);
    gpu_modified_ranges.Subtract(addr, size);
    memory_tracker->UnmarkRegionAsGpuModified(addr, size);
    memory_tracker->MarkRegionAsCpuModified(addr, size);
}

bool BufferCache::IsBufferCacheEntryValid(BufferId id, u64 uid, VAddr address, u64 size) const {
    if (!id || !slot_buffers.is_allocated(id)) {
        return false;
    }
    const auto& buffer = slot_buffers[id];
    return !buffer.is_deleted && buffer.Uid() == uid && buffer.IsInBounds(address, size);
}

u64 BufferCache::GetBufferUid(BufferId id) const {
    ASSERT(id && slot_buffers.is_allocated(id));
    return slot_buffers[id].Uid();
}

BufferId BufferCache::FindBuffer(VAddr device_addr, u32 size) {
    ASSERT(device_addr != 0);
    if (pending_image_readback_ranges.Contains(device_addr, size)) {
        const ImageId image_id = texture_cache.FindImageContainingRange(device_addr, size);
        if (image_id) {
            const Image& image = texture_cache.GetImage(image_id);
            device_addr = image.info.guest_address;
            size = image.info.guest_size;
        }
    }
    const u64 page = device_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page].buffer_id;
    if (!buffer_id) {
        return CreateBuffer(device_addr, size);
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(device_addr, size)) {
        return buffer_id;
    }
    return CreateBuffer(device_addr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(VAddr device_addr, u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    VAddr begin = device_addr;
    VAddr end = device_addr + wanted_size;
    int stream_score = 0;
    bool has_stream_leap = false;
    const auto expand_begin = [&](VAddr add_value) {
        static constexpr VAddr min_page = CACHING_PAGESIZE + DEVICE_PAGESIZE;
        if (add_value > begin - min_page) {
            begin = min_page;
            device_addr = DEVICE_PAGESIZE;
            return;
        }
        begin -= add_value;
        device_addr = begin - CACHING_PAGESIZE;
    };
    const auto expand_end = [&](VAddr add_value) {
        static constexpr VAddr max_page = 1ULL << MemoryTracker::MAX_CPU_PAGE_BITS;
        if (add_value > max_page - end) {
            end = max_page;
            return;
        }
        end += add_value;
    };
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; device_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         device_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        if (!overlap_id) {
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.is_picked) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.is_picked = true;
        const VAddr overlap_device_addr = overlap.CpuAddr();
        const bool expands_left = overlap_device_addr < begin;
        if (expands_left) {
            begin = overlap_device_addr;
        }
        const VAddr overlap_end = overlap_device_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            if (expands_right) {
                expand_end(CACHING_PAGESIZE * 128);
            }
            if (expands_left) {
                expand_begin(CACHING_PAGESIZE * 128);
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

void BufferCache::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                              bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    new_buffer.has_image_alias |= overlap.has_image_alias;
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = dst_base_offset,
        .size = overlap.SizeBytes(),
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> pre_barriers{};
    if (auto src_barrier = overlap.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                              vk::PipelineStageFlagBits2::eTransfer)) {
        pre_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier =
            new_buffer.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eTransfer, dst_base_offset)) {
        pre_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });

    cmdbuf.copyBuffer(overlap.Handle(), new_buffer.Handle(), copy);

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> post_barriers{};
    if (auto src_barrier =
            overlap.GetBarrier(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                               vk::PipelineStageFlagBits2::eAllCommands)) {
        post_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier = new_buffer.GetBarrier(
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eAllCommands, dst_base_offset)) {
        post_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(post_barriers.size()),
        .pBufferMemoryBarriers = post_barriers.data(),
    });
    DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(VAddr device_addr, u32 wanted_size) {
    const VAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const BufferId new_buffer_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, overlap.begin,
                            AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, size);
    auto& new_buffer = slot_buffers[new_buffer_id];
    new_buffer.has_image_alias = image_alias_ranges.Intersects(overlap.begin, size);
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    Register(new_buffer_id);
    return new_buffer_id;
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager.ProcessFaultBuffer();
}

void BufferCache::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
    ++topology_epoch;
}

void BufferCache::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
    ++topology_epoch;
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    const VAddr device_addr_begin = buffer.CpuAddr();
    const VAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    const u64 size_pages = page_end - page_begin;
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page].buffer_id = buffer_id;
        } else {
            page_table[page].buffer_id = BufferId{};
        }
    }
    if constexpr (insert) {
        total_used_memory += Common::AlignUp(size, CACHING_PAGESIZE);
        buffer.SetLRUId(lru_cache.Insert(buffer_id, gc_tick));
        boost::container::small_vector<vk::DeviceAddress, 128> bda_addrs;
        bda_addrs.reserve(size_pages);
        for (u64 i = 0; i < size_pages; ++i) {
            vk::DeviceAddress addr = buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS);
            bda_addrs.push_back(addr);
        }
        WriteDataBuffer(bda_pagetable_buffer, page_begin * sizeof(vk::DeviceAddress),
                        bda_addrs.data(), bda_addrs.size() * sizeof(vk::DeviceAddress));
        buffer_ranges.Add(buffer.CpuAddr(), buffer.SizeBytes(), buffer_id);
    } else {
        total_used_memory -= Common::AlignUp(size, CACHING_PAGESIZE);
        lru_cache.Free(buffer.LRUId());
        const u64 offset = bda_pagetable_buffer.Offset(page_begin * sizeof(vk::DeviceAddress));
        bda_pagetable_buffer.Fill(offset, size_pages * sizeof(vk::DeviceAddress), 0);
        buffer_ranges.Subtract(buffer.CpuAddr(), buffer.SizeBytes());
    }
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                                    bool is_texel_buffer) {
    // The GPU writes land after any image sync recorded by this call.
    SCOPE_EXIT {
        buffer.content_generation += is_written;
    };
    if (pending_image_readback_ranges.Contains(device_addr, size) &&
        SynchronizeBufferFromImage(buffer, device_addr, size)) {
        return true;
    }

    VideoCore::GpuAuthorityTracker::Instance().ResolveForRamRead(device_addr, size);

    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes = 0;
    VAddr buffer_start = buffer.CpuAddr();
    vk::Buffer src_buffer = VK_NULL_HANDLE;
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 device_addr_out, u64 range_size) {
            copies.emplace_back(total_size_bytes, device_addr_out - buffer_start, range_size);
            total_size_bytes += range_size;
        },
        [&] { src_buffer = UploadCopies(buffer, copies, total_size_bytes); });

    if (src_buffer) {
        RecordBufferUpload(buffer, src_buffer, copies, device_addr, total_size_bytes);
    }
    return is_texel_buffer && !is_written && SynchronizeBufferFromImage(buffer, device_addr, size);
}

void BufferCache::SynchronizeBufferRanges(Buffer& buffer, std::span<const SyncRange> ranges) {
    boost::container::small_vector<vk::BufferCopy, 16> copies;
    size_t total_size_bytes = 0;
    const VAddr buffer_start = buffer.CpuAddr();
    for (const auto& [device_addr, size] : ranges) {
        if (pending_image_readback_ranges.Contains(device_addr, size) &&
            SynchronizeBufferFromImage(buffer, device_addr, size)) {
            continue;
        }
        VideoCore::GpuAuthorityTracker::Instance().ResolveForRamRead(device_addr, size);
        memory_tracker->ForEachUploadRange(
            device_addr, size, false,
            [&](u64 device_addr_out, u64 range_size) {
                copies.emplace_back(total_size_bytes, device_addr_out - buffer_start, range_size);
                total_size_bytes += range_size;
            },
            [] {});
    }
    if (copies.empty()) {
        return;
    }
    const vk::Buffer src_buffer = UploadCopies(buffer, copies, total_size_bytes);
    RecordBufferUpload(buffer, src_buffer, copies, ranges.front().device_addr, total_size_bytes);
}

void BufferCache::RecordBufferUpload(Buffer& buffer, vk::Buffer src_buffer,
                                     std::span<const vk::BufferCopy> copies, VAddr device_addr,
                                     size_t total_size_bytes) {
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    // Only the uploaded span of the buffer takes part in the dependency.
    u64 span_begin = std::numeric_limits<u64>::max();
    u64 span_end = 0;
    for (const auto& copy : copies) {
        span_begin = std::min<u64>(span_begin, copy.dstOffset);
        span_end = std::max<u64>(span_end, copy.dstOffset + copy.size);
    }
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer.Handle(),
        .offset = span_begin,
        .size = span_end - span_begin,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer.Handle(),
        .offset = span_begin,
        .size = span_end - span_begin,
    };
    const bool batched = upload_barrier_batch.has_value();
    if (!batched || std::exchange(upload_barrier_batch->needs_pre_barrier, false)) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &pre_barrier,
        });
    }
    cmdbuf.copyBuffer(src_buffer, buffer.buffer, copies);
    if (batched) {
        upload_barrier_batch->recorded = true;
    } else {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &post_barrier,
        });
    }
    TouchBuffer(buffer);
    ++buffer.content_generation;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     size_t total_size_bytes) {
    if (copies.empty()) {
        return VK_NULL_HANDLE;
    }
    const auto [staging, offset] = staging_buffer.Map(total_size_bytes);
    auto& copy_engine = GuestCopyEngine::Instance();
    boost::container::small_vector<GuestCopyEngine::Op, 4> deferred_copies;
    if (staging) {
        const bool defer_copies = copy_engine.CanDefer() && staging_buffer.is_coherent;
        for (auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            {
                const bool resolved = VideoCore::GpuAuthorityTracker::Instance().ResolveForRamRead(
                    device_addr, copy.size);
                if (resolved) {
                    if (defer_copies) {
                        deferred_copies.push_back(GuestCopyEngine::Op{
                            .source = device_addr,
                            .destination = src_pointer,
                            .size = copy.size,
                            .dst_buffer = GuestCopyEngine::BufferId(staging_buffer.Handle()),
                            .dst_offset = offset + copy.srcOffset,
                        });
                    } else {
                        memory->CopySparseMemory(device_addr, src_pointer, copy.size);
                    }
                }
            }
            // Apply the staging offset
            copy.srcOffset += offset;
        }
        if (!deferred_copies.empty()) {
            copy_engine.Enqueue(std::span<const GuestCopyEngine::Op>{deferred_copies.data(),
                                                                    deferred_copies.size()});
        }
        staging_buffer.Commit();
        return staging_buffer.Handle();
    } else {
        // For large one time transfers use a temporary host buffer.
        auto temp_buffer =
            std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Upload, 0,
                                     vk::BufferUsageFlagBits::eTransferSrc, total_size_bytes);
        const vk::Buffer src_buffer = temp_buffer->Handle();
        u8* const staging = temp_buffer->mapped_data.data();
        const bool defer_copies = copy_engine.CanDefer() && temp_buffer->is_coherent;
        for (const auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            {
                const bool resolved = VideoCore::GpuAuthorityTracker::Instance().ResolveForRamRead(
                    device_addr, copy.size);
                if (resolved) {
                    if (defer_copies) {
                        deferred_copies.push_back(GuestCopyEngine::Op{
                            .source = device_addr,
                            .destination = src_pointer,
                            .size = copy.size,
                        });
                    } else {
                        memory->CopySparseMemory(device_addr, src_pointer, copy.size);
                    }
                }
            }
        }
        if (!deferred_copies.empty()) {
            // The buffer is released only after the GPU completes the current tick, which in
            // turn waits for these copies before the command buffer is submitted.
            copy_engine.Enqueue(std::span<const GuestCopyEngine::Op>{deferred_copies.data(),
                                                                    deferred_copies.size()});
        }
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable { buffer.reset(); });
        return src_buffer;
    }
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        ASSERT(*type == TextureCache::MetaType::HTile);
        static constexpr u32 ZmaskUncompressed = 0xf;
        buffer.Fill(buffer.Offset(device_addr), size, ZmaskUncompressed);
        return true;
    }
    const bool pending_readback = pending_image_readback_ranges.Contains(device_addr, size);
    const auto authority = GpuAuthorityTracker::Instance().GetAuthorityForRange(device_addr, size);
    ImageId image_id{};
    if (authority) {
        if (!buffer.IsInBounds(authority->guest_begin, authority->download_size) ||
            !texture_cache.IsGpuAuthorityImageCurrent(
                static_cast<ImageId>(authority->image_id), authority->image_uid,
                authority->resource_version, device_addr, size)) {
            return false;
        }
        image_id = static_cast<ImageId>(authority->image_id);
    } else {
        image_id = pending_readback ? texture_cache.FindImageContainingRange(device_addr, size)
                                    : texture_cache.FindImageFromRange(device_addr, size);
    }
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(buffer.IsInBounds(image.info.guest_address, image.info.guest_size),
               "Buffer does not contain aliased image {:x}:{:x}", image.info.guest_address,
               image.info.guest_size);
    // The GPU-modified mark set by a sync outlives it without readbacks, so it cannot tell whether
    // the buffer still holds the image. The copy is skipped only while neither side has changed.
    const ImageSyncState sync_state{
        .buffer_uid = buffer.uid,
        .buffer_generation = buffer.content_generation,
        .image_uid = image.image_uid,
        .image_epoch = image.content_epoch,
    };
    // A tiled image syncs only the bytes asked for: the tiler leaves the rest alone.
    const VAddr image_addr = image.info.guest_address;
    const bool sync_range = image.info.props.is_tiled && device_addr + size > image_addr;
    u32 range_begin = 0;
    u32 range_end = image.info.guest_size;
    if (sync_range) {
        const u64 bytes_per_texel = std::max(image.info.num_bits / 8, 1U);
        const u64 begin = device_addr > image_addr ? device_addr - image_addr : 0;
        const u64 end = std::min<u64>(device_addr + size - image_addr, image.info.guest_size);
        if (begin < end) {
            range_begin = static_cast<u32>(begin - begin % bytes_per_texel);
            range_end = static_cast<u32>(std::min<u64>(
                end + (bytes_per_texel - end % bytes_per_texel) % bytes_per_texel,
                image.info.guest_size));
        }
    }
    if (const auto it = image_sync_states.find(image_addr);
        it != image_sync_states.end() && it->second.state == sync_state &&
        it->second.begin <= range_begin && range_end <= it->second.end) {
        return true;
    }
    const u32 buf_offset = buffer.Offset(image.info.guest_address);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u32 copy_size = 0;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buf_offset + mip_info.offset + mip_info.size > buffer.SizeBytes()) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
        copy_size += mip_info.size;
    }
    if (copy_size == 0) {
        return false;
    }
    range_end = std::min(range_end, copy_size);
    if (range_begin >= range_end) {
        range_begin = 0;
        range_end = copy_size;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    scheduler.EndRendering();
    const auto dst_access = image.info.props.is_tiled ? vk::AccessFlagBits2::eShaderWrite
                                                       : vk::AccessFlagBits2::eTransferWrite;
    const auto dst_stage = image.info.props.is_tiled ? vk::PipelineStageFlagBits2::eComputeShader
                                                      : vk::PipelineStageFlagBits2::eCopy;
    if (const auto barrier = buffer.GetBarrier(dst_access, dst_stage, buf_offset)) {
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &*barrier,
        });
    }
    if (sync_range) {
        tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size,
                               range_begin, range_end);
    } else {
        tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size);
    }
    const VAddr synced_addr = image_addr + range_begin;
    const u32 synced_size = range_end - range_begin;
    memory_tracker->MarkRegionAsGpuModified(synced_addr, synced_size);
    gpu_modified_ranges.Add(synced_addr, synced_size);
    pending_image_readback_ranges.Subtract(synced_addr, synced_size);
    image_alias_ranges.Add(image_addr, copy_size);
    buffer.has_image_alias = true;
    // Bytes synced before at the same versions stay synced when the ranges touch.
    auto& record = image_sync_states[image_addr];
    if (record.state == sync_state && record.begin <= range_end && range_begin <= record.end) {
        record.begin = std::min(record.begin, range_begin);
        record.end = std::max(record.end, range_end);
    } else {
        record = ImageSyncRecord{.state = sync_state, .begin = range_begin, .end = range_end};
    }
    return true;
}

void BufferCache::SynchronizeBuffersInRange(VAddr device_addr, u64 size) {
    const VAddr device_addr_end = device_addr + size;
    ForEachBufferInRange(device_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
        RENDERER_TRACE;
        VAddr start = std::max(buffer.CpuAddr(), device_addr);
        VAddr end = std::min(buffer.CpuAddr() + buffer.SizeBytes(), device_addr_end);
        u32 size = static_cast<u32>(end - start);
        SynchronizeBuffer(buffer, start, size, false, false);
    });
}

void BufferCache::WriteDataBuffer(Buffer& buffer, VAddr address, const void* value, u32 num_bytes) {
    vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = buffer.Offset(address),
        .size = num_bytes,
    };
    vk::Buffer src_buffer = staging_buffer.Handle();
    if (num_bytes < StagingBufferSize) {
        const auto [staging, offset] = staging_buffer.Map(num_bytes);
        std::memcpy(staging, value, num_bytes);
        copy.srcOffset = offset;
        staging_buffer.Commit();
    } else {
        // For large one time transfers use a temporary host buffer.
        // RenderDoc can lag quite a bit if the stream buffer is too large.
        Buffer temp_buffer{
            instance, scheduler, MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc,
            num_bytes};
        src_buffer = temp_buffer.Handle();
        u8* const staging = temp_buffer.mapped_data.data();
        std::memcpy(staging, value, num_bytes);
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable {});
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(src_buffer, buffer.Handle(), copy);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

void BufferCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    const bool aggressive = total_used_memory >= critical_gc_memory;
    const u64 ticks_to_destroy = std::min<u64>(aggressive ? 80 : 160, gc_tick);
    int max_deletions = aggressive ? 64 : 32;
    const auto clean_up = [&](BufferId buffer_id) {
        if (max_deletions == 0) {
            return;
        }
        --max_deletions;
        Buffer& buffer = slot_buffers[buffer_id];
        // InvalidateMemory(buffer.CpuAddr(), buffer.SizeBytes());
        DownloadBufferMemory<true>(buffer, buffer.CpuAddr(), buffer.SizeBytes());
        memory_tracker->MarkRegionAsCpuModified(buffer.CpuAddr(), buffer.SizeBytes());
        DeleteBuffer(buffer_id);
    };
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
    lru_cache.Touch(buffer.LRUId(), gc_tick);
}

void BufferCache::DeleteBuffer(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    Unregister(buffer_id);
    scheduler.DeferOperation([this, buffer_id] { slot_buffers.erase(buffer_id); });
    buffer.is_deleted = true;
}

} // namespace VideoCore
