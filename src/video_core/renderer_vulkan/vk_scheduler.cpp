// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/draw_bundle.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore},
      bundle_ring_(std::make_unique<DrawBundleRing>()) {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    // Stop recorder thread first (if active)
    if (recorder_thread_.joinable()) {
        recorder_thread_.request_stop();
        recorder_cv_.notify_all();
        recorder_thread_.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    // In threaded mode, check recorder-private state. ProcessBundle's EndRendering
    // sentinel only resets rec_rendering, not is_rendering. Using is_rendering here
    // would incorrectly think a render pass is still active after a sentinel fired.
    if (threaded_recording_) {
        if (recorder_is_rendering_ && recorder_render_state_ == new_state) {
            return;
        }
        // EndRendering sends sentinel → ProcessBundle ends the render pass.
        EndRendering();
    } else {
        if (is_rendering && render_state == new_state) {
            return;
        }
        EndRendering();
    }
    is_rendering = true;
    render_state = new_state;

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = render_state.num_color_attachments > 0
                                 ? render_state.color_attachments.data()
                                 : nullptr,
        .pDepthAttachment = render_state.has_depth ? &render_state.depth_attachment : nullptr,
        .pStencilAttachment = render_state.has_stencil ? &render_state.stencil_attachment : nullptr,
    };

    current_cmdbuf.beginRendering(rendering_info);

    // Sync recorder-private state so ProcessBundle knows a render pass is active.
    if (threaded_recording_) {
        recorder_is_rendering_ = true;
        recorder_render_state_ = render_state;
    }
}

void Scheduler::EndRendering() {
    if (threaded_recording_) {
        // In threaded mode, EndRendering from the parser thread must go through
        // the recorder to maintain command ordering. Always send a sentinel
        // bundle — ProcessBundle correctly skips endRendering if not in a render pass.
        auto* bundle = AllocateBundle();
        bundle->type = DrawBundle::Type::EndRendering;
        SubmitBundle();
        DrainRecorderQueue();
        // Sync is_rendering with rec_rendering (which ProcessBundle just set to false).
        is_rendering = recorder_is_rendering_;
        return;
    }
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    current_cmdbuf.endRendering();
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= master_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    master_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    if (pending_ops.empty()) [[likely]] {
        return;
    }
    master_semaphore.Refresh();
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();
    if (threaded_recording_) {
        parser_dynamic_state_.Invalidate();
        // Reset pipeline dedup — new cmdbuf has no pipeline bound.
        recorder_last_pipeline_ = vk::Pipeline{};
        // New cmdbuf has no active render pass.
        recorder_is_rendering_ = false;
    }

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // Drain recorder queue before submit — all pending bundles must be recorded.
    if (threaded_recording_) {
        DrainRecorderQueue();
    }
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = master_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    EndRendering();
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    master_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        master_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // OPT: Fast early-exit when nothing is dirty. Most consecutive draws with the
    // same pipeline and state produce zero dirty bits. A single comparison avoids
    // all ~25 individual flag checks.
    {
        // Bitfield struct may have padding, so compare all bytes to zero.
        static constexpr decltype(dirty_state) zero_state{};
        if (std::memcmp(&dirty_state, &zero_state, sizeof(dirty_state)) == 0) [[likely]] {
            return;
        }
    }

    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        // OPT: Batch stencil ops/refs/masks. Check front+back together to reduce branching.
        const bool front_ops_dirty = dirty_state.stencil_front_ops;
        const bool back_ops_dirty = dirty_state.stencil_back_ops;
        if (front_ops_dirty || back_ops_dirty) {
            if (front_ops_dirty && back_ops_dirty && stencil_front_ops == stencil_back_ops) {
                dirty_state.stencil_front_ops = false;
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack,
                                    stencil_front_ops.fail_op, stencil_front_ops.pass_op,
                                    stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            } else {
                if (front_ops_dirty) {
                    dirty_state.stencil_front_ops = false;
                    cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront,
                                        stencil_front_ops.fail_op, stencil_front_ops.pass_op,
                                        stencil_front_ops.depth_fail_op,
                                        stencil_front_ops.compare_op);
                }
                if (back_ops_dirty) {
                    dirty_state.stencil_back_ops = false;
                    cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack,
                                        stencil_back_ops.fail_op, stencil_back_ops.pass_op,
                                        stencil_back_ops.depth_fail_op,
                                        stencil_back_ops.compare_op);
                }
            }
        }
        const bool front_ref_dirty = dirty_state.stencil_front_reference;
        const bool back_ref_dirty = dirty_state.stencil_back_reference;
        if (front_ref_dirty || back_ref_dirty) {
            if (front_ref_dirty && back_ref_dirty &&
                stencil_front_reference == stencil_back_reference) {
                dirty_state.stencil_front_reference = false;
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                           stencil_front_reference);
            } else {
                if (front_ref_dirty) {
                    dirty_state.stencil_front_reference = false;
                    cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                               stencil_front_reference);
                }
                if (back_ref_dirty) {
                    dirty_state.stencil_back_reference = false;
                    cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack,
                                               stencil_back_reference);
                }
            }
        }
        const bool front_wm_dirty = dirty_state.stencil_front_write_mask;
        const bool back_wm_dirty = dirty_state.stencil_back_write_mask;
        if (front_wm_dirty || back_wm_dirty) {
            if (front_wm_dirty && back_wm_dirty &&
                stencil_front_write_mask == stencil_back_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                           stencil_front_write_mask);
            } else {
                if (front_wm_dirty) {
                    dirty_state.stencil_front_write_mask = false;
                    cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                               stencil_front_write_mask);
                }
                if (back_wm_dirty) {
                    dirty_state.stencil_back_write_mask = false;
                    cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack,
                                               stencil_back_write_mask);
                }
            }
        }
        const bool front_cm_dirty = dirty_state.stencil_front_compare_mask;
        const bool back_cm_dirty = dirty_state.stencil_back_compare_mask;
        if (front_cm_dirty || back_cm_dirty) {
            if (front_cm_dirty && back_cm_dirty &&
                stencil_front_compare_mask == stencil_back_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                             stencil_front_compare_mask);
            } else {
                if (front_cm_dirty) {
                    dirty_state.stencil_front_compare_mask = false;
                    cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                                 stencil_front_compare_mask);
                }
                if (back_cm_dirty) {
                    dirty_state.stencil_back_compare_mask = false;
                    cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                                 stencil_back_compare_mask);
                }
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

// =========================================================================
// Path A: Threaded command recording
// =========================================================================

void Scheduler::SetThreadedRecording(bool enabled) {
    if (enabled == threaded_recording_) {
        return;
    }
    if (enabled) {
        // End any active render pass before switching modes.
        if (is_rendering) {
            is_rendering = false;
            current_cmdbuf.endRendering();
        }
        parser_dynamic_state_ = dynamic_state;
        // Seed recorder-private state — no render pass active after endRendering above.
        recorder_is_rendering_ = false;
        recorder_render_state_ = RenderState{};
        threaded_recording_ = true;
        LOG_INFO(Render_Vulkan, "Bundle recording enabled (Phase 1B stable)");
    } else {
        threaded_recording_ = false;
    }
}

DrawBundle* Scheduler::AllocateBundle() {
    auto* slot = bundle_ring_->TryStartWrite();
    if (!slot) {
        // Ring full — drain recorder thread and retry.
        DrainRecorderQueue();
        slot = bundle_ring_->TryStartWrite();
        ASSERT_MSG(slot, "DrawBundle ring full after drain");
    }
    slot->has_render_state = false;
    slot->has_dynamic_state = false;
    slot->has_pipeline_bind = false;
    slot->has_push_constants = false;
    slot->has_descriptors = false;
    slot->has_vertex_buffers = false;
    slot->has_index_bind = false;
    slot->has_desc_set_bind = false;
    slot->end_rendering_before_barriers = false;
    slot->num_barriers = 0;
    return slot;
}

void Scheduler::SubmitBundle() {
    // Full-bundle synchronous mode: process inline on parser thread.
    bundle_ring_->FinishWrite();
    auto* bundle = bundle_ring_->TryStartRead();
    ASSERT_MSG(bundle, "No bundle to process after FinishWrite");
    ProcessBundle(*bundle);
    bundle_ring_->FinishRead();
}

void Scheduler::DrainRecorderQueue() {
    // Synchronous mode: all bundles are processed inline, nothing to drain.
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuRecorder");

    while (!stoken.stop_requested()) {
        DrawBundle* bundle = bundle_ring_->TryStartRead();
        if (!bundle) {
            // Nothing to do — sleep until a bundle is submitted.
            std::unique_lock lk{recorder_mutex_};
            recorder_cv_.wait(lk, stoken, [this] {
                return !bundle_ring_->Empty();
            });
            continue;
        }

        ProcessBundle(*bundle);
        bundle_ring_->FinishRead();
    }

    // Final drain on shutdown.
    while (auto* b = bundle_ring_->TryStartRead()) {
        ProcessBundle(*b);
        bundle_ring_->FinishRead();
    }
}

void Scheduler::ProcessBundle(const DrawBundle& bundle) {
    const auto cmdbuf = current_cmdbuf;

    // Use recorder-private state to avoid racing with the parser thread.
    // The parser may read/write is_rendering and render_state (via EndRendering,
    // BeginRendering) while the recorder is processing bundles concurrently.
    auto& rec_rendering = recorder_is_rendering_;
    auto& rec_state = recorder_render_state_;

    // --- Barriers (must happen outside render pass) ---
    if (bundle.num_barriers > 0) {
        if (bundle.end_rendering_before_barriers && rec_rendering) {
            rec_rendering = false;
            cmdbuf.endRendering();
        }
        const auto dependencies = vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = bundle.num_barriers,
            .pBufferMemoryBarriers = bundle.barriers.data(),
        };
        cmdbuf.pipelineBarrier2(dependencies);
    }

    // --- EndRendering command ---
    if (bundle.type == DrawBundle::Type::EndRendering) {
        if (rec_rendering) {
            rec_rendering = false;
            cmdbuf.endRendering();
        }
        return;
    }

    // --- BeginRendering ---
    if (bundle.has_render_state) {
        const auto& new_state = bundle.render_state;
        if (rec_rendering && rec_state == new_state) {
            // Same render pass, skip.
        } else {
            if (rec_rendering) {
                cmdbuf.endRendering();
            }
            rec_rendering = true;
            rec_state = new_state;

            const vk::RenderingInfo rendering_info = {
                .renderArea = {
                    .offset = {0, 0},
                    .extent = {rec_state.width, rec_state.height},
                },
                .layerCount = rec_state.num_layers,
                .colorAttachmentCount = rec_state.num_color_attachments,
                .pColorAttachments = rec_state.num_color_attachments > 0
                                         ? rec_state.color_attachments.data()
                                         : nullptr,
                .pDepthAttachment = rec_state.has_depth
                                        ? &rec_state.depth_attachment : nullptr,
                .pStencilAttachment = rec_state.has_stencil
                                         ? &rec_state.stencil_attachment : nullptr,
            };
            cmdbuf.beginRendering(rendering_info);
        }
    }

    // --- Push constants ---
    if (bundle.has_push_constants) {
        cmdbuf.pushConstants(bundle.pipeline_layout, bundle.push_stage_flags,
                             0u, sizeof(Shader::PushData), &bundle.push_data);
    }

    // --- Descriptor writes (push descriptors) ---
    if (bundle.has_descriptors && bundle.num_desc_writes > 0) {
        cmdbuf.pushDescriptorSetKHR(
            bundle.bind_point, bundle.pipeline_layout, 0,
            vk::ArrayProxy<const vk::WriteDescriptorSet>(
                bundle.num_desc_writes, bundle.desc_writes.data()));
    }

    // --- Descriptor set bind (non-push path) ---
    if (bundle.has_desc_set_bind) {
        cmdbuf.bindDescriptorSets(bundle.bind_point, bundle.pipeline_layout,
                                  0, bundle.desc_set, {});
    }

    // --- Dynamic state + Bind pipeline ---
    // Phase 1B: setters on parser_dynamic_state_ (accumulates across draws),
    // Commit emits dirty bits, BindPipelineCached skips if same pipeline.
    //
    // Pipeline CHANGES: replace + Invalidate (all state before new bind).
    // Pipeline SAME: apply through setters (only real changes produce dirty flags,
    //   accumulated cmdbuf state persists — e.g. stencil write mask from earlier draw).
    {
        const bool pipeline_changed = bundle.has_pipeline_bind &&
                                       bundle.pipeline != recorder_last_pipeline_;

        if (bundle.has_dynamic_state) {
            if (pipeline_changed) {
                dynamic_state = bundle.dynamic_state;
                dynamic_state.Invalidate();
            } else {
                const auto& s = bundle.dynamic_state;
                dynamic_state.SetViewports(s.viewports);
                dynamic_state.SetScissors(s.scissors);
                dynamic_state.SetDepthTestEnabled(s.depth_test_enabled);
                dynamic_state.SetDepthWriteEnabled(s.depth_write_enabled);
                if (s.depth_test_enabled) {
                    dynamic_state.SetDepthCompareOp(s.depth_compare_op);
                }
                dynamic_state.SetDepthBoundsTestEnabled(s.depth_bounds_test_enabled);
                if (s.depth_bounds_test_enabled) {
                    dynamic_state.SetDepthBounds(s.depth_bounds_min, s.depth_bounds_max);
                }
                dynamic_state.SetDepthBiasEnabled(s.depth_bias_enabled);
                if (s.depth_bias_enabled) {
                    dynamic_state.SetDepthBias(s.depth_bias_constant, s.depth_bias_clamp,
                                                s.depth_bias_slope);
                }
                dynamic_state.SetStencilTestEnabled(s.stencil_test_enabled);
                if (s.stencil_test_enabled) {
                    dynamic_state.SetStencilOps(s.stencil_front_ops, s.stencil_back_ops);
                    dynamic_state.SetStencilReferences(s.stencil_front_reference,
                                                        s.stencil_back_reference);
                    dynamic_state.SetStencilWriteMasks(s.stencil_front_write_mask,
                                                       s.stencil_back_write_mask);
                    dynamic_state.SetStencilCompareMasks(s.stencil_front_compare_mask,
                                                         s.stencil_back_compare_mask);
                }
                dynamic_state.SetPrimitiveRestartEnabled(s.primitive_restart_enable);
                dynamic_state.SetRasterizerDiscardEnabled(s.rasterizer_discard_enable);
                dynamic_state.SetCullMode(s.cull_mode);
                dynamic_state.SetFrontFace(s.front_face);
                dynamic_state.SetBlendConstants(s.blend_constants);
                dynamic_state.SetColorWriteMasks(s.color_write_masks);
                dynamic_state.SetLineWidth(s.line_width);
                dynamic_state.SetAttachmentFeedbackLoopEnabled(s.feedback_loop_enabled);
            }
            dynamic_state.Commit(instance, cmdbuf);
        }

        if (pipeline_changed) {
            cmdbuf.bindPipeline(bundle.bind_point, bundle.pipeline);
            recorder_last_pipeline_ = bundle.pipeline;
        }
    }

    // --- Vertex buffers ---
    if (bundle.has_vertex_buffers && bundle.num_vertex_buffers > 0) {
        if (instance.IsVertexInputDynamicState()) {
            cmdbuf.bindVertexBuffers(0, bundle.num_vertex_buffers,
                                      bundle.vb_buffers.data(),
                                      bundle.vb_offsets.data());
        } else {
            cmdbuf.bindVertexBuffers2(0, bundle.num_vertex_buffers,
                                       bundle.vb_buffers.data(),
                                       bundle.vb_offsets.data(),
                                       bundle.vb_sizes.data(),
                                       bundle.vb_strides.data());
        }
    }

    // --- Index buffer ---
    if (bundle.has_index_bind) {
        cmdbuf.bindIndexBuffer(bundle.index_buffer, bundle.index_buffer_offset,
                               bundle.index_type);
    }

    // --- Draw ---
    switch (bundle.type) {
    case DrawBundle::Type::Draw:
        cmdbuf.draw(bundle.num_indices, bundle.num_instances,
                    bundle.vertex_offset, bundle.instance_offset);
        break;
    case DrawBundle::Type::DrawIndexed:
        cmdbuf.drawIndexed(bundle.num_indices, bundle.num_instances, 0,
                           bundle.vertex_offset, bundle.instance_offset);
        break;
    case DrawBundle::Type::DrawIndirect:
        // Indirect draws handled inline for now.
        break;
    case DrawBundle::Type::DrawIndexedIndirect:
        break;
    default:
        break;
    }
}

} // namespace Vulkan
