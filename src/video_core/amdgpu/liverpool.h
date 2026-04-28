// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <atomic>
#include <coroutine>
#include <exception>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/bounded_threadsafe_queue.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};
    u64 GetGfxPipelineStamp() const noexcept {
        return gfx_pipeline_stamp.load(std::memory_order_relaxed);
    }

    /// Check if the pipeline key needs rebuilding (vs just dynamic state change).
    bool IsGfxKeyDirty() const noexcept {
        return gfx_key_dirty_;
    }

    void ClearGfxKeyDirty() noexcept {
        gfx_key_dirty_ = false;
    }

    /// Check if any dynamic-state register changed since last draw.
    bool IsDynamicDirty() const noexcept {
        return dynamic_dirty_;
    }

    void ClearDynamicDirty() noexcept {
        dynamic_dirty_ = false;
    }

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        mapped_queues[GfxQueueId].ccb_buffer_offset.store(0, std::memory_order_relaxed);
        mapped_queues[GfxQueueId].dcb_buffer_offset.store(0, std::memory_order_relaxed);
        submit_done.store(true, std::memory_order_release);
        NotifyGpu();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{idle_mutex_};
        idle_cv_.wait(lk, [this] {
            return num_submits.load(std::memory_order_acquire) == 0;
        });
    }

    bool IsGpuIdle() const {
        return num_submits == 0;
    }

    [[nodiscard]] u32 GetNumSubmits() const noexcept {
        return num_submits.load(std::memory_order_acquire);
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        if (std::this_thread::get_id() == gpu_id) {
            return func();
        }
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            command_queue_.EmplaceWait([&sem, &func] {
                func();
                sem.release();
            });
            num_commands.fetch_add(1, std::memory_order_release);
            NotifyGpu();
            sem.acquire();
        } else {
            command_queue_.EmplaceWait(std::move(func));
            num_commands.fetch_add(1, std::memory_order_release);
            NotifyGpu();
        }
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);
    void BumpGfxPipelineStamp() noexcept {
        gfx_pipeline_stamp.fetch_add(1, std::memory_order_relaxed);
    }

    /// Mark pipeline state as potentially changed. Actual stamp bump deferred to draw time.
    void MarkGfxPipelineDirty() noexcept {
        pipeline_dirty_ = true;
        dynamic_dirty_ = true;
    }

    /// Mark that a key-affecting register changed (not just dynamic state).
    void MarkGfxKeyDirty() noexcept {
        pipeline_dirty_ = true;
        gfx_key_dirty_ = true;
        dynamic_dirty_ = true;
    }

    /// Bump the stamp only if dirty (called from draw/dispatch handlers).
    void FlushGfxPipelineDirty() noexcept {
        if (pipeline_dirty_) {
            BumpGfxPipelineStamp();
            pipeline_dirty_ = false;
        }
    }

    /// Returns true if the context register range is dynamic-state-only
    /// (viewport, scissor, blend constants, depth control, etc.)
    /// and does NOT affect the GraphicsPipelineKey.
    bool IsDynamicStateOnlyContextReg(u32 reg_addr) const noexcept {
        // Use pointer arithmetic against reg_array to compute word offsets.
        // This avoids offsetof on anonymous union/struct members (non-portable).
        const u32* base = regs.reg_array.data();
        auto wo = [base](const auto& field) noexcept -> u32 {
            return static_cast<u32>(reinterpret_cast<const u32*>(&field) - base);
        };
        auto in_range = [reg_addr](u32 start, u32 end) noexcept -> bool {
            return reg_addr >= start && reg_addr < end;
        };

        if (in_range(wo(regs.depth_bounds_min),
                      wo(regs.depth_clear) + 1)) return true;
        if (in_range(wo(regs.screen_scissor),
                      wo(regs.screen_scissor) + sizeof(regs.screen_scissor) / 4)) return true;
        if (in_range(wo(regs.window_offset),
                      wo(regs.window_scissor) + sizeof(regs.window_scissor) / 4)) return true;
        if (in_range(wo(regs.generic_scissor),
                      wo(regs.generic_scissor) + sizeof(regs.generic_scissor) / 4)) return true;
        if (in_range(wo(regs.viewport_scissors[0]),
                      wo(regs.viewport_depths[0]) + sizeof(regs.viewport_depths) / 4)) return true;
        if (in_range(wo(regs.index_offset),
                      wo(regs.primitive_restart_index) + 1)) return true;
        if (in_range(wo(regs.blend_constants),
                      wo(regs.stencil_ref_back) + sizeof(regs.stencil_ref_back) / 4)) return true;
        if (in_range(wo(regs.viewports[0]),
                      wo(regs.viewports[0]) + sizeof(regs.viewports) / 4)) return true;
        if (in_range(wo(regs.poly_offset),
                      wo(regs.poly_offset) + sizeof(regs.poly_offset) / 4)) return true;
        if (in_range(wo(regs.depth_control),
                      wo(regs.depth_control) + sizeof(regs.depth_control) / 4)) return true;
        if (in_range(wo(regs.viewport_control),
                      wo(regs.viewport_control) + sizeof(regs.viewport_control) / 4)) return true;
        return false;
    }

    void ProcessCommands();
    void Process(std::stop_token stoken);

    void NotifyGpu() {
        {
            std::lock_guard lk{wake_mutex_};
        }
        wake_cv_.notify_one();
    }

    void NotifyIdle() {
        {
            std::lock_guard lk{idle_mutex_};
        }
        idle_cv_.notify_all();
    }

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> submit_count{0};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    std::atomic<u32> num_mapped_queues{1u};

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    std::jthread process_thread{};
    std::atomic<u64> gfx_pipeline_stamp{1};
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};

    Common::MPSCQueue<Common::UniqueFunction<void>, 256> command_queue_;

    std::mutex wake_mutex_;
    std::condition_variable_any wake_cv_;

    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;

    std::thread::id gpu_id;
    s32 curr_qid{-1};
    bool pipeline_dirty_{};
    bool gfx_key_dirty_{};
    bool dynamic_dirty_{};
};

} // namespace AmdGpu
