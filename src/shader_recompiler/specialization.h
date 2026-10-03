// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <bitset>
#include <cstddef>
#include <cstring>
#include <type_traits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Shader {

#if defined(_MSC_VER)
#define SHAD_SPECIALIZATION_FORCE_INLINE __forceinline
#else
#define SHAD_SPECIALIZATION_FORCE_INLINE __attribute__((always_inline)) inline
#endif

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization& other) const noexcept {
        u64 lhs_head;
        u64 rhs_head;
        std::memcpy(&lhs_head, this, sizeof(lhs_head));
        std::memcpy(&rhs_head, &other, sizeof(rhs_head));
        return ((lhs_head ^ rhs_head) |
                (static_cast<u8>(dst_select.a) ^ static_cast<u8>(other.dst_select.a))) == 0;
    }
};
static_assert(sizeof(VsAttribSpecialization) == 12);
static_assert(std::is_trivially_copyable_v<VsAttribSpecialization>);

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    SHAD_SPECIALIZATION_FORCE_INLINE bool operator==(
        const BufferSpecialization& other) const noexcept {
        u32 lhs_bits;
        u32 rhs_bits;
        std::memcpy(&lhs_bits, this, sizeof(lhs_bits));
        std::memcpy(&rhs_bits, &other, sizeof(rhs_bits));
        constexpr u32 BaseMask = (1U << 16) - 1;
        constexpr u32 FormattedMask = ((1U << 10) - 1) << 16;
        constexpr u32 SwizzleMask = ((1U << 4) - 1) << 26;
        const u32 delta = lhs_bits ^ rhs_bits;
        if ((delta & BaseMask) != 0) {
            return false;
        }
        if (is_formatted) {
            if ((delta & FormattedMask) != 0) {
                return false;
            }
            u64 lhs_format;
            u64 rhs_format;
            std::memcpy(&lhs_format, &dst_select, sizeof(lhs_format));
            std::memcpy(&rhs_format, &other.dst_select, sizeof(rhs_format));
            if (lhs_format != rhs_format) {
                return false;
            }
        }
        return !swizzle_enable || (delta & SwizzleMask) == 0;
    }
};
static_assert(sizeof(BufferSpecialization) == 12);
static_assert(std::is_trivially_copyable_v<BufferSpecialization>);

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_cube = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization& other) const noexcept {
        return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
};
static_assert(sizeof(ImageSpecialization) == 24);
static_assert(offsetof(ImageSpecialization, type) == 0);
static_assert(offsetof(ImageSpecialization, is_integer) == 8);
static_assert(offsetof(ImageSpecialization, dst_select) == 12);
static_assert(offsetof(ImageSpecialization, num_conversion) == 16);
static_assert(offsetof(ImageSpecialization, num_bindings) == 20);
static_assert(std::is_trivially_copyable_v<ImageSpecialization>);

#if defined(__AVX2__)
template <size_t Bytes>
SHAD_SPECIALIZATION_FORCE_INLINE bool EqualAvx2Block(const u8* lhs, const u8* rhs) noexcept {
    static_assert(Bytes >= 32 && Bytes % 32 == 0);
    __m256i diff = _mm256_xor_si256(
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs)),
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs)));
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (size_t offset = 32; offset < Bytes; offset += 32) {
        const __m256i next = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + offset)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + offset)));
        diff = _mm256_or_si256(diff, next);
    }
    return _mm256_testz_si256(diff, diff) != 0;
}
#endif

SHAD_SPECIALIZATION_FORCE_INLINE bool EqualImageSpecializations(
    const ImageSpecialization* lhs, const ImageSpecialization* rhs, size_t count) noexcept {
    size_t remaining = count * sizeof(ImageSpecialization);
#if defined(__AVX2__)
    const auto* lhs_bytes = reinterpret_cast<const u8*>(lhs);
    const auto* rhs_bytes = reinterpret_cast<const u8*>(rhs);
    while (remaining >= 256) {
        if (!EqualAvx2Block<256>(lhs_bytes, rhs_bytes)) {
            return false;
        }
        lhs_bytes += 256;
        rhs_bytes += 256;
        remaining -= 256;
    }
    if ((remaining & 128) != 0) {
        if (!EqualAvx2Block<128>(lhs_bytes, rhs_bytes)) {
            return false;
        }
        lhs_bytes += 128;
        rhs_bytes += 128;
    }
    if ((remaining & 64) != 0) {
        if (!EqualAvx2Block<64>(lhs_bytes, rhs_bytes)) {
            return false;
        }
        lhs_bytes += 64;
        rhs_bytes += 64;
    }
    if ((remaining & 32) != 0) {
        if (!EqualAvx2Block<32>(lhs_bytes, rhs_bytes)) {
            return false;
        }
        lhs_bytes += 32;
        rhs_bytes += 32;
    }
    if ((remaining & 16) != 0) {
        const __m128i diff = _mm_xor_si128(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs_bytes)),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs_bytes)));
        if (!_mm_testz_si128(diff, diff)) {
            return false;
        }
        lhs_bytes += 16;
        rhs_bytes += 16;
    }
    if ((remaining & 8) != 0) {
        u64 lhs_tail;
        u64 rhs_tail;
        std::memcpy(&lhs_tail, lhs_bytes, sizeof(lhs_tail));
        std::memcpy(&rhs_tail, rhs_bytes, sizeof(rhs_tail));
        if (lhs_tail != rhs_tail) {
            return false;
        }
    }
    return true;
#else
    return std::memcmp(lhs, rhs, remaining) == 0;
#endif
}

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization& other) const noexcept {
        return std::bit_cast<u64>(*this) == std::bit_cast<u64>(other);
    }
};
static_assert(sizeof(FMaskSpecialization) == sizeof(u64));
static_assert(std::is_trivially_copyable_v<FMaskSpecialization>);

SHAD_SPECIALIZATION_FORCE_INLINE bool EqualFMaskSpecializations(
    const FMaskSpecialization* lhs, const FMaskSpecialization* rhs, size_t count) noexcept {
#if defined(__AVX2__)
    while (count >= 4) {
        const __m256i diff = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs)));
        if (!_mm256_testz_si256(diff, diff)) {
            return false;
        }
        lhs += 4;
        rhs += 4;
        count -= 4;
    }
    if ((count & 2) != 0) {
        const __m128i diff = _mm_xor_si128(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs)),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs)));
        if (!_mm_testz_si128(diff, diff)) {
            return false;
        }
        lhs += 2;
        rhs += 2;
    }
    return (count & 1) == 0 || std::bit_cast<u64>(*lhs) == std::bit_cast<u64>(*rhs);
#else
    return std::memcmp(lhs, rhs, count * sizeof(FMaskSpecialization)) == 0;
#endif
}

#undef SHAD_SPECIALIZATION_FORCE_INLINE

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

[[nodiscard]] inline VsAttribSpecialization MakeVsAttribSpecialization(
    const Gcn::VertexAttribute& desc, AmdGpu::Buffer sharp,
    const RuntimeInfo& runtime_info) noexcept {
    VsAttribSpecialization spec{};
    using InstanceIdType = Gcn::VertexAttribute::InstanceIdType;
    if (const auto step_rate = desc.GetStepRate(); step_rate != InstanceIdType::None) {
        spec.divisor = step_rate == InstanceIdType::OverStepRate0
                           ? runtime_info.vs_info.step_rate_0
                           : (step_rate == InstanceIdType::OverStepRate1
                                  ? runtime_info.vs_info.step_rate_1
                                  : 1);
    }
    spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
    spec.dst_select = sharp.DstSelect();
    return spec;
}

[[nodiscard]] inline BufferSpecialization MakeBufferSpecialization(
    const BufferResource& desc, AmdGpu::Buffer sharp) noexcept {
    BufferSpecialization spec{};
    spec.stride = sharp.GetStride();
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
    return spec;
}

[[nodiscard]] inline ImageSpecialization MakeImageSpecialization(const ImageResource& desc,
                                                                  AmdGpu::Image sharp) noexcept {
    ImageSpecialization spec{};
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
    spec.num_bindings = desc.NumBindings(sharp);
    return spec;
}

[[nodiscard]] inline FMaskSpecialization MakeFMaskSpecialization(AmdGpu::Image sharp) noexcept {
    return {.width = static_cast<u32>(sharp.width), .height = static_cast<u32>(sharp.height)};
}

[[nodiscard]] inline SamplerSpecialization MakeSamplerSpecialization(
    AmdGpu::Sampler sharp) noexcept {
    return {
        .force_unnormalized = static_cast<u8>(sharp.force_unnormalized),
        .force_degamma = static_cast<u8>(sharp.force_degamma),
    };
}

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
    std::optional<Gcn::FetchShaderData> fetch_shader_data{};
    boost::container::small_vector<VsAttribSpecialization, 32> vs_attribs;
    boost::container::small_vector<BufferSpecialization, 16> buffers;
    boost::container::small_vector<ImageSpecialization, 16> images;
    boost::container::small_vector<FMaskSpecialization, 8> fmasks;
    boost::container::small_vector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    StageSpecialization(const Info& info_, RuntimeInfo runtime_info_, const Profile& profile_,
                        Backend::Bindings start_,
                        const std::optional<Gcn::FetchShaderData>* fetch_shader_hint = nullptr)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        fetch_shader_data = fetch_shader_hint ? *fetch_shader_hint : Gcn::ParseFetchShader(info_);
        if (info_.stage == Stage::Vertex && fetch_shader_data) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data->attributes, info_.resolved_vertex_buffers,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             spec = MakeVsAttribSpecialization(desc, sharp, runtime_info);
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers, info->resolved_buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         spec = MakeBufferSpecialization(desc, sharp);
                     });
        ForEachSharp(binding, images, info->images, info->resolved_images,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec = MakeImageSpecialization(desc, sharp);
                     });
        ForEachSharp(binding, fmasks, info->fmasks, info->resolved_fmasks,
                     [](auto& spec, const auto&, AmdGpu::Image sharp) {
                         spec = MakeFMaskSpecialization(sharp);
                     });
        ForEachSharp(samplers, info->samplers, info->resolved_samplers,
                     [](auto& spec, const auto&, AmdGpu::Sampler sharp) {
                         spec = MakeSamplerSpecialization(sharp);
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, const auto& resolved_sharps, auto&& func) {
        const bool use_resolved = resolved_sharps.size() == desc_list.size();
        for (u32 index = 0; index < desc_list.size(); ++index) {
            const auto& desc = desc_list[index];
            auto& spec = spec_list.emplace_back();
            const auto sharp = use_resolved ? resolved_sharps[index] : desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, const auto& resolved_sharps,
                      auto&& func) {
        const bool use_resolved = resolved_sharps.size() == desc_list.size();
        for (u32 index = 0; index < desc_list.size(); ++index) {
            const auto& desc = desc_list[index];
            auto& spec = spec_list.emplace_back();
            const auto sharp = use_resolved ? resolved_sharps[index] : desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                continue;
            }
            bitset.set(binding++);
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid() || !other.Valid()) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (vs_attribs.size() != other.vs_attribs.size() ||
            buffers.size() != other.buffers.size() || images.size() != other.images.size() ||
            fmasks.size() != other.fmasks.size() || samplers.size() != other.samplers.size()) {
            return false;
        }

        if (bitset != other.bitset) {
            return false;
        }

        for (size_t index = 0; index < vs_attribs.size(); ++index) {
            if (vs_attribs[index] != other.vs_attribs[index]) {
                return false;
            }
        }

        if (fetch_shader_data.has_value() != other.fetch_shader_data.has_value()) {
            return false;
        }
        if (fetch_shader_data) {
            const auto& lhs_fetch = *fetch_shader_data;
            const auto& rhs_fetch = *other.fetch_shader_data;
            if (lhs_fetch.attributes.size() != rhs_fetch.attributes.size() ||
                lhs_fetch.vertex_offset_sgpr != rhs_fetch.vertex_offset_sgpr ||
                lhs_fetch.instance_offset_sgpr != rhs_fetch.instance_offset_sgpr) {
                return false;
            }
            const auto* lhs_attribute = lhs_fetch.attributes.data();
            const auto* const lhs_attribute_end = lhs_attribute + lhs_fetch.attributes.size();
            const auto* rhs_attribute = rhs_fetch.attributes.data();
            for (; lhs_attribute != lhs_attribute_end; ++lhs_attribute, ++rhs_attribute) {
                if (*lhs_attribute != *rhs_attribute) {
                    return false;
                }
            }
        }

        if (!EqualFMaskSpecializations(fmasks.data(), other.fmasks.data(), fmasks.size())) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings.
        if (bitset.none()) {
            return true;
        }

        u64 lhs_start;
        u64 rhs_start;
        static_assert(sizeof(Backend::Bindings) == 12);
        static_assert(offsetof(Backend::Bindings, buffer) == 4);
        static_assert(offsetof(Backend::Bindings, user_data) == 8);
        std::memcpy(&lhs_start, &start, sizeof(lhs_start));
        std::memcpy(&rhs_start, &other.start, sizeof(rhs_start));
        if (((lhs_start ^ rhs_start) | (start.user_data ^ other.start.user_data)) != 0) {
            return false;
        }

        // Inactive entries are value-initialized by ForEachSharp and preserved by serialization,
        // so the matching bitset makes whole-vector comparison equivalent to skipping them.
        for (size_t index = 0; index < buffers.size(); ++index) {
            if (buffers[index] != other.buffers[index]) {
                return false;
            }
        }
        if (!EqualImageSpecializations(images.data(), other.images.data(), images.size())) {
            return false;
        }
        for (size_t index = 0; index < samplers.size(); ++index) {
            if (samplers[index] != other.samplers[index]) {
                return false;
            }
        }
        return true;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader