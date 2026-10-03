// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "common/incremental_id.h"
#include "common/types.h"
#include "core/memory.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/buffer_cache/stream_buffer_pin.h"
#include "video_core/flush_epoch.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

struct VmaAllocationInfo;

namespace VideoCore {

/// Hints and requirements for the backing memory type of a commit
enum class MemoryUsage {
    DeviceLocal, ///< Requests device local buffer.
    Upload,      ///< Requires a host visible memory type optimized for CPU to GPU uploads
    Download,    ///< Requires a host visible memory type optimized for GPU to CPU readbacks
    Stream,      ///< Requests device local host visible buffer, falling back host memory.
    HostCached = Download,
};
using MemoryType = MemoryUsage;

constexpr vk::BufferUsageFlags ReadFlags =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

constexpr vk::BufferUsageFlags AllFlags =
    ReadFlags | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;

struct UniqueBuffer {
    explicit UniqueBuffer(vk::Device device, VmaAllocator allocator);
    ~UniqueBuffer();

    UniqueBuffer(const UniqueBuffer&) = delete;
    UniqueBuffer& operator=(const UniqueBuffer&) = delete;

    UniqueBuffer(UniqueBuffer&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          buffer{std::exchange(other.buffer, VK_NULL_HANDLE)} {}
    UniqueBuffer& operator=(UniqueBuffer&& other) {
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        return *this;
    }

    void Create(const vk::BufferCreateInfo& image_ci, MemoryUsage usage,
                VmaAllocationInfo* out_alloc_info);

    operator vk::Buffer() const {
        return buffer;
    }

    vk::Device device;
    VmaAllocator allocator;
    VmaAllocation allocation;
    vk::Buffer buffer{};
    vk::DeviceAddress bda_addr = 0;
};

class Buffer {
public:
    /// With transfer_shared, the transfer queue family can access the buffer too.
    explicit Buffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                    MemoryUsage usage, VAddr cpu_addr_, vk::BufferUsageFlags flags,
                    u64 size_bytes_, bool transfer_shared = false);

    Buffer& operator=(const Buffer&) = delete;
    Buffer(const Buffer&) = delete;

    Buffer& operator=(Buffer&&) = default;
    Buffer(Buffer&&) = default;

    void IncreaseStreamScore(int score) noexcept {
        stream_score += score;
    }

    [[nodiscard]] int StreamScore() const noexcept {
        return stream_score;
    }

    [[nodiscard]] bool IsInBounds(VAddr addr, u64 size) const noexcept {
        return addr >= cpu_addr && addr + size <= cpu_addr + SizeBytes();
    }

    [[nodiscard]] VAddr CpuAddr() const noexcept {
        return cpu_addr;
    }

    [[nodiscard]] u64 Offset(VAddr other_cpu_addr) const noexcept {
        return other_cpu_addr - cpu_addr;
    }

    size_t SizeBytes() const {
        return size_bytes;
    }

    void SetLRUId(u64 id) noexcept {
        lru_id = id;
    }

    u64 LRUId() const noexcept {
        return lru_id;
    }

    u64 Uid() const noexcept {
        return uid;
    }

    vk::Buffer Handle() const noexcept {
        return buffer;
    }

    vk::DeviceAddress BufferDeviceAddress() const noexcept {
        ASSERT_MSG(buffer.bda_addr != 0, "Can't get BDA from a non BDA buffer");
        return buffer.bda_addr;
    }

    /// Orders an access after the ones recorded before it. A read waits only for the last
    /// write, and not at all once that write was made visible to its access and stage: reads
    /// are not ordered against each other. A write waits for the reads since the last write,
    /// which themselves waited for it, or for that write when nothing read the buffer since.
    std::optional<vk::BufferMemoryBarrier2> GetBarrier(vk::AccessFlags2 dst_access,
                                                       vk::PipelineStageFlagBits2 dst_stage,
                                                       u32 offset = 0) {
        constexpr vk::AccessFlags2 WriteAccess = vk::AccessFlagBits2::eShaderWrite |
                                                 vk::AccessFlagBits2::eTransferWrite |
                                                 vk::AccessFlagBits2::eMemoryWrite;
        const u64 epoch = FlushEpoch::Current();
        vk::PipelineStageFlags2 src_stage;
        vk::AccessFlags2 src_access;
        if (!(dst_access & WriteAccess)) {
            const bool access_visible =
                (visible_access & vk::AccessFlagBits2::eMemoryRead) ||
                (visible_access & dst_access) == dst_access;
            const bool stage_visible =
                (visible_stages & vk::PipelineStageFlagBits2::eAllCommands) ||
                (visible_stages & dst_stage) == vk::PipelineStageFlags2{dst_stage};
            read_stages |= dst_stage;
            if (access_visible && stage_visible) {
                return {};
            }
            src_stage = write_stage;
            src_access = write_access;
            visible_access |= dst_access;
            visible_stages |= dst_stage;
        } else {
            if (!read_stages && write_access == dst_access && write_stage == dst_stage) {
                // Read-write bindings in a row are only ordered across a guest cache flush.
                if (write_epoch == epoch) {
                    return {};
                }
            }
            if (read_stages) {
                src_stage = read_stages;
                src_access = {};
            } else {
                src_stage = write_stage;
                src_access = write_access;
            }
            write_access = dst_access;
            write_stage = dst_stage;
            write_epoch = epoch;
            visible_access = {};
            visible_stages = {};
            read_stages = {};
        }

        DEBUG_ASSERT(offset < size_bytes);

        const auto barrier = vk::BufferMemoryBarrier2{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .buffer = buffer.buffer,
            .offset = offset,
            .size = size_bytes - offset,
        };
        return barrier;
    }

    void Fill(u64 offset, u32 num_bytes, u32 value);

    /// Makes a completed download range visible to the CPU.
    void Invalidate(u64 offset, u64 size);

    /// Flushes a host-mapped write so the GPU sees it.
    void Flush(u64 offset, u64 size);

    [[nodiscard]] u64 LastTick() const noexcept {
        return last_tick;
    }

public:
    VAddr cpu_addr = 0;
    bool is_picked{};
    bool is_coherent{};
    bool is_deleted{};
    bool has_image_alias{};
    /// Advances whenever the cache uploads into the buffer or binds it for GPU writes.
    u64 content_generation{};
    int stream_score = 0;
    size_t size_bytes = 0;
    u64 lru_id = 0;
    u64 uid = 0;
    std::span<u8> mapped_data;
    const Vulkan::Instance* instance;
    Vulkan::Scheduler* scheduler;
    MemoryUsage usage;
    UniqueBuffer buffer;
    /// Last write access, the accesses and stages it was made visible to since, and the stages
    /// that read the buffer since.
    vk::AccessFlags2 write_access{
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
        vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite};
    vk::PipelineStageFlags2 write_stage{vk::PipelineStageFlagBits2::eAllCommands};
    vk::AccessFlags2 visible_access{};
    vk::PipelineStageFlags2 visible_stages{};
    vk::PipelineStageFlags2 read_stages{};
    /// FlushEpoch::Current() when a write access was last requested.
    u64 write_epoch{};
    u64 last_tick{};

private:
    static Common::IncrementalIdProvider<u64> global_uid;
};

class StreamBuffer : public Buffer {
public:
    explicit StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                          MemoryUsage usage, u64 size_bytes_, bool transfer_shared = false);

    /// Reserves a region of memory from the stream buffer.
    std::pair<u8*, u64> Map(u64 size, u64 alignment = 0, bool allow_wait = true);

    [[nodiscard]] std::optional<u64> Reserve(u64 size, u64 alignment = 0, bool deferred = false) {
        const auto [ptr, off] = Map(size, alignment, !deferred);
        if (!ptr) {
            return std::nullopt;
        }
        return off;
    }

    /// Ensures that reserved bytes of memory are available to the GPU.
    void Commit();
    void Commit(StreamBufferPinHandle pin);

    /// Returns the ring-buffer generation. It changes whenever allocations wrap to offset zero.
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }

    /// Maps and commits a memory region with user provided data
    u64 Copy(auto src, size_t size, size_t alignment = 0) {
        const auto [data, offset] = Map(size, alignment);
        auto* memory = Core::Memory::Instance();
        const VAddr src_vaddr = reinterpret_cast<const VAddr>(src);
        if (memory->IsValidMapping(src_vaddr)) {
            memory->CopySparseMemory(src_vaddr, data, size);
        } else {
            std::memcpy(data, reinterpret_cast<const void*>(src), size);
        }
        Commit();
        return offset;
    }

private:
    struct Watch {
        u64 tick{};
        u64 upper_bound{};
    };

    static constexpr size_t NoNewWatch = ~size_t{0};

    /// Flushes the mapped range and records it in a watch. Returns the index of the new watch,
    /// or NoNewWatch when the range was merged into the previous one.
    size_t CommitWatch(bool pinned);

    /// Increases the amount of watches available.
    void ReserveWatches(std::vector<Watch>& watches, std::vector<StreamBufferPinHandle>& pins,
                        std::size_t grow_size);

    /// Waits pending watches until requested upper bound.
    bool WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
        // What the loop of WaitPendingWatches checks first: most maps have nothing to wait for.
        if (!invalidation_mark || requested_upper_bound <= wait_bound ||
            wait_cursor >= *invalidation_mark) {
            return true;
        }
        return WaitPendingWatches(requested_upper_bound, allow_wait);
    }
    bool WaitPendingWatches(u64 requested_upper_bound, bool allow_wait);

private:
    u64 offset{};
    u64 mapped_size{};
    u64 generation{1};
    std::vector<Watch> current_watches;
    std::vector<StreamBufferPinHandle> current_watch_pins;
    std::size_t current_watch_cursor{};
    std::optional<size_t> invalidation_mark;
    std::vector<Watch> previous_watches;
    std::vector<StreamBufferPinHandle> previous_watch_pins;
    std::size_t wait_cursor{};
    u64 wait_bound{};
};

class StreamBufferMapping {
public:
    StreamBufferMapping(StreamBuffer& stream_buffer, u64 size, u64 alignment = 0,
                        bool allow_wait = true);
    ~StreamBufferMapping();

    StreamBufferMapping(const StreamBufferMapping&) = delete;
    StreamBufferMapping& operator=(const StreamBufferMapping&) = delete;

    StreamBufferMapping(StreamBufferMapping&& other)
        : buffer{std::exchange(other.buffer, nullptr)}, data{std::exchange(other.data, nullptr)},
          offset{std::exchange(other.offset, 0)},
          is_temp_buffer{std::exchange(other.is_temp_buffer, false)} {}

    StreamBufferMapping& operator=(StreamBufferMapping&& other) {
        if (this != &other) {
            buffer = std::exchange(other.buffer, nullptr);
            data = std::exchange(other.data, nullptr);
            offset = std::exchange(other.offset, 0);
            is_temp_buffer = std::exchange(other.is_temp_buffer, false);
        }
        return *this;
    }

    VideoCore::Buffer* Buffer() const {
        return buffer;
    }

    u8* Data() const {
        return data;
    }

    u64 Offset() const {
        return offset;
    }

    bool TemporaryBuffer() const {
        return is_temp_buffer;
    }

private:
    VideoCore::Buffer* buffer;
    u8* data{};
    u64 offset{};
    bool is_temp_buffer{};
};

} // namespace VideoCore