// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "common/debug.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/draw_bundle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"


#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <xxhash.h>

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif


namespace Vulkan {


static void MakeUserData(Shader::PushData& push_data, const AmdGpu::Regs& regs) {
    // PERF(v7): Zero only buf_offsets (40 bytes) with a single memset instead of
    // value-initializing the full ~120-byte PushData every draw. ud_regs are
    // always overwritten by Info::PushUd for every register the shader consumes.
    std::memset(push_data.buf_offsets.data(), 0, push_data.buf_offsets.size());

    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    if (!Config::nullGpu()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlags{}, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // =========================================================================
    // Render target identity hash: skip FindImage when CB/DB addresses unchanged.
    // FindImage is the most expensive per-draw call (~page table walk + hash lookup).
    // Render targets change on pass switches, not between draws within a pass.
    // =========================================================================
    auto mix = [](u64 h, u64 v) noexcept -> u64 {
        return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    };
    u64 rt_hash = 0x84222325cbf29ce4ULL;
    rt_hash = mix(rt_hash, key.mrt_mask);
    rt_hash = mix(rt_hash, skip_cb_binding ? 1ULL : 0ULL);
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        if (!skip_cb_binding && regs.color_buffers[cb] &&
            regs.color_target_mask.GetMask(cb) && (key.mrt_mask & (1 << cb))) {
            rt_hash = mix(rt_hash, regs.color_buffers[cb].Address());
            rt_hash = mix(rt_hash, liverpool->last_cb_extent[cb].raw);
        }
    }
    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        rt_hash = mix(rt_hash, regs.depth_buffer.DepthAddress());
        rt_hash = mix(rt_hash, liverpool->last_db_extent.raw);
        rt_hash = mix(rt_hash, regs.depth_htile_data_base.GetAddress());
    }

    if (rt_cache_.valid && rt_cache_.hash == rt_hash) {
        // Fast path: render targets unchanged. Reuse cached image_ids.
        // Must still add to bound_images and re-mark is_target (cleared by ResetBindings).
        for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
            auto& [image_id, desc] = cb_descs[cb];
            image_id = rt_cache_.cb_image_ids[cb];
            if (!image_id) {
                continue;
            }
            bound_images.emplace_back(image_id);
            texture_cache.GetImage(image_id).binding.is_target = 1u;
        }
        {
            auto& [image_id, desc] = db_desc;
            image_id = rt_cache_.db_image_id;
            if (image_id) {
                bound_images.emplace_back(image_id);
                texture_cache.GetImage(image_id).binding.is_target = 1u;
            }
        }
        return;
    }

    // Full path: resolve render targets via FindImage.
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            rt_cache_.cb_image_ids[cb] = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
        rt_cache_.cb_image_ids[cb] = image_id;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
        rt_cache_.db_image_id = image_id;
    } else {
        db_desc.first = {};
        rt_cache_.db_image_id = {};
    }

    rt_cache_.hash = rt_hash;
    rt_cache_.valid = true;
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    // Batch PopPendingOperations: only check every 16th draw.
    // Pending ops are deferred GPU-side completions; 16-draw latency is imperceptible.
    if ((draw_counter_++ & 15) == 0) {
        scheduler.PopPendingOperations();
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    // Skip FilterDraw when pipeline hasn't changed — FilterDraw only reads
    // key-affecting regs (color_control.mode, primitive_type, depth overrides)
    // which are stable within the same pipeline.
    if (pipeline != last_filter_pipeline_) {
        last_filter_result_ = FilterDraw();
        last_filter_pipeline_ = pipeline;
    }
    if (!last_filter_result_) {
        return;
    }



    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    if (scheduler.IsThreadedRecording()) {
        // =====================================================================
        // Phase 1B: VB, IB, barriers, push, descriptors, dynamic state,
        // bindPipeline → direct cmdbuf. Only beginRendering + draw → bundle.
        // =====================================================================
        buffer_cache.BindVertexBuffers(*pipeline);
        if (is_indexed) {
            buffer_cache.BindIndexBuffer(index_offset);
        }

        pipeline->BindResources(set_writes, buffer_barriers, push_data);
        UpdateDynamicState(pipeline, is_indexed);
        scheduler.GetDynamicState().Commit(instance, scheduler.CommandBuffer());

        BindPipelineCached(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

        const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
        const auto& fetch_shader = pipeline->GetFetchShader();
        const auto [vertex_offset, instance_offset] =
            GetDrawOffsets(regs, vs_info, fetch_shader);

        auto* bundle = scheduler.AllocateBundle();
        bundle->type = is_indexed ? DrawBundle::Type::DrawIndexed : DrawBundle::Type::Draw;
        bundle->render_state = state;
        bundle->has_render_state = true;
        bundle->num_indices = regs.num_indices;
        bundle->num_instances = regs.num_instances.NumInstances();
        bundle->vertex_offset = static_cast<s32>(vertex_offset);
        bundle->instance_offset = instance_offset;

        scheduler.SubmitBundle();
        ResetBindings();
        return;
    }

    // Non-threaded path (unchanged).
    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    BindPipelineCached(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    // DrawIndirect uses non-threaded cmdbuf access. Drain any pending
    // recorder bundles to avoid racing on current_cmdbuf.
    scheduler.DrainRecorderQueue();

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    BindPipelineCached(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }


    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    BindPipelineCached(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    BindPipelineCached(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
}

void Rasterizer::PrepareDescriptorDeltaCache(const Pipeline* pipeline) {
    const auto cmdbuf = scheduler.CommandBuffer();
    const u64 tick = scheduler.CurrentTick();
    const auto layout = pipeline ? pipeline->GetLayout() : vk::PipelineLayout{};

    // FIX(GR2FORK): when the pipeline uses DESCRIPTOR SETS (not push
    // descriptors), each commit allocates a fresh VkDescriptorSet that starts
    // with every binding uninitialized. The delta filter cannot skip writes
    // on that path or those bindings stay unwritten and the draw faults
    // (VUID-vkCmdDrawIndexed-None-08114, RADV DEVICE_LOST on first indexed
    // sample/ssbo load). Force-invalidate the cache every call on the
    // descriptor-set path so ShouldWriteDescriptor always returns true.
    //
    // On push-descriptor pipelines the delta filter is still safe and
    // beneficial — push state persists across draws in the same cmdbuf.
    const bool uses_push = pipeline && pipeline->UsesPushDescriptors();

    // IMPORTANT: Vulkan command buffers may be re-used across recordings with the same handle.
    // Key the delta-cache on the current submit tick + layout to avoid skipping the FIRST push
    // in a fresh recording (which causes validation errors + VK_ERROR_DEVICE_LOST).
    if (!uses_push ||
        desc_cache_cmdbuf != cmdbuf || desc_cache_tick != tick || desc_cache_layout != layout ||
        desc_cache_pipeline != pipeline) {
        desc_cache_cmdbuf = cmdbuf;
        desc_cache_tick = tick;
        desc_cache_layout = layout;
        desc_cache_pipeline = pipeline;

        // Bump epoch instead of clearing the whole cache.
        ++desc_cache_epoch;
        if (desc_cache_epoch == 0) {
            // Extremely unlikely wrap; reset epochs to force invalid.
            desc_cache_epoch = 1;
            for (auto& e : desc_cache) {
                e.epoch = 0;
            }
        }
    }
}

bool Rasterizer::ShouldWriteDescriptor(u32 binding, vk::DescriptorType type, u64 a, u64 b, u64 c) {
    if (binding >= desc_cache.size()) [[unlikely]] {
        return true;
    }
    auto& e = desc_cache[binding];
    if (e.epoch == desc_cache_epoch && e.type == type && e.a == a && e.b == b && e.c == c) [[likely]] {
        return false;
    }
    e.epoch = desc_cache_epoch;
    e.type = type;
    e.a = a;
    e.b = b;
    e.c = c;
    return true;
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) || IsComputeImageClear(pipeline)) {
        return false;
    }

    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();

    PrepareDescriptorDeltaCache(pipeline);

    Shader::Backend::Bindings binding{};
    MakeUserData(push_data, liverpool->regs);

    // =========================================================================
    // Stage binding skip: hash user_data per stage. When pipeline + user_data
    // + cmdbuf tick are unchanged, BindBuffers/BindTextures would iterate
    // hundreds of lines just for ShouldWriteDescriptor to skip everything.
    // Short-circuit that: call PushUd + advance counters only.
    // =========================================================================
    const u64 tick = scheduler.CurrentTick();
    // FIX(GR2FORK): the all_stages_cached fast path below emits an empty
    // set_writes and relies on the pipeline-side push-descriptor state to
    // persist across draws. Push descriptors support that; descriptor sets
    // do not — each newly-committed VkDescriptorSet starts uninitialized,
    // so an empty set_writes produces an empty set. Skip the fast path on
    // descriptor-set pipelines to force a full rebind.
    const bool pipeline_uses_push = pipeline && pipeline->UsesPushDescriptors();
    bool all_stages_cached = !pipeline->IsCompute() &&
                              pipeline_uses_push &&
                              binding_skip_cache_.valid &&
                              binding_skip_cache_.pipeline == pipeline &&
                              binding_skip_cache_.cmdbuf_tick == tick;

    if (all_stages_cached) {
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) continue;
            const u32 si = static_cast<u32>(stage->l_stage);
            const u64 ud_hash = XXH3_64bits(stage->user_data.data(),
                                             stage->user_data.size_bytes());
            if (binding_skip_cache_.stages[si].pgm_hash != stage->pgm_hash ||
                binding_skip_cache_.stages[si].ud_hash != ud_hash) {
                all_stages_cached = false;
                break;
            }
        }
    }

    if (all_stages_cached && !binding_skip_cache_.uses_dma) {
        // Fast path: identical bindings — just update push constants + counters.
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) continue;
            stage->PushUd(binding, push_data);
            binding.buffer += stage->buffers.size();
            binding.unified += stage->buffers.size() +
                               stage->images.size() +
                               stage->samplers.size();
        }
        return true;
    }

    // Full binding path.
    bool uses_dma = false;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) continue;
        uses_dma |= stage->uses_dma;
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
    }

    // Update skip cache for next draw.
    binding_skip_cache_.pipeline = pipeline;
    binding_skip_cache_.cmdbuf_tick = tick;
    binding_skip_cache_.uses_dma = uses_dma;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) continue;
        const u32 si = static_cast<u32>(stage->l_stage);
        binding_skip_cache_.stages[si].pgm_hash = stage->pgm_hash;
        binding_skip_cache_.stages[si].ud_hash =
            XXH3_64bits(stage->user_data.data(), stage->user_data.size_bytes());
    }
    binding_skip_cache_.valid = true;

    if (uses_dma) {
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}


void Rasterizer::BindPipelineCached(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline) {
    const auto cmdbuf = scheduler.CommandBuffer();
    const u64 tick = scheduler.CurrentTick();
    if (last_bound_pipeline_ == pipeline && last_bound_pipeline_cmdbuf_ == cmdbuf &&
        last_bound_pipeline_tick_ == tick) {
        return;
    }
    cmdbuf.bindPipeline(bind_point, pipeline);
    last_bound_pipeline_ = pipeline;
    last_bound_pipeline_cmdbuf_ = cmdbuf;
    last_bound_pipeline_tick_ = tick;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    // OPT-BUF: per-call FindBuffer() de-dup cache.
    // Safe: only dedups within this BindBuffers() call (cannot go stale across frames).
    struct LocalFindBufferCacheEntry {
        VAddr guest_address{};
        u64 guest_size{};
        VideoCore::BufferId buffer_id{};
    };
    static thread_local std::array<LocalFindBufferCacheEntry, 32> find_buffer_cache;
    u32 find_buffer_cache_size = 0;

    auto FindBufferCached = [&](VAddr addr, u64 size) -> VideoCore::BufferId {
        for (u32 i = 0; i < find_buffer_cache_size; ++i) {
            const auto& e = find_buffer_cache[i];
            if (e.guest_address == addr && e.guest_size == size) {
                return e.buffer_id;
            }
        }
        const auto id = buffer_cache.FindBuffer(addr, size);
        if (find_buffer_cache_size < find_buffer_cache.size()) {
            find_buffer_cache[find_buffer_cache_size++] = LocalFindBufferCacheEntry{addr, size, id};
        }
        return id;
    };
    struct LocalObtainBufferCacheEntry {
        VAddr guest_address{};
        u64 guest_size{};
        bool is_written{};
        bool is_formatted{};
        VideoCore::BufferId buffer_id{};
        VideoCore::Buffer* vk_buffer{};
        u32 offset{};
    };
    static thread_local std::array<LocalObtainBufferCacheEntry, 32> obtain_buffer_cache{};
    u32 obtain_buffer_cache_size = 0;

    auto ObtainBufferCached = [&](VAddr addr, u64 size, bool is_written, bool is_formatted,
                                  VideoCore::BufferId buffer_id) -> std::pair<VideoCore::Buffer*, u32> {
        for (u32 i = 0; i < obtain_buffer_cache_size; ++i) {
            const auto& e = obtain_buffer_cache[i];
            if (e.guest_address == addr && e.guest_size == size && e.is_written == is_written &&
                e.is_formatted == is_formatted && e.buffer_id == buffer_id) {
                return {e.vk_buffer, e.offset};
            }
        }
        const auto [vk_buffer, offset] =
            buffer_cache.ObtainBuffer(addr, size, is_written, is_formatted, buffer_id);
        if (obtain_buffer_cache_size < obtain_buffer_cache.size()) {
            obtain_buffer_cache[obtain_buffer_cache_size++] = LocalObtainBufferCacheEntry{
                .guest_address = addr,
                .guest_size = size,
                .is_written = is_written,
                .is_formatted = is_formatted,
                .buffer_id = buffer_id,
                .vk_buffer = vk_buffer,
                .offset = offset,
            };
        }
        return {vk_buffer, offset};
    };

    // Single fused pass: resolve buffer_id and bind in one iteration.
    // Eliminates the buffer_bindings intermediate vector (clear + emplace_back + read-back).
    for (u32 i = 0; i < stage.buffers.size(); i++) {
        const auto& desc = stage.buffers[i];
        const auto vsharp = desc.GetSharp(stage);

        VideoCore::BufferId buffer_id{};
        u64 size = 0;
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            buffer_id = FindBufferCached(vsharp.base_address, size);
        }

        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);

                struct FlatbufCacheEntry {
                    u64 stage_key = 0;
                    vk::CommandBuffer cmdbuf{};
                    u64 hash = 0;
                    u64 offset = 0;
                    u32 size = 0;
                    bool valid = false;
                };
                static thread_local std::array<FlatbufCacheEntry, 32> flatbuf_cache{};

                const auto cmdbuf = scheduler.CommandBuffer();
                const u64 payload_hash = (ubo_size == 0) ? 0 : XXH3_64bits(stage.flattened_ud_buf.data(), ubo_size);
                const u64 stage_key = static_cast<u64>(stage.pgm_hash) ^ (static_cast<u64>(desc.sharp_idx) << 1);

                FlatbufCacheEntry& e = flatbuf_cache[stage_key & (flatbuf_cache.size() - 1)];

                u64 offset = 0;
                if (e.valid && e.stage_key == stage_key && e.cmdbuf == cmdbuf && e.size == ubo_size &&
                    e.hash == payload_hash) {
                    offset = e.offset;
                    } else {
                        offset = vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                        e.stage_key = stage_key;
                        e.cmdbuf = cmdbuf;
                        e.hash = payload_hash;
                        e.offset = offset;
                        e.size = ubo_size;
                        e.valid = true;
                    }

                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);

                constexpr u64 kGpuFillThreshold = 256;
                if (lds_size >= kGpuFillThreshold) {
                    (void)data;
                    auto cmdbuf = scheduler.CommandBuffer();
                    cmdbuf.fillBuffer(lds_buffer.Handle(), offset, lds_size, 0);

                    buffer_barriers.push_back(vk::BufferMemoryBarrier2{
                        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .buffer = lds_buffer.Handle(),
                        .offset = offset,
                        .size = lds_size,
                    });
                } else {
                    std::memset(data, 0, lds_size);
                }

                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = ObtainBufferCached(
                vsharp.base_address, size, desc.is_written, desc.is_formatted, buffer_id);
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          // OPT: Use specific shader stage instead of eAllCommands.
                                          // eAllCommands forces a full pipeline drain which prevents
                                          // the GPU from overlapping independent work.
                                          vk::PipelineStageFlagBits2::eAllGraphics |
                                              vk::PipelineStageFlagBits2::eComputeShader)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

        const u32 dst_binding = binding.unified++;
        const vk::DescriptorType dtype =
        is_storage ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eUniformBuffer;

        const auto bi_handle = reinterpret_cast<u64>(static_cast<VkBuffer>(buffer_infos.back().buffer));
        const u64 bi_offset = static_cast<u64>(buffer_infos.back().offset);
        const u64 bi_range  = static_cast<u64>(buffer_infos.back().range);

        if (ShouldWriteDescriptor(dst_binding, dtype, bi_handle, bi_offset, bi_range)) {
            auto& w = set_writes.emplace_back();
            w.dstSet = VK_NULL_HANDLE;
            w.dstBinding = dst_binding;
            w.dstArrayElement = 0;
            w.descriptorCount = 1;
            w.descriptorType = dtype;
            w.pNext = nullptr;
            w.pImageInfo = nullptr;
            w.pBufferInfo = &buffer_infos.back();
            w.pTexelBufferView = nullptr;
        }

        ++binding.buffer;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    bool any_needs_rebind = false; // OPT(v18): Track during first pass instead of separate scan

    // OPT(v14.1 hotfix): Keep caches off TLS.
    //
    // v14 used large thread_local std::array<...> caches inside BindTextures(). That inflates TLS,
    // and with shadPS4's custom pthread stacks, some systems hit pthread_create() EINVAL (22)
    // during early game startup. Store the caches on the Rasterizer instance instead.
    auto& find_image_cache = find_image_cache_;
    const bool null_descriptors_supported = instance.IsNullDescriptorSupported();
    const bool attachment_feedback_loop_layout_supported =
    instance.IsAttachmentFeedbackLoopLayoutSupported();

    auto make_flags = [](const Shader::ImageResource& r) noexcept -> u32 {
        // Pack the bits that affect ImageInfo/ImageViewInfo construction.
        return (u32(r.is_depth) << 0) | (u32(r.is_atomic) << 1) | (u32(r.is_array) << 2) |
        (u32(r.is_written) << 3) | (u32(r.is_r128) << 4);
    };

    auto make_key = [&](const Shader::ImageResource& r, u32 flags) noexcept -> u64 {
        // Stage + slot + flags. We still memcmp the raw Image descriptor for full match.
        u64 k = stage.pgm_hash;
        k ^= (static_cast<u64>(r.sharp_idx) << 1);
        k ^= (static_cast<u64>(flags) << 32);
        k ^= (static_cast<u64>(static_cast<u32>(stage.l_stage)) << 56);
        return k;
    };

    // PERF(GR2 v17): Epoch-based validity — avoids zeroing 640+ bytes of stamp arrays per call.
    const u32 epoch = ++bind_textures_epoch_;
    // Handle epoch wrap (extremely unlikely, ~every 4 billion BindTextures calls)
    if (epoch == 0) {
        bind_textures_epoch_ = 1;
        find_image_cache_stamp_.fill(0);
        find_texture_cache_stamp_.fill(0);
    }
    auto& find_image_cache_stamp = find_image_cache_stamp_;

    // OPT(v15): De-dup FindTexture() within this BindTextures() call.
    // GR2 frequently binds the same {image_id,type,view_info} multiple times across stages.
    // FindTexture() takes a lock (UpdateImage) and does view lookup, so avoiding duplicates
    // reduces GpuComm overhead (rwlock + Image::FindView + barriers).
    auto& find_texture_cache = find_texture_cache_;
    auto& find_texture_cache_stamp = find_texture_cache_stamp_;
    auto hash_view_info = [](const VideoCore::ImageViewInfo& v) noexcept -> u64 {
        // PERF(GR2 v8): Pack fields into 2-3 mix calls instead of 11.
        // This saves ~8 multiply+xor cycles per image binding in the hot path.
        auto mix = [](u64 h, u64 x) noexcept {
            h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        };
        u64 h = 0x84222325cbf29ce4ULL;
        // Pack type(8) + format(8) + level(8) + layer(8) + levels(8) + layers(8) + storage(1) = fits in 64 bits
        const u64 packed0 =
            (static_cast<u64>(static_cast<u32>(v.type))) |
            (static_cast<u64>(static_cast<u32>(v.format)) << 8) |
            (static_cast<u64>(v.range.base.level) << 16) |
            (static_cast<u64>(v.range.base.layer) << 24) |
            (static_cast<u64>(v.range.extent.levels) << 32) |
            (static_cast<u64>(v.range.extent.layers) << 40) |
            (static_cast<u64>(v.is_storage ? 1u : 0u) << 48);
        h = mix(h, packed0);
        // Pack swizzle components into one u64
        const u64 packed1 =
            (static_cast<u64>(v.mapping.r)) |
            (static_cast<u64>(v.mapping.g) << 8) |
            (static_cast<u64>(v.mapping.b) << 16) |
            (static_cast<u64>(v.mapping.a) << 24);
        h = mix(h, packed1);
        return h;
    };

    auto FindTextureCached = [&](VideoCore::ImageId image_id,
                                 VideoCore::TextureCache::BindingType type,
                                 const VideoCore::ImageViewInfo& view_info) -> VideoCore::ImageView& {
                                     const u64 k = (static_cast<u64>(image_id.index) * 0x9e3779b97f4a7c15ULL) ^
                                     (static_cast<u64>(static_cast<u32>(type)) << 48) ^
                                     hash_view_info(view_info);
                                     const u32 slot = static_cast<u32>(k) & (find_texture_cache_stamp.size() - 1);
                                     auto& e = find_texture_cache[slot];
                                     if (find_texture_cache_stamp[slot] == epoch &&
                                         e.image_id == image_id &&
                                         e.type == type &&
                                         e.view &&
                                         e.view->info == view_info) {
                                         return *e.view;
                                         }
                                         auto& v = texture_cache.FindTexture(image_id, type, view_info);
                                     e = FindTextureCacheEntry{image_id, type, &v};
                                     find_texture_cache_stamp[slot] = epoch;
                                     return v;
                                 };

                                 for (const auto& image_res : stage.images) {
                                     auto tsharp = image_res.GetSharp(stage);

                                     if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }


        if (tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) [[unlikely]] {
            ImageBindingInfo& nb = image_bindings.emplace_back();
            // FIX(GR2FORK): the layout always reserves NumBindings() slots for
            // this ImageResource regardless of whether the sharp resolved to
            // something real. Preserve num_bindings here so the 2nd-pass null
            // emission writes the full array.
            nb.num_bindings =
                static_cast<u8>(std::min<u32>(255u, image_res.NumBindings(stage)));
            continue;
        }

        const u32 flags = make_flags(image_res);
        const u64 key = make_key(image_res, flags);
        // PERF(GR2 v16): Better cache slot selection to reduce collisions.
        // The old key & (size-1) used low bits which cluster when pgm_hash is stable.
        // Mix the key first with a multiplicative hash for better distribution.
        const u64 mixed_key = (key ^ (key >> 16)) * 0x9e3779b97f4a7c15ULL;
        CachedImageDescEntry& ce = image_desc_cache_[static_cast<u32>(mixed_key >> 20) & (image_desc_cache_.size() - 1)];

        if (!(ce.valid && ce.key == key && std::memcmp(&ce.image, &tsharp, sizeof(tsharp)) == 0)) {
            ce.key = key;
            ce.image = tsharp;
            ce.desc = VideoCore::TextureCache::ImageDesc(tsharp, image_res);
            ce.valid = true;
        }
        const auto* base_desc = &ce.desc;

        // De-dup FindImage within this call (common when multiple stages alias).
        VideoCore::TextureCache::FindImageWithViewResult found{};
        bool found_cache_hit = false;
        {
            const u64 k = (static_cast<u64>(reinterpret_cast<uintptr_t>(base_desc)) >> 4);
            const u32 slot = static_cast<u32>(k) & (find_image_cache.size() - 1);
            auto& e = find_image_cache[slot];
            if (find_image_cache_stamp[slot] == epoch && e.valid && e.base == base_desc) {
                found = e.res;
                found_cache_hit = true;
            }
        }
        if (!found_cache_hit) {
            // Cross-call cache: validate cached resolution cheaply and skip full page-table walk when stable.
            // PERF(GR2 v16): Better hash distribution for cache slot.
            const u64 pkey_mixed = (key ^ (key >> 16)) * 0xbf58476d1ce4e5b9ULL;
            const auto pslot = static_cast<u32>(pkey_mixed >> 20) &
            static_cast<u32>(find_image_pcache_.size() - 1);
            auto& pe = find_image_pcache_[pslot];
            if (pe.valid && pe.key == key && pe.base == base_desc &&
                texture_cache.ValidateCachedFindImage(*base_desc, pe.res.image_id, false)) {
                found = pe.res;
            found_cache_hit = true;
                }
        }
        if (!found_cache_hit) {
            found = texture_cache.FindImageWithView(*base_desc, false, false);
        }
        {
            const u64 k = (static_cast<u64>(reinterpret_cast<uintptr_t>(base_desc)) >> 4);
            const u32 slot = static_cast<u32>(k) & (find_image_cache.size() - 1);
            find_image_cache[slot] = LocalFindImageCacheEntry{.base = base_desc, .res = found, .valid = true};
            find_image_cache_stamp[slot] = epoch;

            const u64 pkey_mixed2 = (key ^ (key >> 16)) * 0xbf58476d1ce4e5b9ULL;
            const auto pslot = static_cast<u32>(pkey_mixed2 >> 20) &
            static_cast<u32>(find_image_pcache_.size() - 1);
            find_image_pcache_[pslot] = PersistentFindImageCacheEntry{
                .key = key,
                .base = base_desc,
                .res = found,
                .valid = true,
            };
        }

        auto image_id = found.image_id;

        auto* image = &texture_cache.GetImage(image_id);
        if (image->depth_id) {
            // If this image has an associated depth image, it's a stencil attachment.
            // Redirect the access to the actual depth-stencil buffer.
            image_id = image->depth_id;
            image = &texture_cache.GetImage(image_id);
        }
        // OPT(v18): Track needs_rebind here instead of a separate O(N) scan loop.
        any_needs_rebind |= image->binding.needs_rebind;
        if (image->binding.is_bound) {
            // The image is already bound. In case if it is about to be used as storage we need
            // to force general layout on it.
            image->binding.force_general |= image_res.is_written;
        }
        image->binding.is_bound = 1u;

        ImageBindingInfo& b = image_bindings.emplace_back();
        b.image_id = image_id;
        b.desc = base_desc;
        b.view_mip = static_cast<s16>(found.view_mip);
        b.view_slice = static_cast<s16>(found.view_slice);
        // FIX(GR2FORK): capture NumBindings for the 2nd-loop mip-fallback
        // expansion. DynamicIndex images want >1 consecutive descriptor slots.
        b.num_bindings = static_cast<u8>(std::min<u32>(255u, image_res.NumBindings(stage)));
    }

    // Second pass to re-bind images that were updated after binding.
    //
    // PERF(GR2 v16 + v18): Track whether any image needs rebinding during first pass.
    // Eliminates the O(N) scan loop that previously iterated all image_bindings.
    //
    // PERF(GR2): FindTexture() always calls TextureCache::UpdateImage(), which takes a shared_lock even
    // when the image is clean. GR2 often touches the same image multiple times with different view_info
    // (mip/slice variants), which causes repeated lock traffic. De-dup UpdateImage() per ImageId within
    // this BindTextures() call, and for sampled images call Image::FindView() directly (no extra UpdateImage).
    //
    // PERF(GR2 v17): Use PERSISTENT tick-based cache instead of per-call stamp arrays.
    // This de-dups UpdateImage across multiple draws within the same command buffer tick,
    // avoiding redundant shared_lock acquire/release for images that were already checked this tick.
    const u64 current_tick = scheduler.CurrentTick();
    auto UpdateImageOnce = [&](VideoCore::ImageId id) {
        const u64 k = (static_cast<u64>(id.index) * 0x9e3779b97f4a7c15ULL);
        const u32 slot = static_cast<u32>(k) & (update_image_cache_.size() - 1);
        auto& e = update_image_cache_[slot];
        if (e.image_id == id && e.tick == current_tick) {
            return;
        }
        texture_cache.UpdateImage(id);
        e.image_id = id;
        e.tick = current_tick;
    };
    for (auto& b : image_bindings) {
        const auto* base_desc = b.desc;
        const bool is_storage = base_desc && base_desc->type == VideoCore::TextureCache::BindingType::Storage;

        if (!base_desc || !b.image_id) {
            // FIX(GR2FORK): honor mip-fallback array slot count on null path.
            // Layout still reserves num_bindings descriptors even when the
            // sharp was invalid, so we must emit that many image_infos to
            // keep binding.unified and the descriptorCount-N write aligned.
            const u32 null_count = b.num_bindings ? b.num_bindings : 1u;
            for (u32 null_i = 0; null_i < null_count; ++null_i) {
                if (null_descriptors_supported) {
                    image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
                } else {
                    VideoCore::ImageViewInfo view_info{};
                    if (base_desc) {
                        view_info = base_desc->view_info;
                        if (b.view_mip > 0) {
                            view_info.range.base.level = b.view_mip;
                        }
                        if (b.view_slice > 0) {
                            view_info.range.base.layer = b.view_slice;
                        }
                    }
                    auto& null_image_view =
                    texture_cache.FindTexture(VideoCore::NULL_IMAGE_ID,
                                              VideoCore::TextureCache::BindingType::Texture,
                                              view_info);
                    image_infos.emplace_back(VK_NULL_HANDLE, *null_image_view.image_view,
                                             vk::ImageLayout::eGeneral);
                }
            }
        } else {
            // PERF(GR2 v16): Only check needs_rebind when we know at least one image needs it.
            if (any_needs_rebind) {
                if (auto& old_image = texture_cache.GetImage(b.image_id); old_image.binding.needs_rebind) {
                    old_image.binding = {};
                    const auto rebound = texture_cache.FindImageWithView(*base_desc, false, false);
                    b.image_id = rebound.image_id;
                    b.view_mip = static_cast<s16>(rebound.view_mip);
                    b.view_slice = static_cast<s16>(rebound.view_slice);
                }
            }

            bound_images.emplace_back(b.image_id);

            auto& image = texture_cache.GetImage(b.image_id);

            VideoCore::ImageViewInfo view_info = base_desc->view_info;
            if (b.view_mip > 0) {
                view_info.range.base.level = b.view_mip;
            }
            if (b.view_slice > 0) {
                view_info.range.base.layer = b.view_slice;
            }

            // FIX(GR2FORK): PORT(upstream #4075). For DynamicIndex mip-fallback
            // images, the layout declares descriptorCount = num_mips (one slot
            // per mip). Emit N views + N image_infos here so every array slot
            // gets populated, then emit a single descriptor write below with
            // descriptorCount = num_bindings. Without this, elements >= 1 stay
            // uninitialized and dispatches that index them (e.g. cs_img16 lod=1)
            // fault on RADV -> VK_ERROR_DEVICE_LOST.
            //
            // For the common num_bindings==1 case this loop runs once and is
            // identical to the prior single-emission path.
            const u32 num_bindings = b.num_bindings ? b.num_bindings : 1u;

            for (u32 mip_offset = 0; mip_offset < num_bindings; ++mip_offset) {
                VideoCore::ImageViewInfo mip_view_info = view_info;
                if (num_bindings > 1) {
                    // FIX(GR2FORK): When expanding a mip-fallback array, each
                    // descriptor slot must view exactly one mip level — that's
                    // the whole point of binding-per-mip. The original view_info
                    // may carry extent.levels covering the full chain (e.g. 6),
                    // which combined with a shifted baseMipLevel violates
                    // VUID-VkImageViewCreateInfo-subresourceRange-01718 (base +
                    // count exceeds image mipLevels). Shader indexes per slot,
                    // so single-level views are the correct geometry here.
                    const u32 desired_level = view_info.range.base.level + mip_offset;
                    const u32 max_level = image.info.resources.levels > 0
                                              ? image.info.resources.levels - 1
                                              : 0;
                    mip_view_info.range.base.level =
                        std::min<u32>(desired_level, max_level);
                    mip_view_info.range.extent.levels = 1;
                }

                VideoCore::ImageView* view_ptr = nullptr;
                if (is_storage) {
                    // Keep the original FindTexture() path for storage images (it tags GpuModified + readback).
                    auto& image_view = FindTextureCached(b.image_id, base_desc->type, mip_view_info);
                    view_ptr = &image_view;
                } else {
                    // Sampled images: UpdateImage once per ImageId, then find view without re-taking the lock.
                    UpdateImageOnce(b.image_id);
                    view_ptr = &image.FindView(mip_view_info);
                }

                // Layout transitions only need to happen once per image, but
                // doing them inside the loop is harmless (Transit is a no-op
                // when already in the target state thanks to ARCH-7).
                // Keep transitions driven by the full requested view range
                // (base mip + all expanded mips) so the barrier covers every
                // slot we're about to sample.
                if ((image.binding.force_general || image.binding.is_target) && !image.info.props.is_depth) {
                    image.Transit(attachment_feedback_loop_layout_supported && image.binding.is_target
                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                    : vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits2::eShaderRead |
                                      (image.info.props.is_depth
                                           ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                           : vk::AccessFlagBits2::eColorAttachmentWrite),
                                  {});
                } else {
                    if (is_storage) {
                        // ARCH-7: Skip Transit when already in target state.
                        const auto storage_access = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
                        if (!image.IsInState(vk::ImageLayout::eGeneral, storage_access)) {
                            image.Transit(vk::ImageLayout::eGeneral, storage_access, mip_view_info.range);
                        }
                    } else {
                        const auto new_layout = image.info.props.is_depth
                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                        : vk::ImageLayout::eShaderReadOnlyOptimal;
                        // ARCH-7: Skip Transit for sampled images already in correct state.
                        // In steady-state rendering, most sampled textures stay in ShaderReadOnlyOptimal
                        // between draws. This avoids Transit → GetBarriers function call overhead.
                        if (!image.IsInState(new_layout, vk::AccessFlagBits2::eShaderRead)) {
                            image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead, mip_view_info.range);
                        }
                    }
                }
                image.usage.storage |= is_storage;
                image.usage.texture |= !is_storage;

                image_infos.emplace_back(VK_NULL_HANDLE, *view_ptr->image_view,
                                         image.backing->state.layout);
            }
        }

        // FIX(GR2FORK): The layout always reserves num_bindings slots for
        // this ImageResource (regardless of whether the sharp was valid). Both
        // branches above (null path and real-image path) now emit exactly
        // num_bindings image_infos, so advance binding.unified and emit a
        // single write with descriptorCount = num_bindings. For the common
        // num_bindings == 1 case, behavior is unchanged from upstream.
        const u32 slot_count = b.num_bindings ? b.num_bindings : 1u;

        const u32 dst_binding = binding.unified;
        binding.unified += slot_count;
        const vk::DescriptorType dtype = is_storage ? vk::DescriptorType::eStorageImage
        : vk::DescriptorType::eSampledImage;

        const u32 write_count = slot_count;
        const size_t array_first = image_infos.size() - slot_count;

        const auto ii_sampler =
        reinterpret_cast<u64>(static_cast<VkSampler>(image_infos[array_first].sampler));
        const auto ii_view =
        reinterpret_cast<u64>(static_cast<VkImageView>(image_infos[array_first].imageView));
        const u64 ii_layout = static_cast<u64>(static_cast<u32>(image_infos[array_first].imageLayout));

        // ShouldWriteDescriptor tracks (binding, type, handles) — for multi-slot
        // arrays we only hash the first slot. In practice the tail slots are
        // derived from the same image (consecutive mips of one texture) so
        // they change together; if a more robust invalidation is ever needed,
        // fold write_count into the signature.
        if (ShouldWriteDescriptor(dst_binding, dtype, ii_sampler, ii_view, ii_layout)) {
            auto& w = set_writes.emplace_back();
            w.dstSet = VK_NULL_HANDLE;
            w.dstBinding = dst_binding;
            w.dstArrayElement = 0;
            w.descriptorCount = write_count;
            w.descriptorType = dtype;
            w.pNext = nullptr;
            w.pImageInfo = &image_infos[array_first];
            w.pBufferInfo = nullptr;
            w.pTexelBufferView = nullptr;
        }
    }

    // Sampler fast-path cache. GetSampler() hashes 64 bytes + does a map
    // lookup every call. A direct-mapped cache on the Rasterizer skips both
    // on hit (>90% in steady-state GR2). Samplers are immutable Vulkan
    // objects so caching handles is always safe.
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }

        const u64 samp_hash = XXH3_64bits(&ssharp, sizeof(ssharp));
        const u32 samp_slot = static_cast<u32>(samp_hash) & (SamplerCacheSize - 1);
        auto& sc = sampler_cache_[samp_slot];
        vk::Sampler vk_sampler;
        if (sc.valid && sc.hash == samp_hash) {
            vk_sampler = sc.sampler;
        } else {
            vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
            sc.hash = samp_hash;
            sc.sampler = vk_sampler;
            sc.valid = true;
        }

        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        const u32 dst_binding = binding.unified++;
        const vk::DescriptorType dtype = vk::DescriptorType::eSampler;

        const auto ii_sampler =
        reinterpret_cast<u64>(static_cast<VkSampler>(image_infos.back().sampler));
        const auto ii_view =
        reinterpret_cast<u64>(static_cast<VkImageView>(image_infos.back().imageView));
        const u64 ii_layout = static_cast<u64>(static_cast<u32>(image_infos.back().imageLayout));

        if (ShouldWriteDescriptor(dst_binding, dtype, ii_sampler, ii_view, ii_layout)) {
            auto& w = set_writes.emplace_back();
            w.dstSet = VK_NULL_HANDLE;
            w.dstBinding = dst_binding;
            w.dstArrayElement = 0;
            w.descriptorCount = 1;
            w.descriptorType = dtype;
            w.pNext = nullptr;
            w.pImageInfo = &image_infos.back();
            w.pBufferInfo = nullptr;
            w.pTexelBufferView = nullptr;
        }
    }
}


RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u32>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);

    // OPT: Reuse the tick-based UpdateImage de-dup cache so that render targets
    // already validated by BindTextures() during this draw don't re-acquire the
    // texture cache shared_lock. This saves ~0.3-0.5% of GpuComm time.
    const u64 current_tick = scheduler.CurrentTick();
    auto UpdateImageDedup = [&](VideoCore::ImageId id) {
        const u64 k = (static_cast<u64>(id.index) * 0x9e3779b97f4a7c15ULL);
        const u32 slot = static_cast<u32>(k) & (update_image_cache_.size() - 1);
        auto& e = update_image_cache_[slot];
        if (e.image_id == id && e.tick == current_tick) {
            return;
        }
        texture_cache.UpdateImage(id);
        e.image_id = id;
        e.tick = current_tick;
    };

    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        // OPT: Use tick-dedup to skip re-acquiring shared_lock if BindTextures
        // already validated this image during the same draw call.
        UpdateImageDedup(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            // ARCH-5: Skip Transit when render target is already in correct state.
            // Consecutive draws with the same render targets hit this ~95% of the time.
            const auto ca_access = vk::AccessFlagBits2::eColorAttachmentWrite |
                                   vk::AccessFlagBits2::eColorAttachmentRead;
            if (!image->IsInState(vk::ImageLayout::eColorAttachmentOptimal, ca_access)) {
                image->Transit(vk::ImageLayout::eColorAttachmentOptimal, ca_access,
                               desc.view_info.range);
            }
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);
        state.color_attachments[cb] = {
            .imageView = *image_view.image_view,
            .imageLayout = image->backing->state.layout,
            .loadOp = is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue =
                is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{},
        };
        image->usage.render_target = 1u;
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;

        // FIX(GR2FORK): US disc GR2 (CUSA03694) crashes in Nevi Hand encounters
        // with VK_ERROR_DEVICE_LOST. Validation layer (VUID-06886/06887) shows
        // the same depth+stencil image bound in DEPTH_STENCIL_READ_ONLY_OPTIMAL
        // while the current pipeline has depthWriteEnable=VK_TRUE and/or
        // stencilTestEnable=VK_TRUE with non-zero writeMask and non-KEEP ops.
        //
        // Prior logic picked the layout purely from view_info.is_storage,
        // ignoring whether the current draw actually writes depth or stencil.
        // Nevi deferred-shading Z-prepass + forward stencil masking reuses the
        // same image: an earlier draw binds it read-only for sampling, a later
        // draw writes it — but because is_storage was false on the later bind,
        // the layout stayed DEPTH_STENCIL_READ_ONLY_OPTIMAL. RADV hangs on the
        // mismatch; other drivers may silently corrupt depth/stencil.
        //
        // Correct decision tree: need writable if is_storage, OR if depth
        // writes are on, OR if stencil writes are on. The stencil writemask
        // lives in stencil_ref_front (mirrored to stencil_ref_back).
        const bool stencil_test_enabled = regs.depth_control.stencil_enable;
        const bool stencil_writes_enabled =
            stencil_test_enabled && regs.stencil_ref_front.stencil_write_mask != 0;
        const bool depth_writes_enabled =
            regs.depth_control.depth_enable && regs.depth_control.depth_write_enable;
        const bool needs_writable_layout =
            desc.view_info.is_storage || depth_writes_enabled || stencil_writes_enabled;

        const auto new_layout = needs_writable_layout
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        const auto ds_access = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                               vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        // ARCH-5: Skip Transit when depth target is already in correct state.
        if (!image.IsInState(new_layout, ds_access)) {
            image.Transit(new_layout, ds_access, desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.has_depth = regs.depth_buffer.DepthValid();
        state.has_stencil = regs.depth_buffer.StencilValid();
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);
        if (state.has_depth) {
            state.depth_attachment = {
                .imageView = *image_view.image_view,
                .imageLayout = image.backing->state.layout,
                .loadOp =
                    is_depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                // FIX(GR2FORK): write both fields of the depthStencil union.
                // Previously only `.depth` was set; `.stencil` defaulted to 0,
                // which is harmless when LOAD_OP_LOAD is in effect for stencil
                // (the clearValue is unused) but produces messy diagnostic
                // output (the YAML dump from the LunarG crash-diagnostic layer
                // shows uninitialized-looking values via the active union
                // member). Writing both ensures the struct always reflects
                // the intended state of the depth/stencil pair regardless of
                // which side actually clears.
                .clearValue = vk::ClearValue{
                    .depthStencil = vk::ClearDepthStencilValue{
                        .depth = regs.depth_clear,
                        .stencil = regs.stencil_clear,
                    },
                },
            };
        }
        if (state.has_stencil) {
            state.stencil_attachment = {
                .imageView = *image_view.image_view,
                .imageLayout = image.backing->state.layout,
                .loadOp =
                    is_stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                // FIX(GR2FORK): see depth_attachment above. Both fields are
                // written so the depth/stencil pair stays consistent across
                // both attachment infos (which share the same imageView for
                // combined depth+stencil formats).
                .clearValue = vk::ClearValue{
                    .depthStencil = vk::ClearDepthStencilValue{
                        .depth = regs.depth_clear,
                        .stencil = regs.stencil_clear,
                    },
                },
            };
        }

        image.usage.depth_target = true;
    }

    if (state.num_layers == std::numeric_limits<u32>::max()) {
        state.num_layers = 1;
    }

    // OPT: Pre-compute hash for fast equality rejection in BeginRendering.
    state.ComputeHash();
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    // Underlying texture/buffer data changed — caches are stale.
    binding_skip_cache_.valid = false;
    rt_cache_.valid = false;
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    binding_skip_cache_.valid = false;
    rt_cache_.valid = false;
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    auto& dynamic_state = scheduler.GetDynamicState();

    // Skip all 6 sub-functions when no dynamic-state register changed since last draw.
    // Each sub-function reads dozens of regs and does comparison/computation that Commit
    // would then discover produced zero dirty flags. ~236 lines of work saved per hit.
    if (!liverpool->IsDynamicDirty()) {
        // In threaded mode, skip Commit — ProcessBundle handles it via snapshot.
        if (!scheduler.IsThreadedRecording()) {
            dynamic_state.Commit(instance, scheduler.CommandBuffer());
        }
        return;
    }
    liverpool->ClearDynamicDirty();

    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    // In threaded mode, skip Commit — ProcessBundle handles it via snapshot.
    if (!scheduler.IsThreadedRecording()) {
        dynamic_state.Commit(instance, scheduler.CommandBuffer());
    }
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        dynamic_state.SetStencilReferences(front.stencil_test_val, back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto prim_restart = (regs.enable_primitive_restart & 1) != 0;
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
