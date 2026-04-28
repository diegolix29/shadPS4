// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include "common/alignment.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/draw_bundle.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {

static constexpr size_t DataShareBufferSize = 64_KB;
static constexpr size_t StagingBufferSize = 512_MB;
static constexpr size_t DownloadBufferSize = 32_MB;
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         AmdGpu::Liverpool* liverpool_, TextureCache& texture_cache_,
                         PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      fault_manager{instance, scheduler, *this, CACHING_PAGEBITS, CACHING_NUMPAGES},
      staging_buffer{instance, scheduler, MemoryUsage::Upload, StagingBufferSize},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, MemoryUsage::DeviceLocal,
                           0,        AllFlags,  BDA_PAGETABLE_SIZE} {
    Vulkan::SetObjectName(instance.GetDevice(), gds_buffer.Handle(), "GDS Buffer");
    Vulkan::SetObjectName(instance.GetDevice(), bda_pagetable_buffer.Handle(),
                          "BDA Page Table Buffer");

    memory_tracker = std::make_unique<MemoryTracker>(tracker);

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    // Ensure the first slot is used for the null buffer
    const auto null_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
    ASSERT(null_id.index == 0);
    const vk::Buffer& null_buffer = slot_buffers[null_id].buffer;
    Vulkan::SetObjectName(instance.GetDevice(), null_buffer, "Null Buffer");

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

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    if (!IsRegionRegistered(device_addr, size)) {
        return;
    }
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    const u64 page = device_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page].buffer_id;

    // --- RYZEN 7840U / Z1 EXTREME OPTIMIZATION ---
    // Gravity Rush 2 Physics Hack
    // Forces small physics buffers (Havok data) to sync asynchronously.
    // This eliminates the heavy stuttering during gravity shifts and combat.
    bool likely_physics_stall = (size > 1024 && size < 24576);
    // ---------------------------------------------

    liverpool->SendCommand<true>([this, device_addr, size, is_write, likely_physics_stall] {
        Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];

        if (likely_physics_stall) {
            // Fast Path: Don't wait for GPU. Great for ragdolls/debris.
            DownloadBufferMemory<true>(buffer, device_addr, size, is_write);
        } else {
            // Safe Path: Wait for GPU. Necessary for game logic/visuals.
            DownloadBufferMemory<false>(buffer, device_addr, size, is_write);
        }
    });
}

template <bool async>
void BufferCache::DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size, bool is_write) {
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
    cmdbuf.copyBuffer(buffer.buffer, download_buffer.Handle(), copies);
    const auto write_data = [&]() {
        auto* memory = Core::Memory::Instance();
        for (const auto& copy : copies) {
            const VAddr copy_device_addr = buffer.CpuAddr() + copy.srcOffset;
            const u64 dst_offset = copy.dstOffset - offset;
            memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr), download + dst_offset,
                                    copy.size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if constexpr (async) {
        scheduler.DeferOperation(write_data);
    } else {
        scheduler.Finish();
        write_data();
    }
}

void BufferCache::BindVertexBuffers(const Vulkan::GraphicsPipeline& pipeline) {
    const auto& regs = liverpool->regs;
    Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    Vulkan::VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    // Hot-path: split "vertex input state" from "guest buffer binds".
    // - setVertexInputEXT depends only on bindings/attributes/divisors (+ step rates), NOT buffer addresses.
    // - bindVertexBuffers depends on guest buffer addresses/offsets and can change every draw.
    auto mix = [](u64& h, u64 v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };

    u64 input_sig = 0xcbf29ce484222325ULL; // arbitrary non-zero seed
    mix(input_sig, static_cast<u64>(regs.vgt_instance_step_rate_0));
    mix(input_sig, static_cast<u64>(regs.vgt_instance_step_rate_1));
    mix(input_sig, static_cast<u64>(instance.IsVertexInputDynamicState() ? 1 : 0));
    mix(input_sig, reinterpret_cast<u64>(&pipeline));

    for (const auto& b : bindings) {
        mix(input_sig, (static_cast<u64>(b.binding) << 32) | static_cast<u64>(b.stride));
        mix(input_sig, static_cast<u64>(static_cast<u32>(b.inputRate)));
    }
    for (const auto& a : attributes) {
        mix(input_sig, (static_cast<u64>(a.location) << 32) | static_cast<u64>(a.binding));
        mix(input_sig, (static_cast<u64>(static_cast<u32>(a.format)) << 32) | static_cast<u64>(a.offset));
    }
    for (const auto& d : divisors) {
        mix(input_sig, (static_cast<u64>(d.binding) << 32) | static_cast<u64>(d.divisor));
    }

    // Full bind signature includes guest buffers (addresses/sizes/strides).
    u64 bind_sig = input_sig;
    for (const auto& gb : guest_buffers) {
        mix(bind_sig, static_cast<u64>(gb.base_address));
        mix(bind_sig, static_cast<u64>(gb.GetSize()));
        mix(bind_sig, static_cast<u64>(gb.GetStride()));
    }

    // If NOTHING changed (vertex input + guest buffers), skip ALL rebinding work.
    if (last_vertex_bind_sig_valid && bind_sig == last_vertex_bind_sig) {
        return;
    }

    // Only call setVertexInputEXT when vertex INPUT state changed.
    if (instance.IsVertexInputDynamicState()) {
        if (!last_vertex_input_sig_valid || input_sig != last_vertex_input_sig) {
            const auto cmdbuf = scheduler.CommandBuffer();
            cmdbuf.setVertexInputEXT(bindings, attributes);
            last_vertex_input_sig = input_sig;
            last_vertex_input_sig_valid = true;
        }
    }

    last_vertex_bind_sig = bind_sig;
    last_vertex_bind_sig_valid = true;


    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    // PERF(GR2 v16): Fast path for the common single-active-buffer case.
    // GR2 draws typically bind 1-2 vertex buffers. When there's only one non-empty
    // buffer, skip the sort + merge + find_if entirely (saves ~0.3% of GpuComm).
    Vulkan::VertexInputs<vk::Buffer> host_buffers;
    Vulkan::VertexInputs<vk::DeviceSize> host_offsets;
    Vulkan::VertexInputs<vk::DeviceSize> host_sizes;
    Vulkan::VertexInputs<vk::DeviceSize> host_strides;
    const auto null_buffer =
        instance.IsNullDescriptorSupported() ? VK_NULL_HANDLE : GetBuffer(NULL_BUFFER_ID).Handle();

    // Count non-empty buffers to decide which path to take.
    u32 non_empty_count = 0;
    for (const auto& buffer : guest_buffers) {
        non_empty_count += (buffer.GetSize() > 0) ? 1 : 0;
    }

    if (non_empty_count <= 1) {
        // Single-buffer fast path: no sort/merge needed.
        // Just obtain the one buffer directly and map all guest buffers to it.
        vk::Buffer single_vk_buffer{};
        u64 single_offset = 0;
        VAddr single_base = 0;
        VAddr single_end = 0;
        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0 && !single_vk_buffer) {
                const u64 size = memory->ClampRangeSize(buffer.base_address, buffer.GetSize());
                const auto [obtained, offset] = ObtainBuffer(buffer.base_address, size, false);
                single_vk_buffer = obtained->buffer;
                single_offset = offset;
                single_base = buffer.base_address;
                single_end = buffer.base_address + buffer.GetSize();
            }
        }
        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0) {
                host_buffers.emplace_back(single_vk_buffer);
                host_offsets.push_back(single_offset + buffer.base_address - single_base);
            } else {
                host_buffers.emplace_back(null_buffer);
                host_offsets.push_back(0);
            }
            host_sizes.push_back(buffer.GetSize());
            host_strides.push_back(buffer.GetStride());
        }
    } else {
        // Multi-buffer path: sort, merge, then map.
        struct BufferRange {
            VAddr base_address;
            VAddr end_address;
            vk::Buffer vk_buffer;
            u64 offset;

            [[nodiscard]] size_t GetSize() const {
                return end_address - base_address;
            }
        };

        Vulkan::VertexInputs<BufferRange> ranges{};
        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }

        // Merge connecting ranges together
        Vulkan::VertexInputs<BufferRange> ranges_merged{};
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }

        // Map buffers for merged ranges
        for (auto& range : ranges_merged) {
            const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
            const auto [buffer, offset] = ObtainBuffer(range.base_address, size, false);
            range.vk_buffer = buffer->buffer;
            range.offset = offset;
        }

        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0) {
                const auto host_buffer_info =
                    std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                        return buffer.base_address >= range.base_address &&
                               buffer.base_address < range.end_address;
                    });
                ASSERT(host_buffer_info != ranges_merged.cend());
                host_buffers.emplace_back(host_buffer_info->vk_buffer);
                host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                       host_buffer_info->base_address);
            } else {
                host_buffers.emplace_back(null_buffer);
                host_offsets.push_back(0);
            }
            host_sizes.push_back(buffer.GetSize());
            host_strides.push_back(buffer.GetStride());
        }
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void BufferCache::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->regs;

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;
    const u32 index_buffer_size = regs.num_indices * index_size;

    // OPT(v18): Skip redundant index buffer bind when same buffer is already bound.
    const auto cmdbuf = scheduler.CommandBuffer();
    const u64 tick = scheduler.CurrentTick();
    if (last_index_address_ == index_address && last_index_buffer_size_ == index_buffer_size &&
        last_index_type_ == index_type && last_index_cmdbuf_ == cmdbuf &&
        last_index_tick_ == tick) {
        return;
    }

    // Bind index buffer.
    const auto [vk_buffer, offset] = ObtainBuffer(index_address, index_buffer_size, false);
    cmdbuf.bindIndexBuffer(vk_buffer->Handle(), offset, index_type);

    last_index_address_ = index_address;
    last_index_buffer_size_ = index_buffer_size;
    last_index_type_ = index_type;
    last_index_cmdbuf_ = cmdbuf;
    last_index_tick_ = tick;
}

void BufferCache::ResolveVertexBuffers(const Vulkan::GraphicsPipeline& pipeline,
                                        Vulkan::DrawBundle& bundle) {
    const auto& regs = liverpool->regs;
    Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    Vulkan::VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    auto mix = [](u64& h, u64 v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };

    u64 input_sig = 0xcbf29ce484222325ULL;
    mix(input_sig, static_cast<u64>(regs.vgt_instance_step_rate_0));
    mix(input_sig, static_cast<u64>(regs.vgt_instance_step_rate_1));
    mix(input_sig, static_cast<u64>(instance.IsVertexInputDynamicState() ? 1 : 0));
    mix(input_sig, reinterpret_cast<u64>(&pipeline));

    for (const auto& b : bindings) {
        mix(input_sig, (static_cast<u64>(b.binding) << 32) | static_cast<u64>(b.stride));
        mix(input_sig, static_cast<u64>(static_cast<u32>(b.inputRate)));
    }
    for (const auto& a : attributes) {
        mix(input_sig, (static_cast<u64>(a.location) << 32) | static_cast<u64>(a.binding));
        mix(input_sig, (static_cast<u64>(static_cast<u32>(a.format)) << 32) | static_cast<u64>(a.offset));
    }
    for (const auto& d : divisors) {
        mix(input_sig, (static_cast<u64>(d.binding) << 32) | static_cast<u64>(d.divisor));
    }

    u64 bind_sig = input_sig;
    for (const auto& gb : guest_buffers) {
        mix(bind_sig, static_cast<u64>(gb.base_address));
        mix(bind_sig, static_cast<u64>(gb.GetSize()));
        mix(bind_sig, static_cast<u64>(gb.GetStride()));
    }

    // Two-level cache matching Phase 1B's BindVertexBuffers:
    // Level 1 (bind_sig): full VB+pipeline hash. If unchanged, skip everything
    //   including setVertexInputEXT — previous cmdbuf state persists.
    // Level 2 (input_sig): vertex format hash. If unchanged, skip setVertexInputEXT
    //   but still fill VB data in the bundle.
    if (last_vertex_bind_sig_valid && bind_sig == last_vertex_bind_sig) {
        bundle.has_vertex_buffers = false;
        return;
    }

    if (instance.IsVertexInputDynamicState()) {
        if (!last_vertex_input_sig_valid || input_sig != last_vertex_input_sig) {
            scheduler.DrainRecorderQueue();
            const auto cmdbuf = scheduler.CommandBuffer();
            cmdbuf.setVertexInputEXT(bindings, attributes);
            last_vertex_input_sig = input_sig;
            last_vertex_input_sig_valid = true;
        }
    }

    last_vertex_bind_sig = bind_sig;
    last_vertex_bind_sig_valid = true;

    if (bindings.empty()) {
        bundle.has_vertex_buffers = false;
        return;
    }

    Vulkan::VertexInputs<vk::Buffer> host_buffers;
    Vulkan::VertexInputs<vk::DeviceSize> host_offsets;
    Vulkan::VertexInputs<vk::DeviceSize> host_sizes;
    Vulkan::VertexInputs<vk::DeviceSize> host_strides;
    const auto null_buffer =
        instance.IsNullDescriptorSupported() ? VK_NULL_HANDLE : GetBuffer(NULL_BUFFER_ID).Handle();

    u32 non_empty_count = 0;
    for (const auto& buffer : guest_buffers) {
        non_empty_count += (buffer.GetSize() > 0) ? 1 : 0;
    }

    if (non_empty_count <= 1) {
        vk::Buffer single_vk_buffer{};
        u64 single_offset = 0;
        VAddr single_base = 0;
        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0 && !single_vk_buffer) {
                const u64 size = memory->ClampRangeSize(buffer.base_address, buffer.GetSize());
                const auto [obtained, offset] = ObtainBuffer(buffer.base_address, size, false);
                single_vk_buffer = obtained->buffer;
                single_offset = offset;
                single_base = buffer.base_address;
            }
        }
        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0) {
                host_buffers.emplace_back(single_vk_buffer);
                host_offsets.push_back(single_offset + buffer.base_address - single_base);
            } else {
                host_buffers.emplace_back(null_buffer);
                host_offsets.push_back(0);
            }
            host_sizes.push_back(buffer.GetSize());
            host_strides.push_back(buffer.GetStride());
        }
    } else {
        struct BufferRange {
            VAddr base_address;
            VAddr end_address;
            vk::Buffer vk_buffer;
            u64 offset;
            [[nodiscard]] size_t GetSize() const { return end_address - base_address; }
        };

        Vulkan::VertexInputs<BufferRange> ranges{};
        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }

        Vulkan::VertexInputs<BufferRange> ranges_merged{};
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }

        for (auto& range : ranges_merged) {
            const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
            const auto [buffer, offset] = ObtainBuffer(range.base_address, size, false);
            range.vk_buffer = buffer->buffer;
            range.offset = offset;
        }

        for (const auto& buffer : guest_buffers) {
            if (buffer.GetSize() > 0) {
                const auto host_buffer_info =
                    std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                        return buffer.base_address >= range.base_address &&
                               buffer.base_address < range.end_address;
                    });
                ASSERT(host_buffer_info != ranges_merged.cend());
                host_buffers.emplace_back(host_buffer_info->vk_buffer);
                host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                       host_buffer_info->base_address);
            } else {
                host_buffers.emplace_back(null_buffer);
                host_offsets.push_back(0);
            }
            host_sizes.push_back(buffer.GetSize());
            host_strides.push_back(buffer.GetStride());
        }
    }

    // Write resolved data into bundle instead of calling cmdbuf.
    bundle.CopyVertexBuffers(host_buffers.data(), host_offsets.data(),
                              host_sizes.data(), host_strides.data(),
                              static_cast<u32>(guest_buffers.size()));
}

void BufferCache::ResolveIndexBuffer(u32 index_offset, Vulkan::DrawBundle& bundle) {
    const auto& regs = liverpool->regs;

    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;
    const u32 index_buffer_size = regs.num_indices * index_size;

    const auto [vk_buffer, offset] = ObtainBuffer(index_address, index_buffer_size, false);
    bundle.index_buffer = vk_buffer->Handle();
    bundle.index_buffer_offset = offset;
    bundle.index_type = index_type;
    bundle.has_index_bind = true;
}

void BufferCache::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0, "GDS offset must be dword aligned");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!IsRegionGpuModified(address, num_bytes)) {
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
    if (!dst_gds && !IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
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
            // OPT: Narrow from eAllCommands. Dst buffer only needs to wait for prior
            // shader/transfer writes before becoming a transfer destination.
            .srcStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader |
                            vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            // OPT: Source buffer needs prior writes visible before transfer read.
            .srcStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader |
                            vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
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
            // OPT: After transfer write, make visible to shader reads/writes.
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            // OPT: Source buffer after transfer read only needs future writes visible.
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                            vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
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
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= CACHING_PAGESIZE && !IsRegionGpuModified(device_addr, size)) {
        // ULTRA(GR2): XXH3 hashing of small UBO payloads became a top GpuComm hotspot.
        // Avoid hashing entirely by using the CPU page tracker:
        //   - only reuse a prior stream-buffer copy when the range is currently NOT CPU-modified
        //   - otherwise do a fresh Copy().
        struct StreamCopyCacheEntry {
            vk::CommandBuffer cmdbuf{};
            u64 tick{};
            VAddr addr{};
            u32 size{};
            u64 offset{};
            bool valid{false};
        };
        static thread_local std::array<StreamCopyCacheEntry, 256> cache{};

        const auto cmdbuf = scheduler.CommandBuffer();
        const u64 tick = scheduler.CurrentTick();
        const u64 key = (static_cast<u64>(device_addr) >> 4) ^ (static_cast<u64>(size) << 1);
        StreamCopyCacheEntry& e = cache[key & (cache.size() - 1)];

        if (e.valid && e.cmdbuf == cmdbuf && e.tick == tick && e.addr == device_addr && e.size == size) {
            if (!IsRegionCpuModified(device_addr, size)) {
                return {&stream_buffer, e.offset};
            }
        }

        const u64 offset = stream_buffer.Copy(device_addr, size, instance.UniformMinAlignment());
        e.cmdbuf = cmdbuf;
        e.tick = tick;
        e.addr = device_addr;
        e.size = size;
        e.offset = offset;
        e.valid = true;
        return {&stream_buffer, offset};
    }
    if (IsBufferInvalid(buffer_id)) {
        buffer_id = FindBuffer(device_addr, size);
    }
    Buffer& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {&buffer, buffer.Offset(device_addr)};
}

std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(VAddr gpu_addr, u32 size) {
    // Check if any buffer contains the full requested range.
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            SynchronizeBuffer(buffer, gpu_addr, size, false, false);
            return {&buffer, buffer.Offset(gpu_addr)};
        }
    }
    // If some buffer within was GPU modified create a full buffer to avoid losing GPU data.
    if (IsRegionGpuModified(gpu_addr, size)) {
        return ObtainBuffer(gpu_addr, size, false, false);
    }
    // In all other cases, just do a CPU copy to the staging buffer.
    const auto [data, offset] = staging_buffer.Map(size, 16);
    memory->CopySparseMemory(gpu_addr, data, size);
    staging_buffer.Commit();
    return {&staging_buffer, offset};
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

BufferId BufferCache::FindBuffer(VAddr device_addr, u32 size) {
    if (device_addr == 0) {
        return NULL_BUFFER_ID;
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
                expand_begin(CACHING_PAGESIZE * 128);
            }
            if (expands_left) {
                expand_end(CACHING_PAGESIZE * 128);
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
}

void BufferCache::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
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
    // Coalesce adjacent upload ranges to reduce CopySparseMemory/memmove calls.
    // Safe because copies are constructed with contiguous srcOffset (total_size_bytes accumulator).
    if (copies.size() > 1) {
        size_t out = 0;
        for (size_t i = 0; i < copies.size(); ++i) {
            const auto cur = copies[i];
            if (out > 0) {
                auto& prev = copies[out - 1];
                const bool contiguous_src = (cur.srcOffset == prev.srcOffset + prev.size);
                const bool contiguous_dst = (cur.dstOffset == prev.dstOffset + prev.size);
                if (contiguous_src && contiguous_dst) {
                    prev.size += cur.size;
                    continue;
                }
            }
            copies[out++] = cur;
        }
        copies.resize(out);
    }
    if (src_buffer) {
        scheduler.EndRendering();
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::BufferMemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eTransferRead |
                             vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        const vk::BufferMemoryBarrier2 post_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &pre_barrier,
        });
        cmdbuf.copyBuffer(src_buffer, buffer.buffer, copies);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &post_barrier,
        });
        TouchBuffer(buffer);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeBufferFromImage(buffer, device_addr, size);
    }
    return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     size_t total_size_bytes) {
    if (copies.empty()) {
        return VK_NULL_HANDLE;
    }
    const auto [staging, offset] = staging_buffer.Map(total_size_bytes);
    if (staging) {
        for (auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            memory->CopySparseMemory(device_addr, src_pointer, copy.size);
            // Apply the staging offset
            copy.srcOffset += offset;
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
        for (const auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            memory->CopySparseMemory(device_addr, src_pointer, copy.size);
        }
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable { buffer.reset(); });
        return src_buffer;
    }
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
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
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size);
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
        // OPT: Only need to wait for prior shader/transfer access before writing.
        .srcStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                        vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                         vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        // OPT: After write, make visible to shader reads/writes.
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
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
        DownloadBufferMemory<true>(buffer, buffer.CpuAddr(), buffer.SizeBytes(), true);
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
