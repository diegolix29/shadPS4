// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>

#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/bounded_threadsafe_queue.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_command_chunk.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_record_audit.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;
class Scheduler;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    bool operator==(const RenderState& other) const noexcept {
        if (std::memcmp(&width, &other.width, sizeof(width) * 4) != 0 ||
            std::memcmp(&depth_stencil_attachment, &other.depth_stencil_attachment,
                        sizeof(RenderAttachment)) != 0) {
            return false;
        }
        for (u32 index = 0; index < num_color_attachments; ++index) {
            if (std::memcmp(&color_attachments[index], &other.color_attachments[index],
                            sizeof(RenderAttachment)) != 0) {
                return false;
            }
        }
        return true;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 3> signal_semas;
    std::array<u64, 3> signal_ticks;
    vk::Fence fence;
    std::array<vk::PipelineStageFlags, 4> wait_stages;
    u32 num_wait_semas;
    u32 num_signal_semas;
    /// Present id of the frame the submission renders, for NVIDIA Reflex; 0 when untracked.
    u64 latency_present_id;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1,
                 vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas] = tick;
        wait_stages[num_wait_semas++] = stage;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        const u32 different =
            (static_cast<u32>(fail_op) ^ static_cast<u32>(other.fail_op)) |
            (static_cast<u32>(pass_op) ^ static_cast<u32>(other.pass_op)) |
            (static_cast<u32>(depth_fail_op) ^ static_cast<u32>(other.depth_fail_op)) |
            (static_cast<u32>(compare_op) ^ static_cast<u32>(other.compare_op));
        return different == 0;
    }
};
struct DynamicState {
    union {
        struct {
            u32 viewports : 1;
            u32 scissors : 1;

            u32 depth_test_enabled : 1;
            u32 depth_write_enabled : 1;
            u32 depth_compare_op : 1;

            u32 depth_bounds_test_enabled : 1;
            u32 depth_bounds : 1;

            u32 depth_bias_enabled : 1;
            u32 depth_bias : 1;

            u32 stencil_test_enabled : 1;
            u32 stencil_front_ops : 1;
            u32 stencil_front_reference : 1;
            u32 stencil_front_write_mask : 1;
            u32 stencil_front_compare_mask : 1;
            u32 stencil_back_ops : 1;
            u32 stencil_back_reference : 1;
            u32 stencil_back_write_mask : 1;
            u32 stencil_back_compare_mask : 1;

            u32 primitive_restart_enable : 1;
            u32 rasterizer_discard_enable : 1;
            u32 cull_mode : 1;
            u32 front_face : 1;

            u32 blend_constants : 1;
            u32 color_write_masks : 1;
            u32 line_width : 1;
            u32 feedback_loop_enabled : 1;
        } dirty_state;
        u32 dirty_bits{};
    };

    static constexpr u32 AllDirtyBits = (1U << 26) - 1;

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Records the dirty dynamic state into the current command buffer of the scheduler.
    void Commit(const Instance& instance, Scheduler& scheduler);

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        dirty_bits = AllDirtyBits;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetSingleViewportScissor(const vk::Viewport& viewport, const vk::Rect2D& scissor) {
        const bool viewport_different =
            viewports.size() != 1 ||
            ((viewports.front().x != viewport.x) | (viewports.front().y != viewport.y) |
             (viewports.front().width != viewport.width) |
             (viewports.front().height != viewport.height) |
             (viewports.front().minDepth != viewport.minDepth) |
             (viewports.front().maxDepth != viewport.maxDepth));
        if (viewport_different) {
            viewports.clear();
            viewports.push_back(viewport);
            dirty_state.viewports = true;
        }
        if (scissors.size() != 1 ||
            std::memcmp(&scissors.front(), &scissor, sizeof(scissor)) != 0) {
            scissors.clear();
            scissors.push_back(scissor);
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if ((depth_bias_constant != constant) | (depth_bias_clamp != clamp) |
            (depth_bias_slope != slope)) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if ((blend_constants[0] != blend_constants_[0]) |
            (blend_constants[1] != blend_constants_[1]) |
            (blend_constants[2] != blend_constants_[2]) |
            (blend_constants[3] != blend_constants_[3])) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (std::memcmp(color_write_masks.data(), color_write_masks_.data(),
                        sizeof(color_write_masks)) != 0) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

class Scheduler {
public:
    /// With threaded_recording, commands recorded through this scheduler reach the driver on a
    /// dedicated recording thread, which also begins, ends and hands off the command buffers.
    /// The thread that records (the command processor) never touches a Vulkan command buffer.
    /// With presentation, submissions go to the presentation queue.
    explicit Scheduler(const Instance& instance, bool async_submit = false,
                       bool threaded_recording = false, bool presentation = false);
    ~Scheduler();

    /// Makes every submission wait for the guest copies enqueued before it. Staging memory
    /// written by the copy engine is only read by the GPU after the owning submission.
    void GateSubmitsOnGuestCopies() noexcept {
        gate_guest_copies = true;
    }

    /// Tags the following submissions with the present id of the frame they render, so NVIDIA
    /// Reflex can attribute their GPU time. 0 stops tagging.
    void SetLatencyPresentId(u64 present_id) noexcept {
        latency_present_id.store(present_id, std::memory_order_relaxed);
    }

    /// Buffer copies that the commands of a submission depend on.
    struct PrologueCopies {
        vk::Buffer src{};
        vk::Buffer dst{};
        boost::container::small_vector<vk::BufferCopy, 4> regions;
    };
    /// Fills the prologue copies of the command buffer being submitted. Called on the thread that
    /// records commands, after its last command.
    using PrologueCollector = void (*)(void* context, PrologueCopies& copies);

    /// The collected copies run in a command buffer of their own, so they never break a
    /// rendering scope. With a transfer queue, the copy engines run them beside the graphics
    /// work and the dependent submission waits on a semaphore; otherwise they run on the
    /// graphics queue ahead of the command buffer, behind a barrier.
    void SetPrologueCollector(PrologueCollector collector, void* context) noexcept {
        prologue_collector = collector;
        prologue_context = context;
    }

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Waits until the command buffer of the given tick has been handed to the driver.
    void WaitSubmitted(u64 tick) const;

    /// Discards the commands of the current command buffer. Shutdown only, after Finish.
    void ResetCommandBuffer();

    /// Attempts to execute operations whose tick the GPU has caught up with.
    /// Runs the deferred operations the GPU has caught up with. Draws call this constantly, so
    /// unless force is set the GPU progress is only checked every few calls.
    void PopPendingOperations(bool force = false);

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state) {
        if (is_rendering && render_state == new_state) [[likely]] {
            return;
        }
        BeginNewRendering(new_state);
    }

    /// Ends current rendering scope.
    void EndRendering() {
        if (!is_rendering) {
            return;
        }
        EndRenderingScope();
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the recorder of the current command buffer.
    [[nodiscard]] CommandRecorder CommandBuffer() noexcept {
        return CommandRecorder{*this};
    }

    /// Returns the Vulkan command buffer being recorded, for code that has to call the driver
    /// directly. Invalid with a recording thread, which owns the command buffer.
    [[nodiscard]] vk::CommandBuffer RawCommandBuffer() const {
        ASSERT_MSG(!threaded_recording, "The recording thread owns the command buffer");
        return current_cmdbuf;
    }

    /// Returns true when recorded commands reach the driver on the recording thread.
    [[nodiscard]] bool HasRecordingThread() const noexcept {
        return threaded_recording;
    }

    /// Records func(vk::CommandBuffer) into the current command buffer. Without a recording
    /// thread it runs right away. With one it runs later on that thread, so it must hold
    /// everything it reads by value.
    template <typename Func>
    void Record(Func&& func) {
        if (!threaded_recording) {
            func(current_cmdbuf);
            return;
        }
        if (RecordAudit::watch_producers.load(std::memory_order_relaxed)) [[unlikely]] {
            RecordAudit::NoteProducer();
        }
        if (!chunk->Record(func)) [[unlikely]] {
            RecordInNewChunk(func);
        }
    }

    /// Records func(vk::CommandBuffer, const std::byte* payload) together with size bytes of
    /// payload, and returns the payload for the caller to fill before recording anything else.
    /// Only valid with a recording thread.
    template <typename Func>
    [[nodiscard]] std::byte* RecordWithPayload(size_t size, Func&& func) {
        if (RecordAudit::watch_producers.load(std::memory_order_relaxed)) [[unlikely]] {
            RecordAudit::NoteProducer();
        }
        std::byte* payload = chunk->RecordWithPayload(size, func);
        if (payload == nullptr) [[unlikely]] {
            payload = RecordWithPayloadInNewChunk(size, func);
        }
        return payload;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore.CurrentTick();
    }

    /// Returns the last timeline tick known to have completed on the GPU.
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return master_semaphore.KnownGpuTick();
    }

    /// Returns a monotonic epoch incremented whenever graphics push-descriptor state is disturbed
    /// in the guest command buffer. Cached partial pushes use this to reject stale state.
    [[nodiscard]] u64 GraphicsPushDescriptorEpoch() const noexcept {
        return graphics_push_descriptor_epoch;
    }

    /// Records a graphics push-descriptor write in the guest command buffer.
    void NotifyGraphicsPushDescriptorSet() noexcept {
        ++graphics_push_descriptor_epoch;
    }

    /// Returns true when the supplied push-constant bytes differ from the state already emitted
    /// for this bind point in the current command buffer.
    [[nodiscard]] bool UpdatePushConstantCache(bool is_compute, vk::PipelineLayout layout,
                                               const void* data, size_t size) noexcept {
        ASSERT(size <= PushConstantCache::Capacity);
        auto& cache = push_constant_caches[is_compute ? 1U : 0U];
        const u64 tick = CurrentTick();
        if (cache.valid && cache.layout == layout && cache.tick == tick && cache.size == size &&
            std::memcmp(cache.bytes.data(), data, size) == 0) {
            return false;
        }
        cache.valid = true;
        cache.layout = layout;
        cache.tick = tick;
        cache.size = size;
        std::memcpy(cache.bytes.data(), data, size);
        return true;
    }

    /// Binds a graphics pipeline only when it differs from the state already recorded in the
    /// current guest command buffer.
    void BindGraphicsPipeline(vk::Pipeline pipeline) {
        const u64 tick = CurrentTick();
        if (graphics_pipeline_valid && graphics_pipeline_tick == tick &&
            graphics_pipeline == pipeline) {
            return;
        }
        Record([pipeline](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        });
        graphics_pipeline_valid = true;
        graphics_pipeline_tick = tick;
        graphics_pipeline = pipeline;
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (master_semaphore.IsFree(tick)) {
            return true;
        }
        master_semaphore.Refresh();
        return master_semaphore.IsFree(tick);
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore* GetMasterSemaphore() noexcept {
        return &master_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        if (pending_ops.size() == 1) {
            pending_ops_front_tick.store(pending_ops.front().gpu_tick, std::memory_order_release);
        }
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    /// Defers an operation until the gpu has reached gpu_tick, a tick already handed out.
    void DeferPriorityOperationAt(u64 gpu_tick, Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), gpu_tick);
        }
        priority_pending_ops_cv.notify_one();
    }

private:
    /// Command buffer with the prologue copies of a submission.
    struct Prologue {
        vk::CommandBuffer cmdbuf{};
        /// Recorded for the transfer queue; the graphics submission waits for it.
        bool on_transfer_queue{};
    };

    struct SubmitJob {
        SubmitInfo info{};
        /// Runs ahead of cmdbuf when set: in the same submission, or on the transfer queue.
        Prologue prologue{};
        vk::CommandBuffer cmdbuf{};
        u64 guest_copy_seq{};
        u64 signal_tick{};
        /// Hands pending ImGui texture uploads to the queue right before this job.
        bool texture_uploads{};
    };

    /// Command buffer end requested by the command processor, carried with the last chunk.
    struct SubmitRequest {
        SubmitInfo info{};
        u64 signal_tick{};
        u64 guest_copy_seq{};
        PrologueCopies prologue{};
    };

    struct RecordWork {
        std::unique_ptr<CommandChunk> chunk;
        bool submit{};
        SubmitRequest request{};
    };

    void AllocateWorkerCommandBuffers();

    /// Collects the prologue copies of the command buffer being submitted.
    [[nodiscard]] PrologueCopies CollectPrologue();
    /// Records the prologue copies into a command buffer for tick; null when there are none. A
    /// prologue for the transfer queue adds the wait for it to info.
    [[nodiscard]] Prologue RecordPrologue(const PrologueCopies& copies, u64 tick,
                                          SubmitInfo& info);
    /// Submits a transfer queue prologue, which signals the transfer timeline with tick.
    void SubmitTransferPrologue(vk::CommandBuffer cmdbuf, u64 tick);

    void SubmitExecution(SubmitInfo& info);
    void SubmitRecordedExecution(SubmitInfo& info);

    void SubmitThread(std::stop_token stoken);
    void SubmitJobNow(SubmitJob& job);

    void PriorityPendingOpsThread(std::stop_token stoken);

    /// Hands the filled chunk to the recording thread.
    void DispatchWork();
    void BeginNewRendering(const RenderState& new_state);
    void EndRenderingScope();

    /// Records func after the current chunk filled up. Out of line so that recording a command
    /// does not carry the chunk handover.
    template <typename Func>
    SHAD_NO_INLINE void RecordInNewChunk(Func& func) {
        DispatchWork();
        const bool recorded = chunk->Record(func);
        ASSERT(recorded);
    }
    template <typename Func>
    SHAD_NO_INLINE std::byte* RecordWithPayloadInNewChunk(size_t size, Func& func) {
        DispatchWork();
        std::byte* payload = chunk->RecordWithPayload(size, func);
        ASSERT(payload != nullptr);
        return payload;
    }
    void PushWork(RecordWork&& work);
    [[nodiscard]] std::unique_ptr<CommandChunk> TakeChunk();
    /// Waits until the recording thread has replayed every dispatched chunk.
    void WaitRecordIdle();
    void RecordThread(std::stop_token stoken);
    void ExecuteWork(RecordWork& work);
    void SubmitRecorded(const SubmitRequest& request);

private:
    struct PushConstantCache {
        static constexpr size_t Capacity = 128;
        std::array<std::byte, Capacity> bytes{};
        vk::PipelineLayout layout{};
        u64 tick{};
        size_t size{};
        bool valid{};
    };

    const Instance& instance;
    const bool async_submit;
    const vk::Queue queue;
    std::mutex& queue_mutex;
    bool threaded_recording{};
    bool gate_guest_copies{};
    std::atomic<u64> latency_present_id{0};
    PrologueCollector prologue_collector{};
    void* prologue_context{};
    MasterSemaphore master_semaphore;
    CommandPool command_pool;
    /// Prologue command buffers and their completion, when the device has a transfer queue.
    std::unique_ptr<CommandPool> transfer_pool;
    vk::UniqueSemaphore transfer_timeline;
    DynamicState dynamic_state;
    vk::CommandBuffer current_cmdbuf;
    u64 graphics_push_descriptor_epoch{};
    std::array<PushConstantCache, 2> push_constant_caches{};
    vk::Pipeline graphics_pipeline{};
    u64 graphics_pipeline_tick{};
    bool graphics_pipeline_valid{};
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    static constexpr u64 NoPendingOps = ~0ULL;
    /// Tick of the oldest deferred operation, NoPendingOps when there is none. Written under
    /// pending_ops_mutex, read without it.
    std::atomic<u64> pending_ops_front_tick{NoPendingOps};
    /// Earliest time PopPendingOperations may query the driver for the GPU tick again.
    std::atomic<u64> next_pending_ops_poll_ns{};
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    Common::SPSCQueue<SubmitJob, 8> submit_queue;
    std::atomic<u64> submitted_tick{0};
    std::jthread submit_thread;

    /// Chunk being filled by the command processor.
    std::unique_ptr<CommandChunk> chunk;
    std::mutex work_mutex;
    /// Command processor to recording thread.
    std::condition_variable_any work_cv;
    /// Recording thread to command processor.
    std::condition_variable_any work_done_cv;
    std::deque<RecordWork> work_queue;
    u64 dispatched_work{};
    u64 executed_work{};
    std::mutex reserve_mutex;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    /// Command buffer and tick owned by the recording thread.
    vk::CommandBuffer record_cmdbuf{};
    u64 record_tick{};
    std::jthread record_thread;
    RenderState render_state;
    bool is_rendering = false;
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan