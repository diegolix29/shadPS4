// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <span>
#include <vector>

#include "common/alignment.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

using SyncRange = VideoCore::BufferCache::SyncRange;

/// Collects the source or destination ranges of a batch of copies, sorted and merged when they
/// touch the same or adjacent pages, so the merged ranges cover no page the copies do not.
static void CollectCopyRanges(std::span<const vk::BufferCopy> batch, VAddr base, bool source,
                              std::vector<SyncRange>& ranges) {
    static constexpr u64 TrackerPageSize = 4_KB;
    ranges.clear();
    for (const auto& copy : batch) {
        const VAddr offset = source ? copy.srcOffset : copy.dstOffset;
        ranges.push_back({base + offset, static_cast<u32>(copy.size)});
    }
    std::ranges::sort(ranges, {}, &SyncRange::device_addr);
    size_t last = 0;
    for (size_t i = 1; i < ranges.size(); ++i) {
        const VAddr last_end = ranges[last].device_addr + ranges[last].size;
        const VAddr next_end = ranges[i].device_addr + ranges[i].size;
        if (Common::AlignDown(ranges[i].device_addr, TrackerPageSize) <=
            Common::AlignUp(last_end, TrackerPageSize)) {
            ranges[last].size =
                static_cast<u32>(std::max(last_end, next_end) - ranges[last].device_addr);
        } else {
            ranges[++last] = ranges[i];
        }
    }
    ranges.resize(last + 1);
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& scheduler = rasterizer.GetScheduler();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    scheduler.EndRendering();

    static constexpr vk::MemoryBarrier READ_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
    };
    static constexpr vk::MemoryBarrier WRITE_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
    };
    static constexpr vk::MemoryBarrier UPLOAD_BARRIER{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
    };
    scheduler.CommandBuffer().pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlagBits::eByRegion, READ_BARRIER, {}, {});

    static std::vector<SyncRange> ranges;

    static constexpr vk::DeviceSize MaxDistanceForMerge = 64_MB;
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges, synchronized only where
        // the copies read and write: batches span up to 64 MB of sparse copies.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        const auto obtain = [&](VAddr base, u64 offset_min, u64 offset_max, bool is_written) {
            const VAddr addr = base + offset_min;
            const u32 size = static_cast<u32>(offset_max - offset_min);
            if (size <= VideoCore::BufferCache::CACHING_PAGESIZE) {
                return buffer_cache.ObtainBuffer(addr, size, is_written);
            }
            CollectCopyRanges(vk_copies, base, !is_written, ranges);
            return buffer_cache.ObtainBufferForRanges(addr, size, ranges, is_written);
        };
        // The uploads share one barrier before the copies. The barrier above already orders
        // earlier work before the uploads of the first batch; later batches upload after the
        // copies of the previous one.
        buffer_cache.BeginUploadBarrierBatch(batch_start != 0);
        const auto [src_buf, src_buf_offset] =
            obtain(src_buf_sharp.base_address, src_offset_min, src_offset_max, false);
        const auto [dst_buf, dst_buf_offset] =
            obtain(dst_buf_sharp.base_address, dst_offset_min, dst_offset_max, true);
        if (buffer_cache.EndUploadBarrierBatch()) {
            scheduler.CommandBuffer().pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, UPLOAD_BARRIER, {}, {});
        }

        // Apply found buffer base.
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        scheduler.CommandBuffer().copyBuffer(src_buf->Handle(), dst_buf->Handle(), vk_copies);
        batch_start = batch_end;
    }

    scheduler.CommandBuffer().pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlagBits::eByRegion, WRITE_BARRIER, {}, {});

    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
