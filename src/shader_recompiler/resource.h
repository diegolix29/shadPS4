// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>

#include "common/types.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 16;
static constexpr u32 NUM_FMASKS = 8;

enum class BufferType : u32 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
};

struct Info;

using SharpLocation = u32;
static constexpr SharpLocation UNKNOWN_LOCATION = 0xFFFFFFFFu;

template <typename T>
struct SharpFetch {
    static constexpr u32 NumDwords = static_cast<u32>(sizeof(T) / sizeof(u32));
    std::array<u32, NumDwords> immediates{};
    std::array<SharpLocation, NumDwords> offsets{};
    u32 load_mask{};

    bool operator==(const SharpFetch&) const = default;

    T Fetch(const auto& info) const noexcept {
        std::array<u32, NumDwords> dwords = immediates;
        for (u32 i = 0; i < NumDwords; ++i) {
            if ((load_mask & (1u << i)) != 0 && offsets[i] != UNKNOWN_LOCATION) {
                dwords[i] = info.flattened_ud_buf[offsets[i]];
            }
        }
        T result{};
        std::memcpy(&result, dwords.data(), sizeof(T));
        return result;
    }

    bool UsesFetch() const noexcept {
        return load_mask != 0;
    }
};

// The rejections of invalid sharps are out of line so that resolving a valid sharp, done for
// every resource of every draw, does not carry the logging code.
[[nodiscard]] SHAD_NO_INLINE inline AmdGpu::Buffer RejectBufferSharp() noexcept {
    LOG_DEBUG(Render, "Encountered invalid buffer sharp");
    return AmdGpu::Buffer::Null();
}

[[nodiscard]] SHAD_NO_INLINE inline AmdGpu::Image RejectImageSharp(bool is_depth) noexcept {
    LOG_DEBUG(Render_Vulkan, "Encountered invalid image sharp");
    return AmdGpu::Image::Null(is_depth);
}

[[nodiscard]] SHAD_NO_INLINE inline AmdGpu::Image RejectDepthImageSharp() noexcept {
    LOG_DEBUG(Render_Vulkan, "Encountered non-depth image used with depth instruction!");
    return AmdGpu::Image::Null(true);
}

struct BufferResource {
    u32 sharp_idx{};
    SharpFetch<AmdGpu::Buffer> sharp_fetch{};
    IR::Type used_types;
    AmdGpu::Buffer inline_cbuf;
    BufferType buffer_type;
    u8 instance_attrib{};
    bool is_written{};
    bool is_formatted{};
    SharpFetchPostOp post_op{};
    u32 post_op_dw1_mask{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer{};
        if (inline_cbuf) {
            buffer = inline_cbuf;
            if (inline_cbuf.base_address != 1) {
                buffer.base_address += info.pgm_base; // address fixup
            }
        } else if (sharp_fetch.UsesFetch()) {
            buffer = sharp_fetch.Fetch(info);
        } else {
            buffer = info.template ReadUdSharp<AmdGpu::Buffer>(sharp_idx);
        }
        if (post_op == SharpFetchPostOp::OffsetByProgramBase) {
            buffer.base_address += info.pgm_base;
        } else if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
            auto* dwords = reinterpret_cast<u32*>(&buffer);
            dwords[1] |= post_op_dw1_mask;
        }
        if (!buffer.Valid()) [[unlikely]] {
            return RejectBufferSharp();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

enum class MipStorageFallbackMode : u32 { None, DynamicIndex, ConstantIndex };

struct ImageResource {
    u32 sharp_idx{};
    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    MipStorageFallbackMode mip_fallback_mode{};
    u32 constant_mip_index{};
    SharpFetchPostOp post_op{};

    AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (sharp_fetch.UsesFetch()) {
            image = sharp_fetch.Fetch(info);
        } else if (!is_r128) {
            image = info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
        } else {
            const auto raw = info.template ReadUdSharp<u128>(sharp_idx);
            std::memcpy(&image, &raw, sizeof(raw));
            image.pitch = image.width;
        }
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray && image.IsCube()) {
            image.type = static_cast<u64>(AmdGpu::ImageType::Color2DArray);
        }
        if (!image.Valid()) [[unlikely]] {
            image = RejectImageSharp(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) [[unlikely]] {
                image = RejectDepthImageSharp();
            }
        }
        return image;
    }

    u32 NumBindings(const AmdGpu::Image& tsharp) const {
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }

    u32 NumBindings(const auto& info) const {
        return NumBindings(GetSharp(info));
    }
};
using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    u32 sharp_idx{};
    SharpFetch<AmdGpu::Sampler> sharp_fetch{};
    AmdGpu::Sampler inline_sampler;
    u32 is_inline_sampler : 1;
    u32 associated_image : 4;
    u32 disable_aniso : 1;
    SharpFetchPostOp post_op{};
    SharpLocation post_op_tsharp_dw3_off{UNKNOWN_LOCATION};
    bool is_depth{};

    AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        AmdGpu::Sampler sampler = is_inline_sampler ? inline_sampler
            : (sharp_fetch.UsesFetch() ? sharp_fetch.Fetch(info)
                                       : info.template ReadUdSharp<AmdGpu::Sampler>(sharp_idx));
        if (disable_aniso || post_op == SharpFetchPostOp::DisableAnisoIfSingleLod) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
        }
        if (post_op == SharpFetchPostOp::ForceRepeatXyzClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ForceLastTexelXyClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
        } else if (post_op == SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            sampler.aniso_threshold.Assign(0);
        }
        return sampler;
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    u32 sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_USER_DATA_REGS / 4;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");

} // namespace Shader
