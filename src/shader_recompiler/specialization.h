// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <cstring>
#include <type_traits>

#include <xxhash.h>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_storage : 1;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_storage == other.is_storage &&
               is_formatted == other.is_formatted && swizzle_enable == other.swizzle_enable &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_cube = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // PORT(upstream #4075): IMAGE_STORE_MIP fallback binds N descriptors
    // for N mip levels; baked into the spec so fallback vs non-fallback
    // shaders land on distinct pipeline cache entries.
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

static_assert(std::is_trivially_copyable_v<VsAttribSpecialization>);
static_assert(std::is_trivially_copyable_v<BufferSpecialization>);
static_assert(std::is_trivially_copyable_v<ImageSpecialization>);
static_assert(std::is_trivially_copyable_v<FMaskSpecialization>);
static_assert(std::is_trivially_copyable_v<SamplerSpecialization>);

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    std::array<u64, 2> bitwords{};
    std::optional<Gcn::FetchShaderData> fetch_shader_data{};
    boost::container::small_vector<VsAttribSpecialization, 64> vs_attribs;
    boost::container::small_vector<BufferSpecialization, 32> buffers;
    boost::container::small_vector<ImageSpecialization, 32> images;
    boost::container::small_vector<FMaskSpecialization, 16> fmasks;
    boost::container::small_vector<SamplerSpecialization, 32> samplers;
    Backend::Bindings start{};
    u64 sig{};
    u64 sig2{};

    StageSpecialization() = default;

    // 5th param accepted for codegodplus ABI compat but IGNORED.
    StageSpecialization(const Info& info_, const RuntimeInfo& runtime_info_, const Profile& profile_,
                        Backend::Bindings start_,
                        const std::optional<Gcn::FetchShaderData>* /*preparsed_fetch*/ = nullptr)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        // NOGLITCH: unconditional parse for ALL stages.
        fetch_shader_data = Gcn::ParseFetchShader(info_);
        if (info_.stage == Stage::Vertex && fetch_shader_data) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data->attributes,
                         [&profile_, this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
                             if (const auto step_rate = desc.GetStepRate();
                                 step_rate != InstanceIdType::None) {
                                 spec.divisor = step_rate == InstanceIdType::OverStepRate0
                                                    ? runtime_info.vs_info.step_rate_0
                                                    : (step_rate == InstanceIdType::OverStepRate1
                                                           ? runtime_info.vs_info.step_rate_1
                                                           : 1);
                             }
                             spec.num_class = profile_.support_legacy_vertex_attributes
                                                  ? AmdGpu::NumberClass{}
                                                  : AmdGpu::GetNumberClass(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         spec.stride = sharp.GetStride();
                         spec.is_storage = desc.IsStorage(sharp);
                         spec.is_formatted = desc.is_formatted;
                         spec.swizzle_enable = sharp.swizzle_enable;
                         if (spec.is_formatted) {
                             spec.data_format = static_cast<u32>(sharp.GetDataFmt());
                             spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                             spec.num_conversion = sharp.GetNumberConversion();
                         }
                         if (spec.swizzle_enable) {
                             spec.index_stride = sharp.index_stride;
                             spec.element_size = sharp.element_size;
                         }
                     });
        ForEachSharp(binding, images, info->images,
                     [&](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.type = sharp.GetViewType(desc.is_array);
                         spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
                         spec.is_storage = desc.is_written;
                         spec.is_cube = sharp.IsCube();
                         if (spec.is_storage) {
                             spec.dst_select = sharp.DstSelect();
                         } else {
                             spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
                         }
                         spec.num_conversion = sharp.GetNumberConversion();
                         // PORT(upstream #4075): consecutive descriptor count
                         // for mip fallback (1 otherwise).
                         spec.num_bindings = desc.NumBindings(*info);
                     });
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.width = sharp.width;
                         spec.height = sharp.height;
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         spec.force_unnormalized = sharp.force_unnormalized;
                         spec.force_degamma = sharp.force_degamma;
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->l_stage == LogicalStage::TessellationControl ||
            info->l_stage == LogicalStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            if (info->l_stage == LogicalStage::TessellationControl) {
                runtime_info.hs_info.InitFromTessConstants(tess_constants);
            } else {
                runtime_info.vs_info.InitFromTessConstants(tess_constants);
            }
        }
        ComputeSig();
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                continue;
            }
            const u32 b = binding++;
            bitset.set(b);
            bitwords[b >> 6] |= (1ULL << (b & 63));
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    void ComputeSig() noexcept {
        u64 h1 = 1469598103934665603ULL;
        u64 h2 = 0x84222325cbf29ce4ULL;
        auto step = [&](u64 v) noexcept {
            h1 ^= v;
            h1 *= 1099511628211ULL;
            h2 ^= v + 0x9e3779b97f4a7c15ULL + (h2 << 6) + (h2 >> 2);
        };
        auto mix_pod_bulk = [&](const void* p, size_t n) noexcept {
            if (n == 0) return;
            step(XXH3_64bits(p, n));
        };
        auto mix_pod_vec_fast = [&](const auto& vec) noexcept {
            using T = typename std::decay_t<decltype(vec)>::value_type;
            static_assert(std::is_trivially_copyable_v<T>);
            step(static_cast<u64>(vec.size()));
            if (!vec.empty()) {
                mix_pod_bulk(vec.data(), vec.size() * sizeof(T));
            }
        };
        step(static_cast<u64>(info ? info->pgm_hash : 0));
        step(static_cast<u64>(info ? static_cast<u32>(info->stage) : 0));
        step(static_cast<u64>(info ? static_cast<u32>(info->l_stage) : 0));
        mix_pod_bulk(&runtime_info, sizeof(runtime_info));
        mix_pod_bulk(&start, sizeof(start));
        step(bitwords[0]);
        step(bitwords[1]);
        step(fetch_shader_data.has_value() ? 1ULL : 0ULL);
        if (fetch_shader_data) {
            u64 fs_packed =
                static_cast<u64>(fetch_shader_data->attributes.size()) |
                (static_cast<u64>(static_cast<u8>(fetch_shader_data->vertex_offset_sgpr)) << 16) |
                (static_cast<u64>(static_cast<u8>(fetch_shader_data->instance_offset_sgpr)) << 24);
            step(fs_packed);
            for (const auto& a : fetch_shader_data->attributes) {
                u64 w = 0;
                w |= static_cast<u64>(a.semantic) << 0;
                w |= static_cast<u64>(a.dest_vgpr) << 8;
                w |= static_cast<u64>(a.num_elements) << 16;
                w |= static_cast<u64>(a.sgpr_base) << 24;
                w |= static_cast<u64>(a.dword_offset) << 32;
                w |= static_cast<u64>(a.instance_data) << 40;
                w |= static_cast<u64>(a.inst_offset) << 48;
                step(w);
                step(static_cast<u64>(a.data_format) | (static_cast<u64>(a.num_format) << 8));
            }
        }
        mix_pod_vec_fast(vs_attribs);
        mix_pod_vec_fast(buffers);
        mix_pod_vec_fast(images);
        mix_pod_vec_fast(fmasks);
        mix_pod_vec_fast(samplers);
        sig = h1;
        sig2 = h2;
    }

    // NOGLITCH: full field comparison, NO sig fast-reject.
    bool operator==(const StageSpecialization& other) const {
        if (!Valid()) {
            return false;
        }

        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        if (bitset.none() && other.bitset.none()) {
            return true;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
