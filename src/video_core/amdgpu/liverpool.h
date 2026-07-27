// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <mutex>
#include <queue>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

#include "common/assert.h"
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

struct PM4CmdEventWriteEop;
struct PM4CmdEventWriteEos;
struct PM4CmdReleaseMem;
union PM4Header;

/// Guest memory a queued completion signal writes.
struct GuestSignalLabel {
    VAddr address{};
    u64 value{};
    u32 num_bytes{};    ///< Zero when the signal writes no memory.
    bool described{};   ///< False when the signal may write memory not described here.
    bool value_known{}; ///< The value lands once the copies before the signal finish.
};

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

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        submit_done = true;
        submit_cv.notify_one();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    bool IsGpuIdle() const {
        return num_submits == 0;
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
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([&sem, &func] {
                    func();
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.emplace(std::move(func));
            ++num_commands;
            submit_cv.notify_one();
        }
    }

    [[nodiscard]] bool IsGpuThread() const noexcept {
        return std::this_thread::get_id() == gpu_id;
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

    [[nodiscard]] u64 GraphicsPipelineGeneration() const noexcept {
        return graphics_pipeline_generation;
    }

    [[nodiscard]] u64 GraphicsStateGeneration() const noexcept {
        return graphics_state_generation;
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
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb, u32 ib_depth = 0);
    Task ProcessCeUpdate(std::span<const u32> ccb, u32 ib_depth = 0);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid, u32 ib_depth = 0);

    void ProcessCommands();
    void Process(std::stop_token stoken);
    template <u32 NumWords>
    bool WriteGraphicsRegistersSmall(u32 first_register, const u32* payload);
    bool WriteGraphicsRegisters(u32 first_register, const u32* payload, u32 word_count);
    bool WriteGraphicsRegisters4(u32 first_register, const u32* payload);
    bool WriteGraphicsRegisters8(u32 first_register, const u32* payload);
    bool WriteGraphicsRegistersSlow(u32 first_register, const u32* payload, u32 word_count);
    void HandleContextRegisterHint(u32 register_address, u32 packet_count, const u32* payload);
    bool TryPromoteGoW3Eos(const PM4CmdEventWriteEos& packet);
    void ProcessGraphicsEventWrite(const PM4Header* header);
    void ProcessEventWriteEop(const PM4CmdEventWriteEop& packet);
    void ProcessEventWriteEos(const PM4CmdEventWriteEos& packet);
    void ProcessComputeReleaseMem(const PM4CmdReleaseMem& packet, const u32* queue_pipe_id);

    bool TrackDeferredGpuCompletion(u32 queue_id, VAddr address = 0, u64 value = 0,
                                    u32 num_bytes = 0);
    bool TryBypassGpuCompletionWait(u32 queue_id, VAddr address, u32 function, u32 mask,
                                    u32 reference);
    void FlushPendingGpuCompletionsForWait();
    void RefreshPendingGpuCompletions();

    /// Runs a completion signal once guest copy jobs up to guest_copy_seq have read the memory
    /// parsed before it. Signals run in order, on this thread.
    void SignalAfterGuestReads(u64 guest_copy_seq, Common::UniqueFunction<void>&& signal,
                               const GuestSignalLabel& label = {});
    /// Runs the queued signals whose copies have completed.
    void PollGuestReadSignals();
    /// Waits for the copies of every queued signal and runs them.
    void FlushGuestReadSignals();
    /// Defers a signal to the completion of the GPU work recorded so far, behind the queued
    /// signals, so that signals still land in command stream order.
    void DeferGpuCompletionInOrder(Common::UniqueFunction<void>&& callback,
                                   const GuestSignalLabel& label = {});
    /// Whether the queued signals leave the label at address with a value that passes a
    /// WAIT_REG_MEM. The command stream may then run on while their copies finish.
    bool SkipWaitForQueuedSignal(VAddr address, u32 function, u32 mask, u32 reference);
    /// The command processor is about to write guest memory the guest reads. After a wait
    /// skipped a queued signal, the write must not land before that signal.
    void OrderAfterSkippedSignals() {
        if (skipped_signal_count != 0) [[unlikely]] {
            FlushSkippedSignals();
        }
    }
    void FlushSkippedSignals();

    bool ArmMemoryWait(u32 queue_id, VAddr address);
    void CancelMemoryWait(u32 queue_id);
    void WakeMemoryWait(u32 queue_id) noexcept;
    void ReleaseMemoryWaitFallbacks() noexcept;

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    std::array<Task::Handle, NumTotalQueues> active_tasks{};
    u32 num_mapped_queues{1u}; // GFX is always available

    struct MemoryWaitContext {
        Liverpool* owner{};
        u32 queue_id{};
        VAddr page{};
        u64 id{};
        u64 epoch{};
    };
    std::array<MemoryWaitContext, NumTotalQueues> memory_waits{};

    struct GuestReadSignal {
        u64 guest_copy_seq{};
        Common::UniqueFunction<void> signal;
        GuestSignalLabel label{};
        bool skipped{}; ///< A wait ran on without it.
    };
    std::deque<GuestReadSignal> guest_read_signals;
    u32 skipped_signal_count{};
    std::atomic<u64> ready_queue_mask{};
    std::atomic<u64> blocked_queue_mask{};

    struct PendingGpuFenceWord {
        VAddr address{};
        u32 value{};
        u8 queue_id{};
        bool barriered{};
    };
    static_assert(sizeof(PendingGpuFenceWord) == 16);
    static constexpr u32 MaxPendingGpuFenceWords = 64;
    std::array<PendingGpuFenceWord, MaxPendingGpuFenceWords> pending_gpu_fence_words{};
    u64 pending_gpu_completion_tick{};
    u32 pending_gpu_completion_count{};
    u32 pending_gpu_fence_word_count{};

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};
    u64 graphics_pipeline_generation{1};
    u64 graphics_state_generation{1};
    bool warned_set_predication{};

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
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::thread::id gpu_id;
    s32 curr_qid{-1};
};

} // namespace AmdGpu
