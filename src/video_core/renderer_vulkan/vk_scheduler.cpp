// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/guest_copy_engine.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_record_audit.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

/// Minimum interval between driver queries for the GPU tick made on behalf of deferred
/// operations. Draws come every few microseconds; a query per draw costs more than the draw.
constexpr u64 PendingOpsPollIntervalNs = 50'000;
/// Draw-path calls between two checks of the GPU progress while an operation waits for it.
constexpr u32 PendingOpsCheckPeriod = 8;

[[nodiscard]] u64 SteadyNowNs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// Chunks the command processor may queue ahead of the recording thread before it waits.
constexpr size_t MaxQueuedWork = 64;
/// Interval between two audit reports of the recording thread.
constexpr u64 AuditReportIntervalNs = 10'000'000'000ULL;

void BeginRenderingCommand(vk::CommandBuffer cmdbuf, const RenderState& state) {
    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < state.num_color_attachments; ++i) {
        const auto& cb = state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {state.width, state.height},
            },
        .layerCount = state.num_layers,
        .colorAttachmentCount = state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };
    cmdbuf.beginRendering(rendering_info);
}

/// Dynamic state commands decided by DynamicState::Commit, with the values they set. The
/// decision happens where the state is tracked, the driver calls wherever commands are replayed.
struct DynamicStateEmit {
    enum Bits : u32 {
        DepthTestEnable = 1U << 0,
        DepthWriteEnable = 1U << 1,
        DepthCompareOp = 1U << 2,
        DepthBoundsTestEnable = 1U << 3,
        DepthBounds = 1U << 4,
        DepthBiasEnable = 1U << 5,
        DepthBias = 1U << 6,
        StencilTestEnable = 1U << 7,
        StencilOpBoth = 1U << 8,
        StencilOpFront = 1U << 9,
        StencilOpBack = 1U << 10,
        StencilReferenceBoth = 1U << 11,
        StencilReferenceFront = 1U << 12,
        StencilReferenceBack = 1U << 13,
        StencilWriteMaskBoth = 1U << 14,
        StencilWriteMaskFront = 1U << 15,
        StencilWriteMaskBack = 1U << 16,
        StencilCompareMaskBoth = 1U << 17,
        StencilCompareMaskFront = 1U << 18,
        StencilCompareMaskBack = 1U << 19,
        PrimitiveRestartEnable = 1U << 20,
        RasterizerDiscardEnable = 1U << 21,
        CullMode = 1U << 22,
        FrontFace = 1U << 23,
        BlendConstants = 1U << 24,
        ColorWriteMask = 1U << 25,
        LineWidth = 1U << 26,
        FeedbackLoop = 1U << 27,
    };

    void Apply(vk::CommandBuffer cmdbuf) const {
        if (bits & DepthTestEnable) {
            cmdbuf.setDepthTestEnable(depth_test_enabled);
        }
        if (bits & DepthWriteEnable) {
            cmdbuf.setDepthWriteEnable(depth_write_enabled);
        }
        if (bits & DepthCompareOp) {
            cmdbuf.setDepthCompareOp(depth_compare_op);
        }
        if (bits & DepthBoundsTestEnable) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
        if (bits & DepthBounds) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
        if (bits & DepthBiasEnable) {
            cmdbuf.setDepthBiasEnable(depth_bias_enabled);
        }
        if (bits & DepthBias) {
            cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
        }
        if (bits & StencilTestEnable) {
            cmdbuf.setStencilTestEnable(stencil_test_enabled);
        }
        const auto set_stencil_op = [&](vk::StencilFaceFlags faces, const StencilOps& ops) {
            cmdbuf.setStencilOp(faces, ops.fail_op, ops.pass_op, ops.depth_fail_op, ops.compare_op);
        };
        if (bits & StencilOpBoth) {
            set_stencil_op(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops);
        }
        if (bits & StencilOpFront) {
            set_stencil_op(vk::StencilFaceFlagBits::eFront, stencil_front_ops);
        }
        if (bits & StencilOpBack) {
            set_stencil_op(vk::StencilFaceFlagBits::eBack, stencil_back_ops);
        }
        if (bits & StencilReferenceBoth) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        }
        if (bits & StencilReferenceFront) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront, stencil_front_reference);
        }
        if (bits & StencilReferenceBack) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
        }
        if (bits & StencilWriteMaskBoth) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        }
        if (bits & StencilWriteMaskFront) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront, stencil_front_write_mask);
        }
        if (bits & StencilWriteMaskBack) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
        }
        if (bits & StencilCompareMaskBoth) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        }
        if (bits & StencilCompareMaskFront) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                         stencil_front_compare_mask);
        }
        if (bits & StencilCompareMaskBack) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack, stencil_back_compare_mask);
        }
        if (bits & PrimitiveRestartEnable) {
            cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
        }
        if (bits & RasterizerDiscardEnable) {
            cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
        }
        if (bits & CullMode) {
            cmdbuf.setCullMode(cull_mode);
        }
        if (bits & FrontFace) {
            cmdbuf.setFrontFace(front_face);
        }
        if (bits & BlendConstants) {
            cmdbuf.setBlendConstants(blend_constants.data());
        }
        if (bits & ColorWriteMask) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
        if (bits & LineWidth) {
            cmdbuf.setLineWidth(line_width);
        }
        if (bits & FeedbackLoop) {
            cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                          ? vk::ImageAspectFlagBits::eColor
                                                          : vk::ImageAspectFlagBits::eNone);
        }
    }

    u32 bits{};
    bool depth_test_enabled{};
    bool depth_write_enabled{};
    bool depth_bounds_test_enabled{};
    bool depth_bias_enabled{};
    bool stencil_test_enabled{};
    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    bool feedback_loop_enabled{};
    vk::CompareOp depth_compare_op{};
    float depth_bounds_min{};
    float depth_bounds_max{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};
    StencilOps stencil_front_ops{};
    StencilOps stencil_back_ops{};
    u32 stencil_front_reference{};
    u32 stencil_back_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_back_write_mask{};
    u32 stencil_front_compare_mask{};
    u32 stencil_back_compare_mask{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};
    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
};

} // namespace

Scheduler::Scheduler(const Instance& instance, bool async_submit, bool threaded_recording_,
                     bool presentation)
    : instance{instance}, async_submit{async_submit},
      queue{presentation ? instance.GetPresentQueue() : instance.GetGraphicsQueue()},
      queue_mutex{presentation ? instance.GetPresentQueueMutex()
                               : instance.GetGraphicsQueueMutex()},
      master_semaphore{instance},
      command_pool{instance, &master_semaphore} {
    if (instance.HasTransferQueue()) {
        transfer_pool = std::make_unique<CommandPool>(instance, &master_semaphore,
                                                      instance.GetTransferQueueFamilyIndex());
        const vk::StructureChain semaphore_chain = {
            vk::SemaphoreCreateInfo{},
            vk::SemaphoreTypeCreateInfo{
                .semaphoreType = vk::SemaphoreType::eTimeline,
                .initialValue = 0,
            },
        };
        auto [semaphore_result, semaphore] =
            instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create transfer semaphore: {}", vk::to_string(semaphore_result));
        transfer_timeline = std::move(semaphore);
    }
    bool gpu_profiling = false;
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
    // Tracy zones wrap the command buffer on the recording side.
    gpu_profiling = true;
#endif
    if (threaded_recording_ && gpu_profiling) {
        // The GPU profiler writes timestamps straight into the command buffer.
        LOG_INFO(Render_Vulkan, "GPU profiling keeps command recording on the calling thread");
    }
    threaded_recording = threaded_recording_ && !gpu_profiling;
    if (threaded_recording) {
        if (RecordAudit::RequestedByEnvironment()) {
            RecordAudit::Install();
        }
        LOG_INFO(Render_Vulkan, "Vulkan commands are recorded on a dedicated thread");
        chunk = TakeChunk();
        record_tick = master_semaphore.CurrentTick();
        dynamic_state.Invalidate();
        record_thread = std::jthread(std::bind_front(&Scheduler::RecordThread, this));
    } else {
        AllocateWorkerCommandBuffers();
    }
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
    if (async_submit) {
        submit_thread = std::jthread(std::bind_front(&Scheduler::SubmitThread, this));
    }
}

Scheduler::~Scheduler() {
    if (threaded_recording) {
        // Everything dispatched gets recorded and handed off; the unfinished command buffer is
        // dropped with the pool.
        WaitRecordIdle();
        record_thread.request_stop();
        record_thread.join();
        if (RecordAudit::IsInstalled()) {
            RecordAudit::LogStats("shutdown");
        }
    }
    if (async_submit) {
        WaitSubmitted(CurrentTick() - 1);
        submit_thread.request_stop();
        submit_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

SHAD_NO_INLINE void Scheduler::BeginNewRendering(const RenderState& new_state) {
    EndRendering();
    is_rendering = true;
    render_state = new_state;
    Record(
        [state = render_state](vk::CommandBuffer cmdbuf) { BeginRenderingCommand(cmdbuf, state); });
}

SHAD_NO_INLINE void Scheduler::EndRenderingScope() {
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void Scheduler::Flush(SubmitInfo& info) {
    // A frame is published before its submission reaches the queue: the presentation thread
    // waits for the submission before it queues work that waits for the frame.
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
        Flush();
    }
    WaitSubmitted(tick);
    master_semaphore.Wait(tick);
}

void Scheduler::WaitSubmitted(u64 tick) const {
    if (!async_submit && !threaded_recording) {
        return;
    }
    u64 submitted = submitted_tick.load(std::memory_order_acquire);
    if (submitted >= tick) {
        return;
    }
    while (submitted < tick) {
        submitted_tick.wait(submitted, std::memory_order_relaxed);
        submitted = submitted_tick.load(std::memory_order_acquire);
    }
}

void Scheduler::SubmitThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:VkQueueSubmit");
    while (!stoken.stop_requested()) {
        SubmitJob job{};
        submit_queue.PopWait(job, stoken);
        if (stoken.stop_requested()) {
            break;
        }
        SubmitJobNow(job);
    }
}

void Scheduler::SubmitJobNow(SubmitJob& job) {
    const bool graphics_prologue = job.prologue.cmdbuf && !job.prologue.on_transfer_queue;
    const std::array cmdbufs{job.prologue.cmdbuf, job.cmdbuf};
    const u32 first_cmdbuf = graphics_prologue ? 0U : 1U;
    const vk::LatencySubmissionPresentIdNV latency_si = {
        .presentID = job.info.latency_present_id,
    };
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .pNext = job.info.latency_present_id != 0 ? &latency_si : nullptr,
        .waitSemaphoreValueCount = job.info.num_wait_semas,
        .pWaitSemaphoreValues = job.info.wait_ticks.data(),
        .signalSemaphoreValueCount = job.info.num_signal_semas,
        .pSignalSemaphoreValues = job.info.signal_ticks.data(),
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = job.info.num_wait_semas,
        .pWaitSemaphores = job.info.wait_semas.data(),
        .pWaitDstStageMask = job.info.wait_stages.data(),
        .commandBufferCount = static_cast<u32>(cmdbufs.size()) - first_cmdbuf,
        .pCommandBuffers = cmdbufs.data() + first_cmdbuf,
        .signalSemaphoreCount = job.info.num_signal_semas,
        .pSignalSemaphores = job.info.signal_semas.data(),
    };
    if (job.guest_copy_seq != 0) {
        // The command buffer reads staging bytes that copy workers may still be writing.
        VideoCore::GuestCopyEngine::Instance().WaitCompleted(job.guest_copy_seq);
    }
    if (job.prologue.on_transfer_queue) {
        SubmitTransferPrologue(job.prologue.cmdbuf, job.signal_tick);
    }
    std::unique_lock lk{queue_mutex};
    if (job.texture_uploads) {
        // Overlay texture uploads use the same queue; keep them ahead of this job like the
        // command processor does when it submits itself.
        ImGui::Core::TextureManager::Submit();
    }
    const auto result = queue.submit(submit_info, job.info.fence);
    ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    lk.unlock();
    submitted_tick.store(job.signal_tick, std::memory_order_release);
    submitted_tick.notify_all();
}

void Scheduler::PopPendingOperations(bool force) {
    const u64 front_tick = pending_ops_front_tick.load(std::memory_order_acquire);
    if (front_tick == NoPendingOps) [[likely]] {
        return;
    }
    // A deferred operation waits for the GPU far longer than the few microseconds between draws.
    static thread_local u32 calls_since_check = 0;
    if (!force && ++calls_since_check < PendingOpsCheckPeriod) {
        return;
    }
    calls_since_check = 0;

    if (!master_semaphore.IsFree(front_tick)) {
        // Waits, submits and the priority operation thread keep the known tick fresh; ask the
        // driver only at a bounded rate.
        const u64 now = SteadyNowNs();
        if (!force && now < next_pending_ops_poll_ns.load(std::memory_order_relaxed)) {
            return;
        }
        next_pending_ops_poll_ns.store(now + PendingOpsPollIntervalNs, std::memory_order_relaxed);
        master_semaphore.Refresh();
        if (!master_semaphore.IsFree(front_tick)) {
            return;
        }
    }

    std::unique_lock lk(pending_ops_mutex);
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
    pending_ops_front_tick.store(pending_ops.empty() ? NoPendingOps : pending_ops.front().gpu_tick,
                                 std::memory_order_release);
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit(master_semaphore.CurrentTick());
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

Scheduler::PrologueCopies Scheduler::CollectPrologue() {
    PrologueCopies copies;
    if (prologue_collector) {
        prologue_collector(prologue_context, copies);
    }
    return copies;
}

Scheduler::Prologue Scheduler::RecordPrologue(const PrologueCopies& copies, u64 tick,
                                               SubmitInfo& info) {
    if (copies.regions.empty()) {
        return {};
    }
    const bool on_transfer_queue = transfer_pool != nullptr;
    const vk::CommandBuffer cmdbuf =
        on_transfer_queue ? transfer_pool->Commit(tick) : command_pool.Commit(tick);
    if (RecordAudit::IsInstalled()) [[unlikely]] {
        RecordAudit::ClaimCommandBuffer(cmdbuf);
    }
    Check(cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));
    cmdbuf.copyBuffer(copies.src, copies.dst,
                      std::span{copies.regions.data(), copies.regions.size()});
    if (on_transfer_queue) {
        Check(cmdbuf.end());
        // The semaphore makes the copies available to everything the submission runs.
        info.AddWait(*transfer_timeline, tick, vk::PipelineStageFlagBits::eAllCommands);
        return {.cmdbuf = cmdbuf, .on_transfer_queue = true};
    }
    // The barrier also orders the copies before every command submitted after this buffer.
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
    Check(cmdbuf.end());
    return {.cmdbuf = cmdbuf};
}

void Scheduler::SubmitTransferPrologue(vk::CommandBuffer cmdbuf, u64 tick) {
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1U,
        .pSignalSemaphoreValues = &tick,
    };
    const vk::Semaphore semaphore = *transfer_timeline;
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .commandBufferCount = 1U,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &semaphore,
    };
    std::scoped_lock lk{instance.GetTransferQueueMutex()};
    const auto result = instance.GetTransferQueue().submit(submit_info, vk::Fence{});
    ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during transfer submit");
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    if (threaded_recording) {
        SubmitRecordedExecution(info);
        return;
    }
    if (async_submit) {
        // TextureManager::Submit uses the same queue directly; submit the previous job first.
        WaitSubmitted(CurrentTick() - 1);
    }
    const bool shares_graphics_queue = &queue_mutex == &instance.GetGraphicsQueueMutex();
    if (!shares_graphics_queue) {
        // Overlay texture uploads go to the graphics queue. Its lock is taken alone: destroying
        // an overlay texture waits for this queue while holding the graphics queue's lock.
        std::scoped_lock graphics_lock{instance.GetGraphicsQueueMutex()};
        ImGui::Core::TextureManager::Submit();
    }
    std::unique_lock lk{queue_mutex, std::defer_lock};
    if (!async_submit) {
        lk.lock();
    }
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
    const Prologue prologue = RecordPrologue(CollectPrologue(), signal_value, info);
    const bool graphics_prologue = prologue.cmdbuf && !prologue.on_transfer_queue;
    const std::array cmdbufs{prologue.cmdbuf, current_cmdbuf};
    const u32 first_cmdbuf = graphics_prologue ? 0U : 1U;

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);
    if (info.latency_present_id == 0) {
        info.latency_present_id = latency_present_id.load(std::memory_order_relaxed);
    }

    // Every staging write recorded into this command buffer has been enqueued by now.
    const u64 guest_copy_seq =
        gate_guest_copies ? VideoCore::GuestCopyEngine::Instance().SubmittedSeq() : 0;

    const vk::LatencySubmissionPresentIdNV latency_si = {
        .presentID = info.latency_present_id,
    };
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .pNext = info.latency_present_id != 0 ? &latency_si : nullptr,
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = info.wait_stages.data(),
        .commandBufferCount = static_cast<u32>(cmdbufs.size()) - first_cmdbuf,
        .pCommandBuffers = cmdbufs.data() + first_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    if (shares_graphics_queue) {
        if (async_submit) {
            lk.lock();
        }
        ImGui::Core::TextureManager::Submit();
        if (async_submit) {
            lk.unlock();
        }
    }
    if (async_submit) {
        submit_queue.EmplaceWait(SubmitJob{.info = info,
                                           .prologue = prologue,
                                           .cmdbuf = current_cmdbuf,
                                           .guest_copy_seq = guest_copy_seq,
                                           .signal_tick = signal_value});
    } else {
        if (guest_copy_seq != 0) {
            VideoCore::GuestCopyEngine::Instance().WaitCompleted(guest_copy_seq);
        }
        if (prologue.on_transfer_queue) {
            SubmitTransferPrologue(prologue.cmdbuf, signal_value);
        }
        const auto submit_result = queue.submit(submit_info, info.fence);
        ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    }

    if (!async_submit) {
        master_semaphore.Refresh();
    }
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations(true);
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    std::vector<PendingOp> ready_ops;
    while (!stoken.stop_requested()) {
        u64 wait_tick = 0;
        ready_ops.clear();
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            wait_tick = priority_pending_ops.front().gpu_tick;
        }

        master_semaphore.Wait(wait_tick);
        if (stoken.stop_requested()) {
            break;
        }

        const u64 completed_tick = master_semaphore.KnownGpuTick();
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            while (!priority_pending_ops.empty() &&
                   priority_pending_ops.front().gpu_tick <= completed_tick) {
                ready_ops.emplace_back(std::move(priority_pending_ops.front()));
                priority_pending_ops.pop();
            }
        }

        for (auto& op : ready_ops) {
            op.callback();
        }
    }
}

void Scheduler::SubmitRecordedExecution(SubmitInfo& info) {
    EndRendering();

    const u64 signal_value = master_semaphore.NextTick();
    info.AddSignal(master_semaphore.Handle(), signal_value);
    if (info.latency_present_id == 0) {
        info.latency_present_id = latency_present_id.load(std::memory_order_relaxed);
    }

    // Every staging write recorded into this command buffer has been enqueued by now.
    const u64 guest_copy_seq =
        gate_guest_copies ? VideoCore::GuestCopyEngine::Instance().SubmittedSeq() : 0;

    PushWork(RecordWork{
        .chunk = std::exchange(chunk, TakeChunk()),
        .submit = true,
        .request =
            {
                .info = info,
                .signal_tick = signal_value,
                .guest_copy_seq = guest_copy_seq,
                .prologue = CollectPrologue(),
            },
    });

    // The next command buffer starts here for everything tracked on this thread.
    dynamic_state.Invalidate();

    // Apply pending operations
    PopPendingOperations(true);
}

std::unique_ptr<CommandChunk> Scheduler::TakeChunk() {
    {
        std::scoped_lock lock{reserve_mutex};
        if (!chunk_reserve.empty()) {
            auto reused = std::move(chunk_reserve.back());
            chunk_reserve.pop_back();
            return reused;
        }
    }
    return std::make_unique<CommandChunk>();
}

void Scheduler::DispatchWork() {
    if (chunk->Empty()) {
        return;
    }
    PushWork(RecordWork{.chunk = std::exchange(chunk, TakeChunk())});
}

void Scheduler::PushWork(RecordWork&& work) {
    {
        std::unique_lock lock{work_mutex};
        if (work_queue.size() >= MaxQueuedWork) [[unlikely]] {
            // The recording thread fell behind; do not run ahead of it without bound.
            work_done_cv.wait(lock, [this] { return work_queue.size() < MaxQueuedWork; });
        }
        work_queue.push_back(std::move(work));
        ++dispatched_work;
    }
    work_cv.notify_one();
}

void Scheduler::WaitRecordIdle() {
    std::unique_lock lock{work_mutex};
    work_done_cv.wait(lock, [this] { return executed_work == dispatched_work; });
}

void Scheduler::ResetCommandBuffer() {
    if (!threaded_recording) {
        Check(current_cmdbuf.reset());
        return;
    }
    chunk->Reset();
    WaitRecordIdle();
    // The recording thread only touches its command buffer when it is given work.
    if (record_cmdbuf) {
        Check(record_cmdbuf.reset());
        record_cmdbuf = vk::CommandBuffer{};
    }
}

void Scheduler::RecordThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:VkRecord");
    // Every submission waits for this thread; like the copy workers, it must not queue behind
    // guest threads.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    u64 next_audit_report = SteadyNowNs() + AuditReportIntervalNs;
    while (true) {
        RecordWork work;
        {
            std::unique_lock lock{work_mutex};
            if (!work_cv.wait(lock, stoken, [this] { return !work_queue.empty(); })) {
                break;
            }
            work = std::move(work_queue.front());
            work_queue.pop_front();
        }
        ExecuteWork(work);
        work.chunk->Reset();
        {
            std::scoped_lock lock{reserve_mutex};
            chunk_reserve.push_back(std::move(work.chunk));
        }
        {
            std::scoped_lock lock{work_mutex};
            ++executed_work;
        }
        work_done_cv.notify_all();
        if (RecordAudit::IsInstalled()) [[unlikely]] {
            const u64 now = SteadyNowNs();
            if (now >= next_audit_report) {
                next_audit_report = now + AuditReportIntervalNs;
                RecordAudit::LogStats("recording thread");
            }
        }
    }
}

void Scheduler::ExecuteWork(RecordWork& work) {
    if (!record_cmdbuf) {
        record_cmdbuf = command_pool.Commit(record_tick);
        if (RecordAudit::IsInstalled()) [[unlikely]] {
            RecordAudit::ClaimCommandBuffer(record_cmdbuf);
        }
        Check(record_cmdbuf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        }));
    }
    work.chunk->ExecuteAll(record_cmdbuf);
    if (work.submit) {
        SubmitRecorded(work.request);
    }
}

void Scheduler::SubmitRecorded(const SubmitRequest& request) {
    ASSERT_MSG(request.signal_tick == record_tick,
               "Recorded command buffer for tick {} ended as tick {}", record_tick,
               request.signal_tick);
    Check(record_cmdbuf.end());
    SubmitInfo info = request.info;
    const Prologue prologue = RecordPrologue(request.prologue, request.signal_tick, info);
    SubmitJob job{
        .info = info,
        .prologue = prologue,
        .cmdbuf = record_cmdbuf,
        .guest_copy_seq = request.guest_copy_seq,
        .signal_tick = request.signal_tick,
        .texture_uploads = true,
    };
    record_cmdbuf = vk::CommandBuffer{};
    ++record_tick;
    if (async_submit) {
        submit_queue.EmplaceWait(std::move(job));
    } else {
        SubmitJobNow(job);
    }
}

void DynamicState::Commit(const Instance& instance, Scheduler& scheduler) {
    if (dirty_bits == 0) [[likely]] {
        return;
    }

    const CommandRecorder cmdbuf = scheduler.CommandBuffer();
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    DynamicStateEmit emit{};
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        emit.bits |= DynamicStateEmit::DepthTestEnable;
        emit.depth_test_enabled = depth_test_enabled;
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        emit.bits |= DynamicStateEmit::DepthWriteEnable;
        emit.depth_write_enabled = depth_write_enabled;
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        emit.bits |= DynamicStateEmit::DepthCompareOp;
        emit.depth_compare_op = depth_compare_op;
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            emit.bits |= DynamicStateEmit::DepthBoundsTestEnable;
            emit.depth_bounds_test_enabled = depth_bounds_test_enabled;
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            emit.bits |= DynamicStateEmit::DepthBounds;
            emit.depth_bounds_min = depth_bounds_min;
            emit.depth_bounds_max = depth_bounds_max;
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        emit.bits |= DynamicStateEmit::DepthBiasEnable;
        emit.depth_bias_enabled = depth_bias_enabled;
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        emit.bits |= DynamicStateEmit::DepthBias;
        emit.depth_bias_constant = depth_bias_constant;
        emit.depth_bias_clamp = depth_bias_clamp;
        emit.depth_bias_slope = depth_bias_slope;
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        emit.bits |= DynamicStateEmit::StencilTestEnable;
        emit.stencil_test_enabled = stencil_test_enabled;
    }
    if (stencil_test_enabled) {
        emit.stencil_front_ops = stencil_front_ops;
        emit.stencil_back_ops = stencil_back_ops;
        emit.stencil_front_reference = stencil_front_reference;
        emit.stencil_back_reference = stencil_back_reference;
        emit.stencil_front_write_mask = stencil_front_write_mask;
        emit.stencil_back_write_mask = stencil_back_write_mask;
        emit.stencil_front_compare_mask = stencil_front_compare_mask;
        emit.stencil_back_compare_mask = stencil_back_compare_mask;
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            emit.bits |= DynamicStateEmit::StencilOpBoth;
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                emit.bits |= DynamicStateEmit::StencilOpFront;
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                emit.bits |= DynamicStateEmit::StencilOpBack;
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            emit.bits |= DynamicStateEmit::StencilReferenceBoth;
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                emit.bits |= DynamicStateEmit::StencilReferenceFront;
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                emit.bits |= DynamicStateEmit::StencilReferenceBack;
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            emit.bits |= DynamicStateEmit::StencilWriteMaskBoth;
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                emit.bits |= DynamicStateEmit::StencilWriteMaskFront;
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                emit.bits |= DynamicStateEmit::StencilWriteMaskBack;
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            emit.bits |= DynamicStateEmit::StencilCompareMaskBoth;
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                emit.bits |= DynamicStateEmit::StencilCompareMaskFront;
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                emit.bits |= DynamicStateEmit::StencilCompareMaskBack;
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        emit.bits |= DynamicStateEmit::PrimitiveRestartEnable;
        emit.primitive_restart_enable = primitive_restart_enable;
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        emit.bits |= DynamicStateEmit::RasterizerDiscardEnable;
        emit.rasterizer_discard_enable = rasterizer_discard_enable;
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        emit.bits |= DynamicStateEmit::CullMode;
        emit.cull_mode = cull_mode;
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        emit.bits |= DynamicStateEmit::FrontFace;
        emit.front_face = front_face;
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        emit.bits |= DynamicStateEmit::BlendConstants;
        emit.blend_constants = blend_constants;
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            emit.bits |= DynamicStateEmit::ColorWriteMask;
            emit.color_write_masks = color_write_masks;
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        emit.bits |= DynamicStateEmit::LineWidth;
        emit.line_width = line_width;
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        emit.bits |= DynamicStateEmit::FeedbackLoop;
        emit.feedback_loop_enabled = feedback_loop_enabled;
    }
    if (emit.bits != 0) {
        scheduler.Record([emit](vk::CommandBuffer cmdbuf) { emit.Apply(cmdbuf); });
    }
}

} // namespace Vulkan