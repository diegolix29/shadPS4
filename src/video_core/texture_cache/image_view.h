// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
struct ColorBuffer;
}

namespace Shader {
struct ImageResource;
}

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

struct ImageViewInfo {
    ImageViewInfo() = default;
    ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept;
    ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept;
    ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl);

    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    SubresourceRange range;
    vk::ComponentMapping mapping{};
    u32 min_lod = 0;
    bool is_storage = false;

    bool operator==(const ImageViewInfo& other) const noexcept {
        const auto pack = [](const auto low, const auto high) noexcept {
            return static_cast<u64>(static_cast<u32>(low)) |
                   (static_cast<u64>(static_cast<u32>(high)) << 32);
        };
        const u64 different =
            (pack(type, format) ^ pack(other.type, other.format)) |
            (pack(range.base.level, range.base.layer) ^
             pack(other.range.base.level, other.range.base.layer)) |
            (pack(range.extent.levels, range.extent.layers) ^
             pack(other.range.extent.levels, other.range.extent.layers)) |
            (pack(mapping.r, mapping.g) ^ pack(other.mapping.r, other.mapping.g)) |
            (pack(mapping.b, mapping.a) ^ pack(other.mapping.b, other.mapping.a)) |
            (pack(min_lod, is_storage) ^ pack(other.min_lod, other.is_storage));
        return different == 0;
    }

    auto operator<=>(const ImageViewInfo&) const = default;
};

struct Image;

struct ImageView {
    ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info, const Image& image);
    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    ImageView(ImageView&&) = default;
    ImageView& operator=(ImageView&&) = default;

    ImageViewInfo info;
    vk::UniqueImageView image_view;
};

} // namespace VideoCore