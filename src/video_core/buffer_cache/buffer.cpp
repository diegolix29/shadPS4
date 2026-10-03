// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/config.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

Common::IncrementalIdProvider<u64> Buffer::global_uid{};

std::string_view BufferTypeName(MemoryUsage type) {
    switch (type) {
    case MemoryUsage::Upload:
        return "Upload";
    case MemoryUsage::Download:
        return "Download";
    case MemoryUsage::Stream:
        return "Stream";
    case MemoryUsage::DeviceLocal:
        return "DeviceLocal";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
    return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::Upload:
    case MemoryUsage::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryUsage::Download:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryUsage::DeviceLocal:
        return {};
    }
    return {};
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
    case MemoryUsage::Stream:
        if (Config::getUseHostMemoryFallback()) {
            static bool logged_once = false;
            if (!logged_once) {
                LOG_INFO(Render,
                         "Host memory fallback enabled - using system RAM for GPU allocations");
                logged_once = true;
            }
            return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryUsage::Upload:
    case MemoryUsage::Download:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    if (buffer) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
}

void UniqueBuffer::Create(const vk::BufferCreateInfo& buffer_ci, MemoryUsage usage,
                          VmaAllocationInfo* out_alloc_info) {
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    const VmaAllocationCreateFlags bda_flag =
        with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage),
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    VkBuffer unsafe_buffer{};
    VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                      &allocation, out_alloc_info);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
               vk::to_string(vk::Result{result}));
    buffer = vk::Buffer{unsafe_buffer};

    if (with_bda) {
        vk::BufferDeviceAddressInfo bda_info{
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_, MemoryUsage usage_,
               VAddr cpu_addr_, vk::BufferUsageFlags flags, u64 size_bytes_, bool transfer_shared)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, uid{global_uid.Next()}, instance{&instance_},
      scheduler{&scheduler_}, usage{usage_},
      buffer{instance->GetDevice(), instance->GetAllocator()} {
    // Create buffer object.
    const bool concurrent = transfer_shared && instance->HasTransferQueue();
    const std::array queue_families{instance->GetGraphicsQueueFamilyIndex(),
                                    concurrent ? instance->GetTransferQueueFamilyIndex() : 0U};
    const vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = flags,
        .sharingMode = concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = concurrent ? static_cast<u32>(queue_families.size()) : 0U,
        .pQueueFamilyIndices = concurrent ? queue_families.data() : nullptr,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, usage, &alloc_info);

    const auto device = instance->GetDevice();
    Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);

    // Map it if it is host visible.
    VkMemoryPropertyFlags property_flags{};
    vmaGetAllocationMemoryProperties(instance->GetAllocator(), buffer.allocation, &property_flags);
    if (alloc_info.pMappedData) {
        mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
    }
    is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

void Buffer::Fill(u64 offset, u32 num_bytes, u32 value) {
    scheduler->EndRendering();
    ASSERT_MSG(offset % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer size must be a multiple of 4 bytes");
    const auto cmdbuf = scheduler->CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.fillBuffer(buffer, offset, num_bytes, value);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

void Buffer::Invalidate(u64 offset, u64 size) {
    ASSERT(offset + size <= size_bytes);
    if (!is_coherent && usage == MemoryUsage::Download) {
        vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset, size);
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    ASSERT(offset + size <= size_bytes);
    if (!is_coherent && mapped_data.data() != nullptr) {
        vmaFlushAllocation(instance->GetAllocator(), buffer.allocation, offset, size);
    }
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x4000;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x1000;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                           MemoryUsage usage, u64 size_bytes, bool transfer_shared)
    : Buffer{instance, scheduler, usage, 0, AllFlags, size_bytes, transfer_shared} {
    ReserveWatches(current_watches, current_watch_pins, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, previous_watch_pins, WATCHES_INITIAL_RESERVE);
    const auto device = instance.GetDevice();
    Vulkan::SetObjectName(device, Handle(), "StreamBuffer({}):{:#x}", BufferTypeName(usage),
                          size_bytes);
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!is_coherent && usage == MemoryUsage::Stream) {
        size = Common::AlignUp(size, instance->NonCoherentAtomSize());
    }

    if (size > this->size_bytes) {
        return {nullptr, 0};
    }

    mapped_size = size;

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;
        ++generation;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        std::swap(previous_watch_pins, current_watch_pins);
        wait_cursor = 0;
        wait_bound = 0;
    }

    const u64 mapped_upper_bound = offset + size;
    if (!WaitPendingOperations(mapped_upper_bound, allow_wait)) {
        return {nullptr, 0};
    }

    return {mapped_data.data() + offset, offset};
}

size_t StreamBuffer::CommitWatch(bool pinned) {
    if (!is_coherent) {
        if (usage == MemoryUsage::Download) {
            vmaInvalidateAllocation(instance->GetAllocator(), buffer.allocation, offset,
                                    mapped_size);
        } else {
            vmaFlushAllocation(instance->GetAllocator(), buffer.allocation, offset, mapped_size);
        }
    }

    offset += mapped_size;
    if (current_watch_cursor != 0 && !pinned &&
        (current_watch_pins.empty() || !current_watch_pins[current_watch_cursor - 1]) &&
        current_watches[current_watch_cursor - 1].tick == scheduler->CurrentTick()) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return NoNewWatch;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, current_watch_pins, WATCHES_RESERVE_CHUNK);
    }

    if (pinned && current_watch_pins.empty()) {
        current_watch_pins.resize(current_watches.size());
    }
    const size_t watch_index = current_watch_cursor++;
    auto& watch = current_watches[watch_index];
    watch.upper_bound = offset;
    watch.tick = scheduler->CurrentTick();
    return watch_index;
}

// The unpinned overload keeps shared_ptr destruction out of the common path.
void StreamBuffer::Commit() {
    const size_t watch_index = CommitWatch(false);
    if (watch_index != NoNewWatch && !current_watch_pins.empty()) {
        current_watch_pins[watch_index].reset();
    }
}

void StreamBuffer::Commit(StreamBufferPinHandle pin) {
    const size_t watch_index = CommitWatch(pin != nullptr);
    if (watch_index != NoNewWatch && !current_watch_pins.empty()) {
        current_watch_pins[watch_index] = std::move(pin);
    }
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches,
                                  std::vector<StreamBufferPinHandle>& pins,
                                  std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
    if (!pins.empty()) {
        pins.resize(watches.size());
    }
}

SHAD_NO_INLINE bool StreamBuffer::WaitPendingWatches(u64 requested_upper_bound, bool allow_wait) {
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        auto pin = previous_watch_pins.empty() ? StreamBufferPinHandle{}
                                               : previous_watch_pins[wait_cursor];
        const u64 required_tick = pin ? pin->RequiredTick(watch.tick) : watch.tick;
        if ((!scheduler->IsFree(required_tick) ||
              (pin && !pin->IsReleased())) &&
            !allow_wait) {
            return false;
        }
        {
            scheduler->Wait(required_tick);
        }
        if (pin) {
            pin->Reclaim();
            if (!pin->IsReleased()) {
                return false;
            }
            previous_watch_pins[wait_cursor].reset();
        }
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

StreamBufferMapping::StreamBufferMapping(StreamBuffer& stream_buffer, u64 size, u64 alignment,
                                         bool allow_wait) {
    const auto [data, offset] = stream_buffer.Map(size, alignment, allow_wait);
    if (!data) {
        // This happens if the size is too big or no waiting is allowed when it is required
        is_temp_buffer = true;
        this->buffer = new VideoCore::Buffer(*stream_buffer.instance, *stream_buffer.scheduler,
                                             stream_buffer.usage, 0, AllFlags, size);
        this->data = this->buffer->mapped_data.data();
        this->offset = 0;
        ASSERT_MSG(this->data, "Failed to map temporary buffer");
    } else {
        is_temp_buffer = false;
        buffer = &stream_buffer;
        this->data = data;
        this->offset = offset;
    }
}

StreamBufferMapping::~StreamBufferMapping() {
    if (is_temp_buffer) {
        ASSERT(buffer);
        auto scheduler = buffer->scheduler;
        scheduler->DeferOperation([buffer = this->buffer]() mutable { delete buffer; });
    }
}

} // namespace VideoCore