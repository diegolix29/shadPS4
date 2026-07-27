// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>

#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"

namespace VideoCore {

struct ImageInfo;
struct Image;
class StreamBuffer;

class TileManager {
    static constexpr size_t NUM_BPPS = 5;

public:
    using ScratchBuffer = std::pair<vk::Buffer, VmaAllocation>;
    using Result = std::pair<vk::Buffer, u32>;

    explicit TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer);
    ~TileManager();

    /// Writes the image tiled into out_buffer. A tiled image writes only the tiled bytes in
    /// [range_begin, range_end), counted from its start.
    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   vk::Buffer out_buffer, u32 out_offset, u32 copy_size, u32 range_begin = 0,
                   u32 range_end = std::numeric_limits<u32>::max());

    /// in_host_memory: the tiled data lives in host memory, where the scattered reads of the
    /// detiler cross the bus one small request at a time.
    Result DetileImage(vk::Buffer in_buffer, u32 in_offset, const ImageInfo& info,
                       bool in_host_memory = false);

    /// Tiles linear data from in_buffer into a new scratch buffer.
    /// Returns the scratch buffer; caller must ensure GPU is finished before reading.
    Result TileLinearBuffer(vk::Buffer in_buffer, u32 in_offset, const ImageInfo& info);

private:
    vk::Pipeline GetTilingPipeline(const ImageInfo& info, bool is_tiler, bool from_image = false);
    ScratchBuffer GetScratchBuffer(u32 size);
    /// Format of a uint view the tiler can read the image through, or eUndefined when the
    /// image has to be copied to a buffer first.
    [[nodiscard]] static vk::Format TilingViewFormat(const Image& image) noexcept;

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    StreamBuffer& stream_buffer;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> detilers{};
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> tilers{};
    vk::UniqueDescriptorSetLayout image_desc_layout;
    vk::UniquePipelineLayout image_pl_layout;
    std::array<vk::UniquePipeline, AmdGpu::NUM_TILE_MODES * NUM_BPPS> image_tilers{};
};

} // namespace VideoCore
