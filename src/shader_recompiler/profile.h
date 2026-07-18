// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader {

struct Profile {
    u64 max_ubo_size{};
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_float16{};
    bool support_float64{};
    bool supports_denorm_behavior_independence{};
    bool supports_rounding_mode_independence{};
    bool support_fp16_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp16_round_to_zero{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_fp64_denorm_preserve{};
    bool support_fp64_denorm_flush{};
    bool support_fp64_round_to_zero{};
    bool support_fp16_signed_zero_inf_nan_preserve{};
    bool support_fp32_signed_zero_inf_nan_preserve{};
    bool support_fp64_signed_zero_inf_nan_preserve{};
    bool supports_image_load_store_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool has_incomplete_fragment_shader_barycentric{};
    bool has_broken_spirv_clamp{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};

    bool operator!=(const Profile& other) const {
        return max_ubo_size != other.max_ubo_size ||
               max_viewport_width != other.max_viewport_width ||
               max_viewport_height != other.max_viewport_height ||
               max_shared_memory_size != other.max_shared_memory_size ||
               supported_spirv != other.supported_spirv || subgroup_size != other.subgroup_size ||
               support_int8 != other.support_int8 || support_int16 != other.support_int16 ||
               support_int64 != other.support_int64 || support_float16 != other.support_float16 ||
               support_float64 != other.support_float64 ||
               supports_denorm_behavior_independence !=
                   other.supports_denorm_behavior_independence ||
               supports_rounding_mode_independence != other.supports_rounding_mode_independence ||
               support_fp16_denorm_preserve != other.support_fp16_denorm_preserve ||
               support_fp16_denorm_flush != other.support_fp16_denorm_flush ||
               support_fp16_round_to_zero != other.support_fp16_round_to_zero ||
               support_fp32_denorm_preserve != other.support_fp32_denorm_preserve ||
               support_fp32_denorm_flush != other.support_fp32_denorm_flush ||
               support_fp32_round_to_zero != other.support_fp32_round_to_zero ||
               support_fp64_denorm_preserve != other.support_fp64_denorm_preserve ||
               support_fp64_denorm_flush != other.support_fp64_denorm_flush ||
               support_fp64_round_to_zero != other.support_fp64_round_to_zero ||
               support_fp16_signed_zero_inf_nan_preserve !=
                   other.support_fp16_signed_zero_inf_nan_preserve ||
               support_fp32_signed_zero_inf_nan_preserve !=
                   other.support_fp32_signed_zero_inf_nan_preserve ||
               support_fp64_signed_zero_inf_nan_preserve !=
                   other.support_fp64_signed_zero_inf_nan_preserve ||
               supports_image_load_store_lod != other.supports_image_load_store_lod ||
               supports_native_cube_calc != other.supports_native_cube_calc ||
               supports_trinary_minmax != other.supports_trinary_minmax ||
               supports_buffer_fp32_atomic_min_max != other.supports_buffer_fp32_atomic_min_max ||
               supports_image_fp32_atomic_min_max != other.supports_image_fp32_atomic_min_max ||
               supports_buffer_int64_atomics != other.supports_buffer_int64_atomics ||
               supports_shared_int64_atomics != other.supports_shared_int64_atomics ||
               supports_workgroup_explicit_memory_layout !=
                   other.supports_workgroup_explicit_memory_layout ||
               supports_amd_shader_explicit_vertex_parameter !=
                   other.supports_amd_shader_explicit_vertex_parameter ||
               supports_fragment_shader_barycentric != other.supports_fragment_shader_barycentric ||
               has_incomplete_fragment_shader_barycentric !=
                   other.has_incomplete_fragment_shader_barycentric ||
               has_broken_spirv_clamp != other.has_broken_spirv_clamp ||
               lower_left_origin_mode != other.lower_left_origin_mode ||
               needs_manual_interpolation != other.needs_manual_interpolation ||
               needs_lds_barriers != other.needs_lds_barriers ||
               needs_buffer_offsets != other.needs_buffer_offsets ||
               needs_unorm_fixup != other.needs_unorm_fixup ||
               needs_clip_distance_emulation != other.needs_clip_distance_emulation;
    }
    bool supports_shader_stencil_export{};

    bool operator==(const Profile&) const = default;
};

} // namespace Shader
