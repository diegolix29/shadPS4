// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>
#include <limits>
#include <cstring>
#include <mutex>
#include <vector>
#include <xxhash.h>

#include "common/config.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

// =========================================================================
// OPT(GR2 v78): Pipeline compile graveyard.
// =========================================================================
// std::future<T>'s destructor joins the worker thread. When a Vulkan driver
// hangs inside vkCreateGraphicsPipelines, that thread never returns, and
// any future destruction (PipelineCache dtor, pending-map erase) blocks
// forever. We dump "abandoned" futures here on permafail and on PipelineCache
// teardown; this storage is intentionally never freed so futures never get
// destructed. On process exit the OS reaps the hung threads via _exit.
struct PipelineCompileGraveyard {
    std::mutex mu;
    // Heap-allocated vector we never delete — this is the leak-on-purpose.
    std::vector<std::future<std::unique_ptr<GraphicsPipeline>>>* graves = nullptr;
    void Bury(std::future<std::unique_ptr<GraphicsPipeline>> f) {
        std::lock_guard lk{mu};
        if (!graves) {
            graves = new std::vector<std::future<std::unique_ptr<GraphicsPipeline>>>();
        }
        graves->push_back(std::move(f));
    }
};

PipelineCompileGraveyard& Graveyard() {
    // Leaked Meyers-style singleton — heap allocated + never deleted, so its
    // dtor never runs (which is exactly what we want; see comment above).
    static auto* g = new PipelineCompileGraveyard();
    return *g;
}

} // namespace

using Shader::LogicalStage;
using Shader::Output;
using Shader::Stage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

// PERF(GR2 v16): Hash RuntimeInfo by stage, matching operator== semantics exactly.
// Cannot hash raw bytes because the union has padding and some stages use custom
// equality (e.g. FragmentRuntimeInfo only compares inputs[0..num_inputs]).
static u64 HashRuntimeInfoForStage(const Shader::RuntimeInfo& ri) {
    // Combine helper: boost::hash_combine style
    auto mix = [](u64 seed, u64 v) -> u64 {
        return seed ^ (v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    u64 h = static_cast<u64>(ri.stage);
    switch (ri.stage) {
    case Shader::Stage::Local:
        h = mix(h, ri.ls_info.ls_stride);
        break;
    case Shader::Stage::Export:
        h = mix(h, ri.es_info.vertex_data_size);
        break;
    case Shader::Stage::Vertex: {
        const auto& v = ri.vs_info;
        h = mix(h, v.num_outputs);
        h = mix(h, XXH3_64bits(v.outputs.data(), sizeof(v.outputs)));
        h = mix(h, static_cast<u64>(v.tess_emulated_primitive) |
                    (static_cast<u64>(v.emulate_depth_negative_one_to_one) << 1) |
                    (static_cast<u64>(v.clip_disable) << 2));
        h = mix(h, v.step_rate_0);
        h = mix(h, v.step_rate_1);
        h = mix(h, static_cast<u64>(v.tess_type));
        h = mix(h, static_cast<u64>(v.tess_topology));
        h = mix(h, static_cast<u64>(v.tess_partitioning));
        h = mix(h, v.hs_output_cp_stride);
        break;
    }
    case Shader::Stage::Hull:
        // Uses default operator==, so hash all fields.
        h = mix(h, XXH3_64bits(&ri.hs_info, sizeof(ri.hs_info)));
        break;
    case Shader::Stage::Geometry: {
        const auto& g = ri.gs_info;
        h = mix(h, g.num_outputs);
        h = mix(h, XXH3_64bits(g.outputs.data(), sizeof(g.outputs)));
        h = mix(h, g.num_invocations);
        h = mix(h, g.output_vertices);
        h = mix(h, static_cast<u64>(g.in_primitive));
        h = mix(h, XXH3_64bits(g.out_primitive.data(), sizeof(g.out_primitive)));
        h = mix(h, g.vs_copy_hash); // Not the span pointer!
        break;
    }
    case Shader::Stage::Fragment: {
        const auto& f = ri.fs_info;
        h = mix(h, XXH3_64bits(f.color_buffers.data(), sizeof(f.color_buffers)));
        h = mix(h, XXH3_64bits(&f.en_flags, sizeof(f.en_flags)));
        h = mix(h, XXH3_64bits(&f.addr_flags, sizeof(f.addr_flags)));
        h = mix(h, f.num_inputs);
        h = mix(h, static_cast<u64>(f.z_export_format));
        h = mix(h, static_cast<u64>(f.mrtz_mask) |
                    (static_cast<u64>(f.dual_source_blending) << 8));
        if (f.num_inputs > 0) {
            h = mix(h, XXH3_64bits(f.inputs.data(),
                                    f.num_inputs * sizeof(f.inputs[0])));
        }
        break;
    }
    case Shader::Stage::Compute: {
        const auto& c = ri.cs_info;
        h = mix(h, XXH3_64bits(c.workgroup_size.data(), sizeof(c.workgroup_size)));
        h = mix(h, static_cast<u64>(c.tgid_enable[0]) |
                    (static_cast<u64>(c.tgid_enable[1]) << 1) |
                    (static_cast<u64>(c.tgid_enable[2]) << 2));
        break;
    }
    default:
        break;
    }
    return h;
}

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(Stage stage, LogicalStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) {
        info.num_user_data = program.settings.num_user_regs;
        info.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.num_allocated_vgprs = program.NumVgprs();
        info.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.fp_round_mode32 = program.settings.fp_round_mode32;
    };
    info.Initialize(stage);
    switch (stage) {
    case Stage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants;
        const auto* hull_info = infos[u32(Shader::LogicalStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.ls_info.ls_stride = tess_constants.ls_stride;
        break;
    }
    case Stage::Hull: {
        BuildCommon(regs.hs_program);
        info.hs_info.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.hs_info.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.hs_info.tess_type = regs.tess_config.type;
        info.hs_info.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;

        // We need to initialize most hs_info fields after finding the V# with tess constants
        break;
    }
    case Stage::Export: {
        BuildCommon(regs.es_program);
        if (l_stage == LogicalStage::TessellationEval) {
            // Combined LS+HS+ES+GS pipeline: ES acts as domain shader.
            info.vs_info.num_outputs = regs.vgt_esgs_ring_itemsize;
            info.vs_info.tess_type = regs.tess_config.type;
            info.vs_info.tess_topology = regs.tess_config.topology;
            info.vs_info.tess_partitioning = regs.tess_config.partitioning;
        } else {
            info.es_info.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        }
        break;
    }
    case Stage::Vertex: {
        BuildCommon(regs.vs_program);
        info.vs_info.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.vs_info.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.vs_info.num_outputs = MapOutputs(info.vs_info.outputs, regs.vs_output_control);
        info.vs_info.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.vs_info.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        info.vs_info.clip_disable = regs.IsClipDisabled();
        if (l_stage == LogicalStage::TessellationEval) {
            info.vs_info.tess_type = regs.tess_config.type;
            info.vs_info.tess_topology = regs.tess_config.topology;
            info.vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Geometry: {
        BuildCommon(regs.gs_program);
        auto& gs_info = info.gs_info;
        gs_info.num_outputs = MapOutputs(gs_info.outputs, regs.vs_output_control);
        gs_info.output_vertices = regs.vgt_gs_max_vert_out;
        gs_info.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        gs_info.in_primitive = regs.primitive_type;
        // In combined tess+GS pipelines, primitive_type is PatchPrimitive which isn't
        // meaningful for GS input. Resolve to the actual post-tessellation output type.
        if (gs_info.in_primitive == AmdGpu::PrimitiveType::PatchPrimitive) {
            switch (regs.tess_config.type) {
            case AmdGpu::TessellationType::Isoline:
                gs_info.in_primitive = AmdGpu::PrimitiveType::LineList;
                break;
            case AmdGpu::TessellationType::Triangle:
            case AmdGpu::TessellationType::Quad:
                gs_info.in_primitive = AmdGpu::PrimitiveType::TriangleList;
                break;
            }
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            gs_info.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        gs_info.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        gs_info.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        gs_info.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        gs_info.vs_copy = params_vc.code;
        gs_info.vs_copy_hash = params_vc.hash;
        DumpShader(gs_info.vs_copy, gs_info.vs_copy_hash, Shader::Stage::Vertex, 0, "copy.bin");
        break;
    }
    case Stage::Fragment: {
        BuildCommon(regs.ps_program);
        info.fs_info.en_flags = regs.ps_input_ena;
        info.fs_info.addr_flags = regs.ps_input_addr;
        info.fs_info.num_inputs = regs.num_interp;
        info.fs_info.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.fs_info.mrtz_mask = regs.depth_shader_control.z_export_enable |
                                 (stencil_ref_export_enable << 1) |
                                 (regs.depth_shader_control.mask_export_enable << 2) |
                                 (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.fs_info.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.fs_info.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.fs_info.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.fs_info.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.fs_info.color_buffers[i] = graphics_key.color_buffers[i];
        }
        break;
    }
    case Stage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.num_user_data = cs_pgm.settings.num_user_regs;
        info.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.cs_info.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
        info.cs_info.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
        info.cs_info.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetMasterSemaphore(), DescriptorHeapSizes} {
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        // When binding a UBO, we calculate its size considering the offset in the larger buffer
        // cache underlying resource. In some cases, it may produce sizes exceeding the system
        // maximum allowed UBO range, so we need to reduce the threshold to prevent issues.
        .max_ubo_size = instance.UniformMaxSize() - instance.UniformMinAlignment(),
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_legacy_vertex_attributes = instance_.IsLegacyVertexAttributesSupported(),
        // PORT(upstream #4075, IMAGE_STORE_MIP fallback, commit — merged Mar 17 2026):
        // upstream hardcodes this to false with a // TEST marker. The fallback path
        // is preferred on both AMD and NVIDIA for GR2 per compat issue #1429
        // (mipmap-only-level-0 bug affects both vendors). Small perf cost on AMD
        // where native load-store-lod works — each IMAGE_STORE_MIP image uses
        // N descriptor slots instead of 1.
        .supports_image_load_store_lod = /*instance_.IsImageLoadStoreLodSupported()*/ false, // TEST
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        // TODO: Emitted bounds checks cause problems with phi control flow; needs to be fixed.
        .supports_robust_buffer_access = true, // instance_.IsRobustBufferAccess2Supported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .has_incomplete_fragment_shader_barycentric =
            instance_.IsFragmentShaderBarycentricSupported() &&
            instance.GetDriverID() == vk::DriverId::eMoltenvk,
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMoltenvk,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMoltenvk,
    };

    WarmUp();

    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
}

PipelineCache::~PipelineCache() {
    // OPT(GR2 v78): Dump any still-in-flight async compiles to the graveyard.
    // The map's own destructor would destruct each std::future, which joins
    // its worker thread — and if any of those threads are hung inside the
    // Vulkan driver, the emulator would freeze on its way out. Move futures
    // out first so map destruction sees already-empty PendingGraphicsPipeline
    // objects.
    for (auto& [key, pending] : pending_graphics_pipelines) {
        if (pending && pending->future.valid()) {
            Graveyard().Bury(std::move(pending->future));
        }
    }
    pending_graphics_pipelines.clear();
}


const GraphicsPipeline* PipelineCache::GetGraphicsPipeline() {
    const u64 stamp = liverpool->GetGfxPipelineStamp();
    if (stamp == last_gfx_stamp && last_gfx_pipeline) {
        return last_gfx_pipeline;
    }
    // Level 2: stamp bumped but only dynamic-state regs changed (viewport, scissor, etc.)
    // Skip the entire RefreshGraphicsKey rebuild — pipeline key cannot have changed.
    if (!liverpool->IsGfxKeyDirty() && last_gfx_pipeline) {
        last_gfx_stamp = stamp;
        return last_gfx_pipeline;
    }
    liverpool->ClearGfxKeyDirty();
    if (!RefreshGraphicsKey()) {
        return nullptr;
    }
    // Key-level dedup: when only dynamic state changed (viewport, scissor, blend constants),
    // the stamp bumps but the pipeline key is byte-identical. Skip the hash + map lookup.
    if (last_gfx_pipeline &&
        std::memcmp(&graphics_key, &prev_graphics_key_,
                    offsetof(GraphicsPipelineKey, cached_hash_)) == 0) {
        last_gfx_stamp = stamp;
        return last_gfx_pipeline;
    }

    // =========================================================================
    // OPT(GR2 v78): Pending-async check first.
    // =========================================================================
    // If a previous call for this key launched an async compile that didn't
    // finish within the sync budget, the future lives in pending_graphics_pipelines.
    // Poll non-blockingly; finalize into graphics_pipelines on ready.
    if (auto pit = pending_graphics_pipelines.find(graphics_key);
        pit != pending_graphics_pipelines.end()) {
        if (TryFinalizePending(*pit->second, graphics_key)) {
            // Result moved into graphics_pipelines[graphics_key]. Erase pending.
            pending_graphics_pipelines.erase(pit);
            // Fall through to the main-path update below.
        } else {
            // Still compiling (or permafailed). Skip this draw.
            return nullptr;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);

        auto pending = LaunchAsyncPipelineCompile(graphics_key, pipeline_hash);

        // Synchronous fast-path wait. Most compiles complete in <50ms; waiting
        // kInitialSyncBudget catches them without triggering frame-skip.
        if (pending->future.wait_for(kInitialSyncBudget) == std::future_status::ready) {
            std::unique_ptr<GraphicsPipeline> pipeline;
            try {
                pipeline = pending->future.get();
            } catch (const std::exception& e) {
                LOG_ERROR(Render_Vulkan, "Async pipeline compile threw: {}", e.what());
            }
            if (!pipeline) {
                // Compile failed. Drop the empty map slot so we can retry next tick.
                graphics_pipelines.erase(it);
                return nullptr;
            }
            // Move result into the cache slot.
            it.value() = std::move(pipeline);
            // Finalize side effects — these need post-ctor state (sdata, modules).
            RegisterPipelineData(graphics_key, pipeline_hash, pending->sdata);
            ++num_new_pipelines;
            if (Config::collectShadersForDebug()) {
                for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                    if (pending->infos_copy[stage]) {
                        auto& m = pending->modules_copy[stage];
                        module_related_pipelines[m].emplace_back(graphics_key);
                    }
                }
            }
            fetch_shader.reset();
        } else {
            // Slow path: compile hasn't finished. Stash the pending entry, leave
            // graphics_pipelines[key] as null (indicates "in-flight"), return
            // null so the Rasterizer skips this draw. Subsequent draws for the
            // same key hit the TryFinalizePending branch above.
            pending_graphics_pipelines.emplace(graphics_key, std::move(pending));
            fetch_shader.reset();
            return nullptr;
        }
    } else if (!it->second) {
        // Defensive: is_new=false but slot is null. This shouldn't happen if
        // invariants hold (TryFinalizePending always writes non-null on success,
        // and we already checked pending above). Treat as "still compiling."
        return nullptr;
    }

    last_gfx_stamp = stamp;
    last_gfx_pipeline = it->second.get();
    std::memcpy(&prev_graphics_key_, &graphics_key, sizeof(GraphicsPipelineKey));
    return last_gfx_pipeline;
}

bool PipelineCache::TryFinalizePending(PendingGraphicsPipeline& pending,
                                       const GraphicsPipelineKey& key) {
    if (pending.permafailed) {
        return false;
    }
    if (pending.future.wait_for(std::chrono::milliseconds{0}) !=
        std::future_status::ready) {
        // Still compiling. Check thresholds for escalation.
        const auto elapsed = std::chrono::steady_clock::now() - pending.started_at;
        if (elapsed >= kPermaFailThreshold) {
            LOG_CRITICAL(Render_Vulkan,
                         "Pipeline {:#x} stuck >{}s — permafailed. Moving to graveyard; "
                         "this pipeline's draws will be skipped for the rest of the session. "
                         "This is almost certainly a Vulkan driver hang "
                         "(Mesa/RADV). Try updating your GPU driver.",
                         pending.pipeline_hash,
                         std::chrono::duration_cast<std::chrono::seconds>(kPermaFailThreshold)
                             .count());
            Graveyard().Bury(std::move(pending.future));
            pending.permafailed = true;
        } else if (elapsed >= kHangLogThreshold && !pending.hang_warned) {
            LOG_WARNING(Render_Vulkan,
                        "Pipeline {:#x} compile exceeded {}s — likely driver hang. "
                        "Draws using this pipeline are being skipped. Will permafail at {}s.",
                        pending.pipeline_hash,
                        std::chrono::duration_cast<std::chrono::seconds>(kHangLogThreshold)
                            .count(),
                        std::chrono::duration_cast<std::chrono::seconds>(kPermaFailThreshold)
                            .count());
            pending.hang_warned = true;
        }
        return false;
    }
    // Ready. Collect the result.
    std::unique_ptr<GraphicsPipeline> pipeline;
    try {
        pipeline = pending.future.get();
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Async pipeline {:#x} threw: {}", pending.pipeline_hash,
                  e.what());
    }
    if (!pipeline) {
        // Compile failed. Don't leave a permafail marker — let the outer code
        // retry next tick by erasing the pending entry (caller does this).
        return false;
    }
    // Install into the main map (create slot if missing; it usually exists as null).
    auto [it, is_new] = graphics_pipelines.try_emplace(key);
    it.value() = std::move(pipeline);
    // Finalize side effects.
    RegisterPipelineData(key, pending.pipeline_hash, pending.sdata);
    ++num_new_pipelines;
    if (Config::collectShadersForDebug()) {
        for (auto stage = 0; stage < MaxShaderStages; ++stage) {
            if (pending.infos_copy[stage]) {
                auto& m = pending.modules_copy[stage];
                module_related_pipelines[m].emplace_back(key);
            }
        }
    }
    LOG_INFO(Render_Vulkan, "Pipeline {:#x} compile finished after {} ms",
             pending.pipeline_hash,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - pending.started_at)
                 .count());
    return true;
}

std::unique_ptr<PipelineCache::PendingGraphicsPipeline>
PipelineCache::LaunchAsyncPipelineCompile(const GraphicsPipelineKey& key, u64 pipeline_hash) {
    auto pending = std::make_unique<PendingGraphicsPipeline>();
    pending->started_at = std::chrono::steady_clock::now();
    pending->pipeline_hash = pipeline_hash;

    // Deep-copy stage data. The PipelineCache::infos/runtime_infos/modules
    // members are span-targets and get overwritten by the next RefreshGraphicsStages.
    // The async task must not alias them.
    pending->infos_copy = infos;
    pending->runtime_infos_copy = runtime_infos;
    pending->modules_copy = modules;
    pending->fetch_shader_copy = fetch_shader;

    // Capture by raw pointer into the pending entry. The pending entry lives
    // in the map owned by PipelineCache until finalize/permafail, so pointers
    // into it are stable for the lifetime of the worker's execution (worker
    // result is harvested before erase in TryFinalizePending, or moved to the
    // graveyard where it's never touched again).
    PendingGraphicsPipeline* raw = pending.get();

    // Snapshot fields the ctor needs but that reference PipelineCache members.
    const Instance* instance_ptr = &instance;
    Scheduler* scheduler_ptr = &scheduler;
    DescriptorHeap* desc_heap_ptr = &desc_heap;
    const Shader::Profile* profile_ptr = &profile;
    vk::PipelineCache cache_handle = *pipeline_cache;
    GraphicsPipelineKey key_copy = key;

    pending->future = std::async(
        std::launch::async,
        [raw, instance_ptr, scheduler_ptr, desc_heap_ptr, profile_ptr, cache_handle,
         key_copy]() -> std::unique_ptr<GraphicsPipeline> {
            // Spans over the pending-owned copies — stable for the async task's lifetime.
            std::span<const Shader::Info*, MaxShaderStages> infos_span{raw->infos_copy};
            std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_span{
                raw->runtime_infos_copy};
            std::span<const vk::ShaderModule> modules_span{raw->modules_copy};
            // Note: GraphicsPipeline ctor ASSERTs on driver-return failure, which
            // kills the process. We can't soften that; it's only the hang case
            // (no return at all) that this whole mechanism addresses.
            return std::make_unique<GraphicsPipeline>(
                *instance_ptr, *scheduler_ptr, *desc_heap_ptr, *profile_ptr, key_copy,
                cache_handle, infos_span, runtime_span, raw->fetch_shader_copy, modules_span,
                raw->sdata, false);
        });
    return pending;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (Config::collectShadersForDebug()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](Shader::Stage stage_in, Shader::LogicalStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(infos[stage_out_idx], modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            fetch_shader = fetch_shader_;
        }
        return true;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);
    bind_stage(Stage::Fragment, LogicalStage::Fragment);

    const auto* fs_info = infos[static_cast<u32>(LogicalStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Stream output unsupported, skipping");
            return false;
        }
        if (!bind_stage(Stage::Export, LogicalStage::Vertex)) {
            return false;
        }
        if (!bind_stage(Stage::Geometry, LogicalStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported() ||
            (regs.tess_config.type == AmdGpu::TessellationType::Isoline &&
             !instance.IsTessellationIsolinesSupported())) {
            return false;
        }
        if (!bind_stage(Stage::Hull, LogicalStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(Stage::Vertex, LogicalStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(Stage::Local, LogicalStage::Vertex)) {
            return false;
        }
        break;
    default:
        if (regs.stage_enable.hs_en && regs.stage_enable.gs_en) {
            // Combined LS+HS+ES+GS pipeline (e.g. foliage with tessellation + geometry).
            if (!instance.IsTessellationSupported() || !instance.IsGeometryStageSupported()) {
                LOG_WARNING(Render_Vulkan,
                            "Combined tessellation+geometry pipeline unsupported, skipping");
                return false;
            }
            if (regs.tess_config.type == AmdGpu::TessellationType::Isoline &&
                !instance.IsTessellationIsolinesSupported()) {
                return false;
            }
            if (regs.vgt_strmout_config.raw) {
                LOG_WARNING(Render_Vulkan, "Stream output unsupported, skipping");
                return false;
            }
            if (!bind_stage(Stage::Hull, LogicalStage::TessellationControl)) {
                return false;
            }
            if (!bind_stage(Stage::Export, LogicalStage::TessellationEval)) {
                return false;
            }
            if (!bind_stage(Stage::Local, LogicalStage::Vertex)) {
                return false;
            }
            if (!bind_stage(Stage::Geometry, LogicalStage::Geometry)) {
                return false;
            }
        } else {
            bind_stage(Stage::Vertex, LogicalStage::Vertex);
        }
        break;
    }

    const auto* vs_info = infos[static_cast<u32>(Shader::LogicalStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT(vertex_binding < MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(infos[0], modules[0], fetch_shader, compute_key.value) =
        GetProgram(Shader::Stage::Compute, LogicalStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.stage, perm_idx, "bin");

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
    const bool is_patched = patch && Config::patchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (Config::collectShadersForDebug()) {
        DebugState.CollectShader(name, info.l_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(Stage stage, LogicalStage l_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    Shader::RuntimeInfo runtime_info = BuildRuntimeInfo(stage, l_stage);
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        it_pgm.value() = std::make_unique<Program>(stage, l_stage, params);
        auto& program = it_pgm.value();
        auto start = binding;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        program->AddPermut(module, std::move(spec));
        return std::make_tuple(&program->info, module, program->modules[0].spec.fetch_shader_data,
                               perm_hash);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();

    // PERF(GR2 v16): Fast-path to skip StageSpecialization construction.
    // Hash (user_data, runtime_info by stage, full binding) and compare to cached result.
    // When only context regs change (viewport, scissor, blend) but SH regs stay the same,
    // this avoids constructing StageSpecialization (~2.26% of GpuComm).
    u64 ud_hash;
    u64 ri_bind_hash;
    {
        ud_hash = XXH3_64bits(params.user_data.data(), params.user_data.size_bytes());
        // Mix in stage-aware runtime_info hash (handles union padding + custom operator==)
        ri_bind_hash = HashRuntimeInfoForStage(runtime_info);
        // Mix in ALL binding fields (unified, buffer, user_data — 3x u32 = 12 bytes)
        ri_bind_hash ^= (static_cast<u64>(binding.unified) |
                     (static_cast<u64>(binding.buffer) << 32)) +
                    0x9e3779b97f4a7c15ULL + (ri_bind_hash << 6) + (ri_bind_hash >> 2);
        ri_bind_hash ^= static_cast<u64>(binding.user_data) +
                    0x517cc1b727220a95ULL + (ri_bind_hash << 6) + (ri_bind_hash >> 2);

        ud_hash ^= ri_bind_hash + 0x9e3779b97f4a7c15ULL + (ud_hash << 6) + (ud_hash >> 2);

        if (program->last_result.valid && program->last_result.ud_hash == ud_hash) {
            const auto perm_idx = program->last_result.perm_idx;
            if (perm_idx < program->modules.size()) {
                info.AddBindings(binding);
                return std::make_tuple(&program->info, program->last_result.module,
                                       program->modules[perm_idx].spec.fetch_shader_data,
                                       program->last_result.perm_hash);
            }
        }

        // FIX(GR2FORK): PERF(GR2 v17) "Stable single-permutation shortcut" removed.
        //
        // The removed shortcut assumed that if `ri_bind_hash` (hash of
        // runtime_info + binding offsets) matched the cached value for 64+
        // consecutive calls, the cached shader module remained valid even
        // when `user_data` (the SGPRs) differed — on the rationale that
        // "stride/format/etc. are extremely unlikely to change" once a
        // program has been stable.
        //
        // That assumption is wrong. `user_data` values ARE the guest
        // pointers (or inline encodings) to the image/buffer sharps that
        // StageSpecialization codegens against — see Info::ReadUdReg in
        // shader_recompiler/info.h, which dereferences user_data[i] to
        // reach sharp memory. Different user_data → different sharp
        // address → potentially different image type / dst_select /
        // num_conversion / srgb / storage / array-ness / fetch-shader
        // layout. A module specialized against sharp set A is NOT safe to
        // serve for sharp set B, even if both have the same slot layout.
        //
        // GR2 (CUSA03694) exposes this. The same fragment shader is used
        // both for comic/UI panels (RGBA8_SRGB 2D textures) and for
        // in-world effect draws (different image types / num_conversion).
        // Ditto vertex shaders used for UI quads and for high-velocity
        // character animation. After ~64 UI draws the shortcut locks the
        // cache to the UI spec; subsequent effect/fall draws then read
        // the stale module and produce green garbled effect textures and
        // vertex explosions. The shortcut also rewrote last_result.ud_hash
        // to the new user_data, so Fast-path 1 above would keep firing on
        // the stale module for the remainder of the 512-call revalidate
        // window — self-reinforcing.
        //
        // Fast-path 1 (ud_hash-keyed) above remains correct: ud_hash is
        // XXH3 of the user_data BYTES, so any SGPR change — which is what
        // changes when sharp pointers change — invalidates the hit. We
        // lose the v17 perf win for programs that receive genuinely new
        // user_data every frame but point to structurally identical
        // sharps; that case pays one StageSpecialization construct per
        // call, which is what the sig-based lookup below is optimized for.
        //
        // Store hashes for later cache update (after spec construction + lookup)
        program->last_result.ud_hash = ud_hash;
        program->last_result.ri_bind_hash = ri_bind_hash;
    }

    const std::optional<Shader::Gcn::FetchShaderData>* cached_fetch = nullptr;
    if (stage == Stage::Vertex && !program->modules.empty()) {
        cached_fetch = &program->modules.front().spec.fetch_shader_data;
    }
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding, cached_fetch);

    // Fast path: look up by specialization signature.
    // We use a *pair* of signatures (sig + sig2) so we can avoid expensive deep comparisons.
    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    bool found = false;
    if (const auto it_sig = program->perm_index_by_sig.find(spec.sig);
        it_sig != program->perm_index_by_sig.end() && it_sig->second < program->modules.size()) {
        const auto& ms = program->modules[it_sig->second].spec;
        if (ms.sig == spec.sig && ms.sig2 == spec.sig2) {
            info.AddBindings(binding);
            perm_idx = it_sig->second;
            perm_hash = HashCombine(params.hash, perm_idx);
            module = program->modules[perm_idx].module;
            found = true;
            // Update per-program result cache.
            program->last_result.perm_idx = perm_idx;
            program->last_result.perm_hash = perm_hash;
            program->last_result.module = module;
            program->last_result.valid = true;
        }
    }

    if (!found) {
        // Fallback: linear scan by (sig,sig2) without deep comparisons.
        size_t found_idx = std::numeric_limits<size_t>::max();
        for (size_t i = 0; i < program->modules.size(); ++i) {
            const auto& ms = program->modules[i].spec;
            if (ms.sig == spec.sig && ms.sig2 == spec.sig2) {
                found_idx = i;
                break;
            }
        }

        if (found_idx == std::numeric_limits<size_t>::max()) {
            auto new_info = Shader::Info(stage, l_stage, params);
            module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

            RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
            program->AddPermut(module, std::move(spec));
        } else {
            info.AddBindings(binding);
            module = program->modules[found_idx].module;
            perm_idx = found_idx;
            perm_hash = HashCombine(params.hash, perm_idx);
            // Keep the map warm for future lookups.
            program->perm_index_by_sig.try_emplace(spec.sig, perm_idx);
            // Update per-program result cache.
            program->last_result.perm_idx = perm_idx;
            program->last_result.perm_hash = perm_hash;
            program->last_result.module = module;
            program->last_result.valid = true;
        }
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::Stage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!Config::dumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::Stage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
                                                              }
} // namespace Vulkan
