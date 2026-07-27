// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/preprocessor/stringize.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <immintrin.h>
#include <utility>

#include "common/assert.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/hash.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/gpu_authority_tracker.h"
#include "video_core/guest_copy_engine.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace AmdGpu {

namespace {

/// The command processor is about to write guest memory that deferred copies may still read.
inline void PrepareGuestWrite(VAddr address, u64 size) {
    VideoCore::GuestCopyEngine::Instance().WaitForGuestWrite(address, size);
}

inline void PrepareGuestWrite(const void* address, u64 size) {
    PrepareGuestWrite(std::bit_cast<VAddr>(address), size);
}

/// Everything parsed before a completion signal must have finished reading guest memory before
/// the guest can observe the signal and recycle that memory.
inline void CompleteGuestReads(u64 guest_copy_seq) {
    VideoCore::GuestCopyEngine::Instance().WaitCompleted(guest_copy_seq);
}

inline void CompleteGuestReads() {
    VideoCore::GuestCopyEngine::Instance().Drain();
}

[[nodiscard]] inline u64 GuestCopySeq() {
    return VideoCore::GuestCopyEngine::Instance().SubmittedSeq();
}

struct GraphicsRegisterRange {
    u32 first;
    u32 last;
};

#define GRAPHICS_REG_RANGE(field)                                                                  \
    GraphicsRegisterRange {                                                                        \
        static_cast<u32>(offsetof(Regs, field) / sizeof(u32)),                                     \
            static_cast<u32>((offsetof(Regs, field) + sizeof(((Regs*)nullptr)->field)) /           \
                             sizeof(u32))                                                          \
    }

constexpr std::array GraphicsPipelineRegisterRanges = {
    GraphicsRegisterRange{
        static_cast<u32>(offsetof(Regs, ps_program) / sizeof(u32)),
        static_cast<u32>((offsetof(Regs, ps_program) + offsetof(ShaderProgram, user_data)) /
                         sizeof(u32))},
    GraphicsRegisterRange{
        static_cast<u32>(offsetof(Regs, vs_program) / sizeof(u32)),
        static_cast<u32>((offsetof(Regs, vs_program) + offsetof(ShaderProgram, user_data)) /
                         sizeof(u32))},
    GraphicsRegisterRange{
        static_cast<u32>(offsetof(Regs, gs_program) / sizeof(u32)),
        static_cast<u32>((offsetof(Regs, gs_program) + offsetof(ShaderProgram, user_data)) /
                         sizeof(u32))},
    GraphicsRegisterRange{
        static_cast<u32>(offsetof(Regs, es_program) / sizeof(u32)),
        static_cast<u32>((offsetof(Regs, es_program) + offsetof(ShaderProgram, user_data)) /
                         sizeof(u32))},
    GraphicsRegisterRange{
        static_cast<u32>(offsetof(Regs, hs_program) / sizeof(u32)),
        static_cast<u32>((offsetof(Regs, hs_program) + offsetof(ShaderProgram, user_data)) /
                         sizeof(u32))},
    GraphicsRegisterRange{
        static_cast<u32>(offsetof(Regs, ls_program) / sizeof(u32)),
        static_cast<u32>((offsetof(Regs, ls_program) + offsetof(ShaderProgram, user_data)) /
                         sizeof(u32))},
    GRAPHICS_REG_RANGE(depth_buffer),
    GRAPHICS_REG_RANGE(depth_render_override),
    GRAPHICS_REG_RANGE(clipper_control),
    GRAPHICS_REG_RANGE(polygon_control),
    GRAPHICS_REG_RANGE(color_control),
    GRAPHICS_REG_RANGE(color_shader_mask),
    GRAPHICS_REG_RANGE(color_target_mask),
    GRAPHICS_REG_RANGE(color_export_format),
    GRAPHICS_REG_RANGE(color_buffers),
    GRAPHICS_REG_RANGE(blend_control),
    GRAPHICS_REG_RANGE(stage_enable),
    GRAPHICS_REG_RANGE(vs_output_control),
    GRAPHICS_REG_RANGE(vs_output_config),
    GRAPHICS_REG_RANGE(shader_pos_format),
    GRAPHICS_REG_RANGE(vgt_instance_step_rate_0),
    GRAPHICS_REG_RANGE(vgt_instance_step_rate_1),
    GRAPHICS_REG_RANGE(vgt_esgs_ring_itemsize),
    GRAPHICS_REG_RANGE(vgt_gsvs_ring_itemsize),
    GRAPHICS_REG_RANGE(ls_hs_config),
    GRAPHICS_REG_RANGE(tess_config),
    GraphicsRegisterRange{static_cast<u32>(offsetof(Regs, stage_enable) / sizeof(u32) - 7),
                          static_cast<u32>(offsetof(Regs, stage_enable) / sizeof(u32) - 6)},
    GRAPHICS_REG_RANGE(vgt_gs_instance_cnt),
    GRAPHICS_REG_RANGE(vgt_gs_out_prim_type),
    GRAPHICS_REG_RANGE(vgt_gs_vert_itemsize),
    GRAPHICS_REG_RANGE(vgt_gs_mode),
    GRAPHICS_REG_RANGE(vgt_strmout_config),
    GRAPHICS_REG_RANGE(ps_input_ena),
    GraphicsRegisterRange{static_cast<u32>(offsetof(Regs, ps_input_addr) / sizeof(u32)),
                          static_cast<u32>((offsetof(Regs, z_export_format) +
                                            sizeof(((Regs*)nullptr)->z_export_format)) /
                                           sizeof(u32))},
    GRAPHICS_REG_RANGE(depth_shader_control),
    GRAPHICS_REG_RANGE(ps_inputs),
    GRAPHICS_REG_RANGE(primitive_type),
};

#undef GRAPHICS_REG_RANGE

consteval auto BuildGraphicsPipelineRegisterMask() {
    std::array<u64, (Regs::NumRegs + 63) / 64> mask{};
    for (const auto& range : GraphicsPipelineRegisterRanges) {
        for (u32 index = range.first; index < range.last; ++index) {
            mask[index / 64] |= 1ULL << (index % 64);
        }
    }
    return mask;
}

constexpr auto GraphicsPipelineRegisterMask = BuildGraphicsPipelineRegisterMask();
constexpr auto MemoryWaitFallbackInterval = std::chrono::microseconds{250};

template <typename Condvar, typename Lock, typename Rep, typename Period, typename Pred>
#if defined(__clang__) || defined(__GNUC__)
[[gnu::cold]]
#endif
SHAD_NO_INLINE bool WaitMemoryFallback(
    Condvar& cv, std::unique_lock<Lock>& lk, std::stop_token stoken,
    const std::chrono::duration<Rep, Period>& timeout, Pred&& pred) {
    return cv.wait_for(lk, stoken, timeout, std::forward<Pred>(pred));
}

template <u32 WordCount>
[[nodiscard]] inline u32 PipelineRegisterBits(u32 first_register) {
    static_assert(WordCount <= 32);
    const u32 bit_offset = first_register & 63U;
    const u32 mask_index = first_register / 64;
    u64 bits = GraphicsPipelineRegisterMask[mask_index] >> bit_offset;
    if (bit_offset > 64 - WordCount) [[unlikely]] {
        bits |= GraphicsPipelineRegisterMask[mask_index + 1] << (64 - bit_offset);
    }
    return static_cast<u32>(bits);
}

[[nodiscard]] u32 NextReadyQueue(u64 ready_mask, s32 current_queue) {
    ASSERT(ready_mask != 0);
    if (std::has_single_bit(ready_mask)) [[likely]] {
        return std::countr_zero(ready_mask);
    }
    const u32 first_queue = static_cast<u32>(current_queue + 1);
    const u64 queues_after_current = ready_mask & (~0ULL << first_queue);
    return std::countr_zero(queues_after_current != 0 ? queues_after_current : ready_mask);
}

static SHAD_NO_INLINE bool InvalidGraphicsRegisterRange(u32 first_register, u32 word_count) {
    ASSERT(first_register <= Regs::NumRegs && word_count <= Regs::NumRegs - first_register);
    return false;
}

static SHAD_NO_INLINE void WriteComputeProgramRegisters(ComputeProgram& program,
                                                        u32 register_offset, const u32* payload,
                                                        u32 word_count) {
    const size_t byte_count = static_cast<size_t>(word_count) * sizeof(u32);
    ASSERT(byte_count <= sizeof(ComputeProgram));
    auto* destination = reinterpret_cast<u32*>(&program) + (register_offset - 0x200);
    std::memcpy(destination, payload, byte_count);
}

static SHAD_NO_INLINE void GraphicsPacketAssertionFailed() {
    ASSERT(false);
}

[[noreturn]] static SHAD_NO_INLINE void InvalidDmaData(const PM4DmaData& packet) {
    UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(packet.src_sel.Value()),
                    u32(packet.dst_sel.Value()));
}

[[noreturn]] static SHAD_NO_INLINE void UnsupportedWriteDataAddressMode() {
    UNREACHABLE();
}

static SHAD_NO_INLINE void WarnStrmoutBufferUpdate(const PM4CmdStrmoutBufferUpdate& packet) {
    LOG_WARNING(Render_Vulkan,
                "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                "source_select = {}, buffer_select = {}",
                packet.update_memory.Value(), magic_enum::enum_name(packet.source_select.Value()),
                packet.buffer_select.Value());
}

static SHAD_NO_INLINE void WarnGetLodStats() {
    LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
}

static SHAD_NO_INLINE void WarnReservedCondExec() {
    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
}

static SHAD_NO_INLINE void WarnSetPredication() {
    LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION");
}

static SHAD_NO_INLINE void WarnCopyData(const PM4CmdCopyData& packet) {
    LOG_WARNING(Render,
                "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                u32(packet.src_sel.Value()), u32(packet.dst_sel.Value()),
                packet.count_sel.Value(), packet.wr_confirm.Value(),
                u32(packet.engine_sel.Value()));
}

static SHAD_NO_INLINE void BeginHostMarker(Vulkan::Rasterizer& rasterizer,
                                           const void* command_address,
                                           std::string_view command_name) {
    rasterizer.ScopeMarkerBegin(fmt::format("gfx:{}:{}", command_address, command_name));
}

[[noreturn]] static SHAD_NO_INLINE void UnknownType3Opcode(PM4ItOpcode opcode, u32 count) {
    UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}", static_cast<u32>(opcode),
                    count);
}

[[nodiscard]] inline bool TestWaitValue(u32 value, PM4CmdWaitRegMem::Function function, u32 mask,
                                        u32 reference) {
    if (mask == 0xffffffffU) [[likely]] {
        if (function == PM4CmdWaitRegMem::Function::NotEqual) [[likely]] {
            return value != reference;
        }
        if (function == PM4CmdWaitRegMem::Function::Equal) {
            return value == reference;
        }
    }
    return PM4CmdWaitRegMem::TestValue(value, function, mask, reference);
}

} // namespace

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define COUNT_FAILED_MEMORY_WAIT() static_cast<void>(0)

#define WAIT_MEMORY(queue_id, address, condition, yield_command)                                  \
    do {                                                                                           \
        while (!(condition)) {                                                                     \
            COUNT_FAILED_MEMORY_WAIT();                                                            \
            const bool memory_watch_armed =                                                       \
                ArmMemoryWait(queue_id, reinterpret_cast<VAddr>(address));                         \
            if (memory_watch_armed && (condition)) {                                               \
                CancelMemoryWait(queue_id);                                                        \
                break;                                                                             \
            }                                                                                      \
            yield_command                                                                          \
            if (memory_watch_armed) {                                                              \
                CancelMemoryWait(queue_id);                                                        \
            }                                                                                      \
        }                                                                                          \
    } while (false)

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static SHAD_NO_INLINE std::span<const u32> InvalidNextPacket(std::span<const u32> span,
                                                             size_t offset) {
    LOG_ERROR(Lib_GnmDriver,
              ": packet length exceeds remaining submission size. Packet dword count={}, "
              "remaining submission dwords={}",
              offset, span.size());
    return {};
}

static inline std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset <= span.size()) [[likely]] {
        return span.subspan(offset);
    }
    return InvalidNextPacket(span, offset);
}

static SHAD_NO_INLINE std::span<const u32> NextNonType3Packet(std::span<const u32> dcb, u32 type) {
    const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
    switch (type) {
    case 0:
        UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                        header->type0.base.Value(), header->type0.NumWords());
    case 2:
        // Type-2 packets are used for padding purposes
        return NextPacket(dcb, 1);
    default:
        UNREACHABLE_MSG("Wrong PM4 type {}", type);
    }
}

Liverpool::Liverpool() {
    for (u32 queue_id = 0; queue_id < NumTotalQueues; ++queue_id) {
        memory_waits[queue_id].owner = this;
        memory_waits[queue_id].queue_id = queue_id;
    }
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    submit_cv.notify_all();
    process_thread.join();
    for (u32 queue_id = 0; queue_id < NumTotalQueues; ++queue_id) {
        CancelMemoryWait(queue_id);
    }
}

bool Liverpool::ArmMemoryWait(u32 queue_id, VAddr address) {
    if (rasterizer == nullptr) [[unlikely]] {
        return false;
    }

    auto& wait = memory_waits[queue_id];
    ASSERT(wait.id == 0);
    const u64 queue_bit = 1ULL << queue_id;
    blocked_queue_mask.fetch_or(queue_bit, std::memory_order_acq_rel);
    ready_queue_mask.fetch_and(~queue_bit, std::memory_order_acq_rel);

    const auto watch = rasterizer->ArmMemoryWriteWatch(
        address,
        [](void* user_data, VAddr, u64, VideoCore::MemoryWriteSource) noexcept {
            auto& context = *static_cast<MemoryWaitContext*>(user_data);
            context.owner->WakeMemoryWait(context.queue_id);
        },
        &wait);
    if (!watch) [[unlikely]] {
        WakeMemoryWait(queue_id);
        return false;
    }
    wait.page = watch.page;
    wait.id = watch.id;
    wait.epoch = watch.epoch;
    return true;
}

void Liverpool::CancelMemoryWait(u32 queue_id) {
    auto& wait = memory_waits[queue_id];
    if (wait.id == 0) {
        return;
    }

    const VideoCore::MemoryWriteWatch watch{
        .page = wait.page,
        .id = wait.id,
        .epoch = wait.epoch,
    };
    wait.id = 0;
    if (rasterizer != nullptr) {
        rasterizer->CancelMemoryWriteWatch(watch);
    }
    WakeMemoryWait(queue_id);
}

void Liverpool::WakeMemoryWait(u32 queue_id) noexcept {
    const u64 queue_bit = 1ULL << queue_id;
    blocked_queue_mask.fetch_and(~queue_bit, std::memory_order_acq_rel);
    ready_queue_mask.fetch_or(queue_bit, std::memory_order_release);
    submit_cv.notify_one();
}

void Liverpool::ReleaseMemoryWaitFallbacks() noexcept {
    const u64 blocked = blocked_queue_mask.exchange(0, std::memory_order_acq_rel);
    if (blocked == 0) {
        return;
    }
    ready_queue_mask.fetch_or(blocked, std::memory_order_release);
}

void Liverpool::ProcessCommands() {
    // Process incoming commands with high priority
    while (num_commands.load(std::memory_order_acquire) != 0) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    gpu_id = std::this_thread::get_id();
    VideoCore::GuestCopyEngine::Instance().SetProducerThread();
    curr_qid = -1;

    while (!stoken.stop_requested()) {
        // Nothing else runs while this thread blocks, and the guest or a memory wait may be
        // waiting for a queued signal.
        FlushGuestReadSignals();
        bool memory_wait_fallback{};
        {
            std::unique_lock lk{submit_mutex};
            const auto has_ready_work = [this] {
                return num_commands.load(std::memory_order_acquire) != 0 ||
                       ready_queue_mask.load(std::memory_order_acquire) != 0 ||
                       (submit_done.load(std::memory_order_acquire) &&
                        num_submits.load(std::memory_order_acquire) == 0);
            };
            if (num_submits.load(std::memory_order_acquire) != 0 &&
                ready_queue_mask.load(std::memory_order_acquire) == 0) {
                memory_wait_fallback = !WaitMemoryFallback(
                    submit_cv, lk, stoken, MemoryWaitFallbackInterval, has_ready_work);
            } else {
                Common::CondvarWait(submit_cv, lk, stoken, has_ready_work);
            }
        }
        if (stoken.stop_requested()) {
            break;
        }

        if (memory_wait_fallback) {
            ReleaseMemoryWaitFallbacks();
        }
        VideoCore::StartCapture();

        for (;;) {
            if (num_commands.load(std::memory_order_acquire) != 0) [[unlikely]] {
                ProcessCommands();
            }

            const u64 ready_queues = ready_queue_mask.load(std::memory_order_acquire);
            if (ready_queues == 0) {
                break;
            }
            curr_qid = static_cast<s32>(NextReadyQueue(ready_queues, curr_qid));

            Task::Handle task = active_tasks[curr_qid];
            if (!task) [[unlikely]] {
                auto& queue = mapped_queues[curr_qid];
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    ready_queue_mask.fetch_and(~(1ULL << curr_qid), std::memory_order_acq_rel);
                    continue;
                }
                task = queue.submits.front();
                active_tasks[curr_qid] = task;
            }
            {
                task.resume();
            }

            if (task.done()) [[unlikely]] {
                active_tasks[curr_qid] = {};
                task.destroy();

                {
                    auto& queue = mapped_queues[curr_qid];
                    std::scoped_lock lock{queue.m_access};
                    queue.submits.pop();
                    const u64 queue_bit = 1ULL << curr_qid;
                    if (queue.submits.empty()) {
                        ready_queue_mask.fetch_and(~queue_bit, std::memory_order_acq_rel);
                    } else {
                        ready_queue_mask.fetch_or(queue_bit, std::memory_order_release);
                    }
                }

                // WaitGpuIdle and IsGpuIdle let the guest treat a retired submit as consumed.
                CompleteGuestReads();
                FlushGuestReadSignals();
                {
                    std::scoped_lock lock{submit_mutex};
                    --num_submits;
                    submit_cv.notify_all();
                }
            }
        }

        if (num_submits.load(std::memory_order_acquire) == 0) {
            if (submit_done) {
                VideoCore::EndCapture();
                if (rasterizer) {
                    rasterizer->OnSubmit();
                    rasterizer->Flush();
                }
                submit_done = false;
            }
            CompleteGuestReads();
            FlushGuestReadSignals();
            Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
        }
    }
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb, u32 ib_depth) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        if (num_commands.load(std::memory_order_acquire) != 0) [[unlikely]] {
            ProcessCommands();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            OrderAfterSkippedSignals();
            PrepareGuestWrite(dump_const->Address<void*>(), dump_const->Size());
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            if (rasterizer) {
                rasterizer->NotifyMemoryWrite(std::bit_cast<VAddr>(dump_const->Address<void*>()),
                                              dump_const->Size(),
                                              VideoCore::MemoryWriteSource::CommandProcessor);
            }
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size},
                                ib_depth + 1);
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

#ifdef _MSC_VER
#define SHAD_LOCAL_FORCE_INLINE __forceinline
#else
#define SHAD_LOCAL_FORCE_INLINE __attribute__((always_inline)) inline
#endif

template <u32 NumWords>
SHAD_LOCAL_FORCE_INLINE bool Liverpool::WriteGraphicsRegistersSmall(u32 first_register,
                                                                     const u32* payload) {
    static_assert(NumWords == 1 || NumWords == 2);
    if (first_register > Regs::NumRegs - NumWords) [[unlikely]] {
        return InvalidGraphicsRegisterRange(first_register, NumWords);
    }

    u32* const destination = &regs.reg_array[first_register];
    if constexpr (NumWords == 1) {
        const u32 new_value = *payload;
        if (*destination == new_value) [[likely]] {
            return false;
        }
        const bool pipeline_state_changed = PipelineRegisterBits<1>(first_register) != 0;
        *destination = new_value;
        ++graphics_state_generation;
        graphics_pipeline_generation += pipeline_state_changed;
    } else {
        u64 old_values{};
        u64 new_values{};
        std::memcpy(&old_values, destination, sizeof(old_values));
        std::memcpy(&new_values, payload, sizeof(new_values));
        const u64 different = old_values ^ new_values;
        if (different == 0) [[likely]] {
            return false;
        }
        const u32 changed_registers = static_cast<u32>(static_cast<u32>(different) != 0) |
                                      (static_cast<u32>(different >> 32) != 0) << 1;
        const bool pipeline_state_changed =
            (changed_registers & PipelineRegisterBits<2>(first_register)) != 0;
        std::memcpy(destination, &new_values, sizeof(new_values));
        ++graphics_state_generation;
        graphics_pipeline_generation += pipeline_state_changed;
    }
    return true;
}

SHAD_LOCAL_FORCE_INLINE bool Liverpool::WriteGraphicsRegisters(u32 first_register,
                                                                const u32* payload,
                                                                u32 word_count) {
    if (word_count == 1) [[likely]] {
        return WriteGraphicsRegistersSmall<1>(first_register, payload);
    }
    if (word_count == 2) {
        return WriteGraphicsRegistersSmall<2>(first_register, payload);
    }

    if (first_register > Regs::NumRegs || word_count > Regs::NumRegs - first_register) [[unlikely]] {
        return InvalidGraphicsRegisterRange(first_register, word_count);
    }
    if (word_count == 0) {
        return false;
    }
    return WriteGraphicsRegistersSlow(first_register, payload, word_count);
}

SHAD_LOCAL_FORCE_INLINE bool Liverpool::WriteGraphicsRegisters4(u32 first_register,
                                                                 const u32* payload) {
    if (first_register > Regs::NumRegs - 4) [[unlikely]] {
        return InvalidGraphicsRegisterRange(first_register, 4);
    }

    u32* const destination = &regs.reg_array[first_register];
    const auto old_values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination));
    const auto new_values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(payload));
    const u32 equal_registers = static_cast<u32>(
        _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(old_values, new_values))));
    const u32 changed_registers = (~equal_registers) & 0xf;
    if (changed_registers == 0) [[likely]] {
        return false;
    }

    const bool pipeline_state_changed =
        (changed_registers & PipelineRegisterBits<4>(first_register)) != 0;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), new_values);
    ++graphics_state_generation;
    graphics_pipeline_generation += pipeline_state_changed;
    return true;
}

SHAD_NO_INLINE bool Liverpool::WriteGraphicsRegisters8(u32 first_register, const u32* payload) {
    if (first_register > Regs::NumRegs - 8) [[unlikely]] {
        return InvalidGraphicsRegisterRange(first_register, 8);
    }

    u32* const destination = &regs.reg_array[first_register];
    const auto old_values =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination));
    const auto new_values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(payload));
    const u32 equal_registers = static_cast<u32>(
        _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(old_values, new_values))));
    const u32 changed_registers = (~equal_registers) & 0xff;
    if (changed_registers == 0) [[likely]] {
        return false;
    }

    const bool pipeline_state_changed =
        (changed_registers & PipelineRegisterBits<8>(first_register)) != 0;
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination), new_values);
    ++graphics_state_generation;
    graphics_pipeline_generation += pipeline_state_changed;
    return true;
}

#undef SHAD_LOCAL_FORCE_INLINE

SHAD_NO_INLINE bool Liverpool::WriteGraphicsRegistersSlow(u32 first_register, const u32* payload,
                                                           u32 word_count) {
    u32* const destination = &regs.reg_array[first_register];
    const size_t byte_count = static_cast<size_t>(word_count) * sizeof(u32);
    if (std::memcmp(destination, payload, byte_count) == 0) {
        return false;
    }

    bool pipeline_state_changed = false;
    u32 processed = 0;
    while (processed < word_count && !pipeline_state_changed) {
        const u32 register_index = first_register + processed;
        const u32 bit_offset = register_index & 63U;
        const u32 chunk_size = std::min<u32>(word_count - processed, 64U - bit_offset);
        u64 relevant = GraphicsPipelineRegisterMask[register_index / 64] >> bit_offset;
        if (chunk_size != 64) {
            relevant &= (1ULL << chunk_size) - 1;
        }
        while (relevant != 0) {
            const u32 offset = processed + std::countr_zero(relevant);
            if (destination[offset] != payload[offset]) {
                pipeline_state_changed = true;
                break;
            }
            relevant &= relevant - 1;
        }
        processed += chunk_size;
    }

    std::memcpy(destination, payload, byte_count);
    ++graphics_state_generation;
    if (pipeline_state_changed) {
        ++graphics_pipeline_generation;
    }
    return true;
}

SHAD_NO_INLINE void Liverpool::HandleContextRegisterHint(u32 register_address, u32 packet_count,
                                                          const u32* payload) {
    switch (register_address) {
    case ContextRegs::CbColor0Base:
    case ContextRegs::CbColor1Base:
    case ContextRegs::CbColor2Base:
    case ContextRegs::CbColor3Base:
    case ContextRegs::CbColor4Base:
    case ContextRegs::CbColor5Base:
    case ContextRegs::CbColor6Base:
    case ContextRegs::CbColor7Base: {
        const auto col_buf_id = (register_address - ContextRegs::CbColor0Base) /
                                (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
        ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

        if (packet_count == 0x0e || packet_count == 0x0d || packet_count == 0x0b) {
            ASSERT_MSG(payload[packet_count] == 0xc0001000,
                       "NOP hint is missing in CB setup sequence");
            last_cb_extent[col_buf_id].raw = payload[packet_count + 1];
        } else {
            last_cb_extent[col_buf_id].raw = 0;
        }
        break;
    }
    case ContextRegs::CbColor0Cmask:
    case ContextRegs::CbColor1Cmask:
    case ContextRegs::CbColor2Cmask:
    case ContextRegs::CbColor3Cmask:
    case ContextRegs::CbColor4Cmask:
    case ContextRegs::CbColor5Cmask:
    case ContextRegs::CbColor6Cmask:
    case ContextRegs::CbColor7Cmask: {
        const auto col_buf_id = (register_address - ContextRegs::CbColor0Cmask) /
                                (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
        ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

        if (packet_count == 0x04) {
            ASSERT_MSG(payload[packet_count] == 0xc0001000,
                       "NOP hint is missing in CB setup sequence");
            last_cb_extent[col_buf_id].raw = payload[packet_count + 1];
        }
        break;
    }
    case ContextRegs::DbZInfo: {
        if (packet_count == 8) {
            ASSERT_MSG(payload[20] == 0xc0001000, "NOP hint is missing in DB setup sequence");
            last_db_extent.raw = payload[21];
        } else {
            last_db_extent.raw = 0;
        }
        break;
    }
    default:
        break;
    }
}

namespace {

SHAD_NO_INLINE void WriteFenceMemory(Vulkan::Rasterizer* rasterizer, void* address, u64 data,
                                     u32 num_bytes) {
    PrepareGuestWrite(address, num_bytes);
    auto* memory = Core::Memory::Instance();
    if (!memory->TryWriteBacking(address, &data, num_bytes)) {
        memcpy(address, &data, num_bytes);
        if (rasterizer) {
            rasterizer->NotifyMemoryWrite(std::bit_cast<VAddr>(address), num_bytes,
                                          VideoCore::MemoryWriteSource::CommandProcessor);
        }
    }
}

[[nodiscard]] GuestSignalLabel SignalLabel(const PM4CmdEventWriteEos& packet) {
    return {.address = packet.Address<VAddr>(),
            .value = packet.DataDWord(),
            .num_bytes = sizeof(u32),
            .described = true,
            .value_known = true};
}

[[nodiscard]] GuestSignalLabel SignalLabel(DataSelect data_sel, VAddr address, u64 data) {
    switch (data_sel) {
    case DataSelect::None:
        return {.described = true};
    case DataSelect::Data32Low:
        return {.address = address,
                .value = static_cast<u32>(data),
                .num_bytes = sizeof(u32),
                .described = true,
                .value_known = true};
    case DataSelect::Data64:
        return {.address = address,
                .value = data,
                .num_bytes = sizeof(u64),
                .described = true,
                .value_known = true};
    default:
        // Clocks and counters are read when the signal runs.
        return {.address = address, .num_bytes = sizeof(u64), .described = true};
    }
}

/// The label of a signal that also waits for the GPU, which a wait cannot run ahead of.
[[nodiscard]] GuestSignalLabel AfterGpu(GuestSignalLabel label) {
    label.value_known = false;
    return label;
}

SHAD_NO_INLINE void SignalEventWriteEos(const PM4CmdEventWriteEos& packet,
                                        Vulkan::Rasterizer* rasterizer) {
    packet.SignalFence([rasterizer](void* address, u64 data, u32 num_bytes) {
        WriteFenceMemory(rasterizer, address, data, num_bytes);
    });
}

SHAD_NO_INLINE void SignalEventWriteEop(const PM4CmdEventWriteEop& packet,
                                        Vulkan::Rasterizer* rasterizer) {
    packet.SignalFence(
        [rasterizer](void* address, u64 data, u32 num_bytes) {
            WriteFenceMemory(rasterizer, address, data, num_bytes);
        },
        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
}

SHAD_NO_INLINE void SignalReleaseMem(const PM4CmdReleaseMem& packet, Vulkan::Rasterizer* rasterizer,
                                     u32 pipe_id) {
    packet.SignalFence(
        [pipe_id] {
            Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
        },
        [rasterizer](VAddr dst, u16 gds_index, u16 num_dwords) {
            rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
        });
    const auto data_sel = packet.data_sel.Value();
    if (rasterizer && data_sel != DataSelect::None && data_sel != DataSelect::GdsMemStore) {
        const u64 write_size = data_sel == DataSelect::Data32Low ? sizeof(u32) : sizeof(u64);
        rasterizer->NotifyMemoryWrite(packet.Address<VAddr>(), write_size,
                                      VideoCore::MemoryWriteSource::CommandProcessor);
    }
}

} // namespace

void Liverpool::RefreshPendingGpuCompletions() {
    if (pending_gpu_completion_count == 0) {
        return;
    }
    if (rasterizer && pending_gpu_completion_tick == rasterizer->CurrentTick()) {
        return;
    }
    pending_gpu_completion_tick = 0;
    pending_gpu_completion_count = 0;
    pending_gpu_fence_word_count = 0;
}

bool Liverpool::TrackDeferredGpuCompletion(u32 queue_id, VAddr address, u64 value,
                                            u32 num_bytes) {
    ASSERT(num_bytes == 0 || num_bytes == sizeof(u32) || num_bytes == sizeof(u64));
    RefreshPendingGpuCompletions();

    const u32 num_words = num_bytes / sizeof(u32);
    u32 new_words{};
    for (u32 word = 0; word < num_words; ++word) {
        const VAddr word_address = address + word * sizeof(u32);
        bool found{};
        for (u32 index = 0; index < pending_gpu_fence_word_count; ++index) {
            const auto& pending = pending_gpu_fence_words[index];
            if (pending.queue_id == queue_id && pending.address == word_address) {
                found = true;
                break;
            }
        }
        new_words += !found;
    }

    if (pending_gpu_fence_word_count + new_words > MaxPendingGpuFenceWords) [[unlikely]] {
        rasterizer->Flush();
        pending_gpu_completion_tick = 0;
        pending_gpu_completion_count = 0;
        pending_gpu_fence_word_count = 0;
        return false;
    }

    if (pending_gpu_completion_count == 0) {
        pending_gpu_completion_tick = rasterizer->CurrentTick();
    }
    ++pending_gpu_completion_count;

    for (u32 word = 0; word < num_words; ++word) {
        const PendingGpuFenceWord replacement{
            .address = address + word * sizeof(u32),
            .value = static_cast<u32>(value >> (word * 32)),
            .queue_id = static_cast<u8>(queue_id),
        };
        bool replaced{};
        for (u32 index = pending_gpu_fence_word_count; index != 0; --index) {
            auto& pending = pending_gpu_fence_words[index - 1];
            if (pending.queue_id == queue_id && pending.address == replacement.address) {
                pending = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            pending_gpu_fence_words[pending_gpu_fence_word_count++] = replacement;
        }
    }

    return true;
}

bool Liverpool::TryBypassGpuCompletionWait(u32 queue_id, VAddr address, u32 function, u32 mask,
                                            u32 reference) {
    RefreshPendingGpuCompletions();
    if (pending_gpu_completion_count == 0) {
        return false;
    }

    for (u32 index = pending_gpu_fence_word_count; index != 0; --index) {
        auto& pending = pending_gpu_fence_words[index - 1];
        if (pending.queue_id != queue_id || pending.address != address) {
            continue;
        }
        if (!TestWaitValue(pending.value, static_cast<PM4CmdWaitRegMem::Function>(function), mask,
                           reference)) {
            return false;
        }
        if (!pending.barriered) {
            rasterizer->GpuFenceWait();
            pending.barriered = true;
        }
        return true;
    }
    return false;
}

void Liverpool::SignalAfterGuestReads(u64 guest_copy_seq, Common::UniqueFunction<void>&& signal,
                                      const GuestSignalLabel& label) {
    if (guest_read_signals.empty() &&
        VideoCore::GuestCopyEngine::Instance().CompletedSeq() >= guest_copy_seq) {
        signal();
        return;
    }
    guest_read_signals.push_back({guest_copy_seq, std::move(signal), label});
}

void Liverpool::PollGuestReadSignals() {
    const u64 completed = VideoCore::GuestCopyEngine::Instance().CompletedSeq();
    while (!guest_read_signals.empty() && guest_read_signals.front().guest_copy_seq <= completed) {
        auto& front = guest_read_signals.front();
        skipped_signal_count -= front.skipped;
        auto signal = std::move(front.signal);
        guest_read_signals.pop_front();
        signal();
    }
}

void Liverpool::FlushGuestReadSignals() {
    while (!guest_read_signals.empty()) {
        CompleteGuestReads(guest_read_signals.front().guest_copy_seq);
        auto& front = guest_read_signals.front();
        skipped_signal_count -= front.skipped;
        auto signal = std::move(front.signal);
        guest_read_signals.pop_front();
        signal();
    }
}

bool Liverpool::SkipWaitForQueuedSignal(VAddr address, u32 function, u32 mask, u32 reference) {
    // The last queued signal to write the label decides what the wait reads once they land.
    for (auto it = guest_read_signals.rbegin(); it != guest_read_signals.rend(); ++it) {
        const GuestSignalLabel& label = it->label;
        if (!label.described) {
            return false;
        }
        if (label.num_bytes == 0 || address + sizeof(u32) <= label.address ||
            label.address + label.num_bytes <= address) {
            continue;
        }
        if (!label.value_known || address < label.address ||
            label.address + label.num_bytes < address + sizeof(u32)) {
            return false;
        }
        const u32 value = static_cast<u32>(label.value >> ((address - label.address) * 8));
        if (!TestWaitValue(value, static_cast<PM4CmdWaitRegMem::Function>(function), mask,
                           reference)) {
            return false;
        }
        // What the stream does next reaches the guest through later signals, which land after
        // this one, or through the writes OrderAfterSkippedSignals holds back.
        if (!it->skipped) {
            it->skipped = true;
            ++skipped_signal_count;
        }
        return true;
    }
    return false;
}

void Liverpool::FlushSkippedSignals() {
    FlushGuestReadSignals();
}

void Liverpool::DeferGpuCompletionInOrder(Common::UniqueFunction<void>&& callback,
                                          const GuestSignalLabel& label) {
    if (guest_read_signals.empty()) {
        rasterizer->DeferGpuCompletion(std::move(callback));
        return;
    }
    // Deferring once the earlier signals have run waits for the GPU work of this point of the
    // command stream, not for what gets recorded until then.
    const u64 gpu_tick = rasterizer->CurrentTick();
    auto* completion_rasterizer = rasterizer;
    guest_read_signals.push_back(
        {guest_read_signals.back().guest_copy_seq,
         [completion_rasterizer, gpu_tick, callback = std::move(callback)]() mutable {
             completion_rasterizer->DeferGpuCompletionAt(gpu_tick, std::move(callback));
         },
         AfterGpu(label)});
}

void Liverpool::FlushPendingGpuCompletionsForWait() {
    // A wait may be for a queued signal.
    FlushGuestReadSignals();
    RefreshPendingGpuCompletions();
    if (pending_gpu_completion_count == 0) {
        return;
    }
    rasterizer->Flush();
    pending_gpu_completion_tick = 0;
    pending_gpu_completion_count = 0;
    pending_gpu_fence_word_count = 0;
}

SHAD_NO_INLINE bool Liverpool::TryPromoteGoW3Eos(const PM4CmdEventWriteEos& packet) {
    auto& texture_cache = rasterizer->GetTextureCache();
    auto cand_opt = texture_cache.TakePendingFastpathCandidate();
    const bool is_sig_fence = (packet.command == PM4CmdEventWriteEos::Command::SignalFence);
    const bool is_val_1 = (packet.DataDWord() == 1);
    const VAddr label_addr = packet.Address<VAddr>();
    std::shared_ptr<VideoCore::GpuAuthorityShadow> authority_shadow;
    bool promoted{};
    if (cand_opt && is_sig_fence && is_val_1 && label_addr != 0) {
        promoted = texture_cache.PromotePendingDownloadAuthority(
            cand_opt->image_id, cand_opt->image_uid, cand_opt->resource_version,
            &authority_shadow);
    }

    if (promoted) {
        auto candidate = *cand_opt;

        auto& authority_tracker = VideoCore::GpuAuthorityTracker::Instance();
        const auto [authority_seq, virtual_fence_seq, label_generation] =
            authority_tracker.AllocateIds(label_addr);
        const u64 producer_tick = rasterizer->CurrentTick();

        VideoCore::GpuAuthorityEntry auth_entry{
            .authority_seq = authority_seq,
            .image_id = candidate.image_id.index,
            .image_uid = candidate.image_uid,
            .resource_version = candidate.resource_version,
            .guest_begin = candidate.guest_addr,
            .guest_end = candidate.guest_addr + candidate.download_size,
            .download_size = candidate.download_size,
            .producer_tick = producer_tick,
            .virtual_fence_seq = virtual_fence_seq,
            .label_addr = label_addr,
            .label_value = packet.DataDWord(),
            .label_generation = label_generation,
            .state = VideoCore::GpuAuthorityState::GpuAuthoritative,
            .shadow = std::move(authority_shadow),
        };

        VideoCore::VirtualGpuFence virt_fence{
            .virtual_fence_seq = virtual_fence_seq,
            .authority_seq = authority_seq,
            .label_addr = label_addr,
            .label_generation = label_generation,
            .expected_value = 1,
            .producer_tick = producer_tick,
            .gpu_complete = false,
            .host_label_written = false,
            .wait_consumed = false,
        };

        authority_tracker.RetireStaleAuthorities();
        authority_tracker.RegisterAuthority(auth_entry);
        authority_tracker.RegisterVirtualFence(virt_fence);

        texture_cache.PruneSupersededPendingDownloads(candidate.image_uid,
                                                      candidate.resource_version - 1);

        const u64 virt_seq = virtual_fence_seq;
        const u64 prod_tk = producer_tick;
        DeferGpuCompletionInOrder(
            [virt_seq, prod_tk] {
                VideoCore::GpuAuthorityTracker::Instance().SignalAsyncLabel(virt_seq, prod_tk);
            },
            SignalLabel(packet));
        return true;
    }
    return false;
}

SHAD_NO_INLINE void Liverpool::ProcessEventWriteEos(const PM4CmdEventWriteEos& packet) {
    const bool is_gow3 = (Common::ElfInfo::Instance().GameSerial() == "CUSA01715");
    if (is_gow3 && rasterizer && TryPromoteGoW3Eos(packet)) {
        return;
    }

    bool gpu_resident{};
    bool has_writebacks = false;
    if (rasterizer) {
        has_writebacks = rasterizer->ProcessDownloadImages(
            VideoCore::TextureCache::DownloadContext{
                .trigger = VideoCore::TextureCache::DownloadTrigger::EventWriteEos,
                .trigger_control = packet.event_control,
                .trigger_data_control = packet.cmd_info,
            },
            &gpu_resident);
    }
    const u64 guest_copy_seq = GuestCopySeq();
    if (has_writebacks && packet.command == PM4CmdEventWriteEos::Command::SignalFence) {
        auto* completion_rasterizer = rasterizer;
        DeferGpuCompletionInOrder(
            [packet, completion_rasterizer, guest_copy_seq] {
                CompleteGuestReads(guest_copy_seq);
                SignalEventWriteEos(packet, completion_rasterizer);
            },
            SignalLabel(packet));
        return;
    }
    if (packet.command == PM4CmdEventWriteEos::Command::SignalFence) {
        auto* completion_rasterizer = rasterizer;
        SignalAfterGuestReads(
            guest_copy_seq,
            [packet, completion_rasterizer] { SignalEventWriteEos(packet, completion_rasterizer); },
            SignalLabel(packet));
        return;
    }
    FlushGuestReadSignals();
    CompleteGuestReads(guest_copy_seq);
    SignalEventWriteEos(packet, rasterizer);
    if (packet.command == PM4CmdEventWriteEos::Command::GdsStore) {
        if (packet.size != 1) [[unlikely]] {
            GraphicsPacketAssertionFailed();
        }
        if (rasterizer) {
            rasterizer->Finish();
            const u32 value = rasterizer->ReadDataFromGds(packet.gds_index);
            *packet.Address() = value;
            rasterizer->NotifyMemoryWrite(std::bit_cast<VAddr>(packet.Address<void*>()),
                                          sizeof(value),
                                          VideoCore::MemoryWriteSource::CommandProcessor);
        }
    }
}

SHAD_NO_INLINE void Liverpool::ProcessEventWriteEop(const PM4CmdEventWriteEop& packet) {
    bool has_writebacks = false;
    if (rasterizer) {
        has_writebacks = rasterizer->ProcessDownloadImages(VideoCore::TextureCache::DownloadContext{
            .trigger = VideoCore::TextureCache::DownloadTrigger::EventWriteEop,
            .trigger_control = packet.event_control,
            .trigger_data_control = packet.data_control,
        });
    }
    const u64 guest_copy_seq = GuestCopySeq();
    if (has_writebacks) {
        auto* completion_rasterizer = rasterizer;
        DeferGpuCompletionInOrder(
            [packet, completion_rasterizer, guest_copy_seq] {
                CompleteGuestReads(guest_copy_seq);
                SignalEventWriteEop(packet, completion_rasterizer);
            },
            SignalLabel(packet.data_sel.Value(), reinterpret_cast<VAddr>(packet.Address<void>()),
                        packet.DataQWord()));
        return;
    }
    auto* completion_rasterizer = rasterizer;
    SignalAfterGuestReads(
        guest_copy_seq,
        [packet, completion_rasterizer] { SignalEventWriteEop(packet, completion_rasterizer); },
        SignalLabel(packet.data_sel.Value(), reinterpret_cast<VAddr>(packet.Address<void>()),
                    packet.DataQWord()));
}

SHAD_NO_INLINE void Liverpool::ProcessGraphicsEventWrite(const PM4Header* header) {
    const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
    LOG_TRACE(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
              magic_enum::enum_name(event->event_type.Value()),
              magic_enum::enum_name(event->event_index.Value()));
    if (event->event_index.Value() == EventIndex::ZpassDone &&
        event->event_type.Value() == EventType::PixelPipeStatDump) {
        if ((event->Address<u64>() & 0x8) == 0) {
            rasterizer->StartOcclusionQuery(event->Address<VAddr>(), num_counter_pairs);
        } else {
            rasterizer->EndOcclusionQuery(event->Address<VAddr>() & ~0xF, num_counter_pairs);
        }
    } else if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
        // TODO: handle proper synchronization, for now signal that update is done
        // immediately
        regs.cp_strmout_cntl.offset_update_done = 1;
    } else if (rasterizer) {
        rasterizer->FlushCaches(event->event_type.Value());
    }
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                            u32 ib_depth) {
    FIBER_ENTER(dcb_task_name);

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb, ib_depth);
        RESUME_GFX(ce_task);
    }
    const bool host_markers_enabled = rasterizer && EmulatorSettings.IsVkHostMarkersEnabled();
    const bool guest_markers_enabled = rasterizer && EmulatorSettings.IsVkGuestMarkersEnabled();

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!dcb.empty()) {
        if (num_commands.load(std::memory_order_acquire) != 0) [[unlikely]] {
            ProcessCommands();
        }
        if (!guest_read_signals.empty()) [[unlikely]] {
            PollGuestReadSignals();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 header_raw = header->raw;
        const u32 type = header_raw >> 30;

        if (type != 3) [[unlikely]] {
            dcb = NextNonType3Packet(dcb, type);
            continue;
        }

        {
            const u32 count = ((header_raw >> 16) + 1) & 0x3fff;
            u32 packet_words = count + 1;
            const auto opcode = static_cast<PM4ItOpcode>((header_raw >> 8) & 0xff);
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                const u32 payload = nop->data_block[0];
                if ((payload & 0xffff0000u) != 0x68750000u) [[likely]] {
                    break;
                }

                switch (payload) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    CompleteGuestReads();
                    FlushGuestReadSignals();
                    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                ++graphics_pipeline_generation;
                ++graphics_state_generation;
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                [[maybe_unused]] const bool changed =
                    WriteGraphicsRegisters(reg_addr, payload, count - 1);
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                const u32 word_count = count - 1;
                [[maybe_unused]] bool changed;
                if (word_count == 1) [[likely]] {
                    changed = WriteGraphicsRegistersSmall<1>(reg_addr, payload);
                } else if (word_count == 2) {
                    changed = WriteGraphicsRegistersSmall<2>(reg_addr, payload);
                } else if (word_count == 8) {
                    changed = WriteGraphicsRegisters8(reg_addr, payload);
                } else if (reg_addr > Regs::NumRegs ||
                           word_count > Regs::NumRegs - reg_addr) [[unlikely]] {
                    changed = InvalidGraphicsRegisterRange(reg_addr, word_count);
                } else {
                    changed = WriteGraphicsRegistersSlow(reg_addr, payload, word_count);
                }

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                constexpr u32 ColorHintSpan =
                    ContextRegs::CbColor7Cmask - ContextRegs::CbColor0Base;
                if (reg_addr - ContextRegs::CbColor0Base <= ColorHintSpan ||
                    reg_addr == ContextRegs::DbZInfo) [[unlikely]] {
                    HandleContextRegisterHint(reg_addr, header->type3.count, payload);
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                const u32 word_count = count - 1;
                [[maybe_unused]] bool changed{};

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    WriteComputeProgramRegisters(mapped_queues[GfxQueueId].cs_state,
                                                 set_data->reg_offset, payload, word_count);
                } else {
                    const u32 reg_addr = Regs::ShRegWordOffset + set_data->reg_offset;
                    if (word_count == 2) [[likely]] {
                        changed = WriteGraphicsRegistersSmall<2>(reg_addr, payload);
                    } else if (word_count == 1) {
                        changed = WriteGraphicsRegistersSmall<1>(reg_addr, payload);
                    } else if (word_count == 4) {
                        changed = WriteGraphicsRegisters4(reg_addr, payload);
                    } else if (word_count == 8) {
                        changed = WriteGraphicsRegisters8(reg_addr, payload);
                    } else if (reg_addr > Regs::NumRegs ||
                               word_count > Regs::NumRegs - reg_addr) [[unlikely]] {
                        changed = InvalidGraphicsRegisterRange(reg_addr, word_count);
                    } else {
                        changed = WriteGraphicsRegistersSlow(reg_addr, payload, word_count);
                    }
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                [[maybe_unused]] const bool changed =
                    WriteGraphicsRegisters(Regs::UconfigRegWordOffset + set_data->reg_offset,
                                           reinterpret_cast<const u32*>(header + 2), count - 1);
                break;
            }
            case PM4ItOpcode::SetPredication: {
                if (!warned_set_predication) {
                    warned_set_predication = true;
                    WarnSetPredication();
                }
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndex2");
                        rasterizer->Draw(true);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(true);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndexOffset2");
                        rasterizer->Draw(true, draw_index_off->index_offset);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(true, draw_index_off->index_offset);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndexAuto");
                        rasterizer->Draw(false);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(false);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndirect");
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndirectMulti");
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndexIndirect");
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DrawIndexIndirectMulti");
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address,
                                        "DrawIndexIndirectCountMulti");
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count,
                                                 draw_index_indirect->count_indirect_enable.Value()
                                                     ? draw_index_indirect->count_addr
                                                     : 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count,
                                                 draw_index_indirect->count_indirect_enable.Value()
                                                     ? draw_index_indirect->count_addr
                                                     : 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DispatchDirect");
                        rasterizer->DispatchDirect();
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DispatchDirect();
                    }
                }
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) [[unlikely]] {
                        BeginHostMarker(*rasterizer, cmd_address, "DispatchIndirect");
                        rasterizer->DispatchIndirect(indirect_args_addr, offset, size);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DispatchIndirect(indirect_args_addr, offset, size);
                    }
                }
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                if (set_base->base_index != PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable)
                    [[unlikely]] {
                    GraphicsPacketAssertionFailed();
                }
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                ProcessGraphicsEventWrite(header);
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                ProcessEventWriteEos(*event_eos);
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                ProcessEventWriteEop(*event_eop);
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                                           dma_data->data, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                           dma_data->NumBytes(), true, false);
                } else if (dma_data->src_sel == DmaDataSrc::Data &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                           dma_data->data, false);
                } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                           dma_data->NumBytes(), false, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                           dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                                           false, false);
                } else {
                    InvalidDmaData(*dma_data);
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                if (write_data->dst_sel.Value() != 2 && write_data->dst_sel.Value() != 5)
                    [[unlikely]] {
                    GraphicsPacketAssertionFailed();
                }
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                if (!write_data->wr_one_addr.Value()) {
                    OrderAfterSkippedSignals();
                    PrepareGuestWrite(address, data_size);
                    std::memcpy(address, write_data->data, data_size);
                    if (rasterizer) {
                        rasterizer->NotifyMemoryWrite(
                            std::bit_cast<VAddr>(address), data_size,
                            VideoCore::MemoryWriteSource::CommandProcessor);
                    }
                } else {
                    UnsupportedWriteDataAddressMode();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                WarnCopyData(*copy_data);
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                OrderAfterSkippedSignals();
                PrepareGuestWrite(mem_semaphore->Address<VAddr>(), sizeof(u64));
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    if (!mem_semaphore->Signaled()) {
                        FlushPendingGpuCompletionsForWait();
                    }
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                if (rasterizer) {
                    rasterizer->NotifyMemoryWrite(
                        mem_semaphore->Address<VAddr>(), sizeof(u64),
                        VideoCore::MemoryWriteSource::CommandProcessor);
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                const auto* acquire_mem = reinterpret_cast<const PM4CmdAcquireMem*>(header);
                const VAddr base_address =
                    (static_cast<VAddr>(acquire_mem->cp_coher_base_lo) |
                     (static_cast<VAddr>(acquire_mem->cp_coher_base_hi) << 32)) << 8;
                const u64 size = (static_cast<u64>(acquire_mem->cp_coher_size_lo) |
                                  (static_cast<u64>(acquire_mem->cp_coher_size_hi) << 32))
                                 << 8;
                if (rasterizer) {
                    rasterizer->AcquireMemory(acquire_mem->cp_coher_cntl, base_address, size);
                }
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                if (!rewind->Valid()) {
                    FlushPendingGpuCompletionsForWait();
                }
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                const auto function = wait_reg_mem->function.Value();
                const u32 mask = wait_reg_mem->mask;
                const u32 reference = wait_reg_mem->ref;
                const auto test_value = [function, mask, reference](u32 value) {
                    return TestWaitValue(value, function, mask, reference);
                };
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                if (wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory) {
                    const u32* poll_address = wait_reg_mem->Address<const u32*>();
                    const VAddr wait_addr = reinterpret_cast<VAddr>(poll_address);
                    auto virt_fence = VideoCore::GpuAuthorityTracker::Instance().MatchVirtualWait(
                        wait_addr, reference, mask, static_cast<u32>(function));
                    if (virt_fence != nullptr) {
                        const u64 cur_tk = rasterizer ? rasterizer->CurrentTick() : 0;
                        const bool needs_progress_submit =
                            rasterizer && virt_fence->producer_tick >= cur_tk;
                        if (needs_progress_submit) {
                            rasterizer->Flush();
                        }
                        break;
                    } else {
                        bool already_satisfied = test_value(*poll_address);
                        if (!already_satisfied && !guest_read_signals.empty()) {
                            // The wait may be for a queued signal. When its value passes, the
                            // stream runs on while the copies before it finish; otherwise
                            // waiting for them here is cheaper than submitting.
                            if (SkipWaitForQueuedSignal(wait_addr, static_cast<u32>(function),
                                                        mask, reference)) {
                                already_satisfied = true;
                            } else {
                                FlushGuestReadSignals();
                                already_satisfied = test_value(*poll_address);
                            }
                        }
                        const bool gpu_fence_bypass =
                            !already_satisfied &&
                            TryBypassGpuCompletionWait(GfxQueueId,
                                                       reinterpret_cast<VAddr>(poll_address),
                                                       static_cast<u32>(function), mask, reference);
                        if (!already_satisfied && !gpu_fence_bypass) {
                            FlushPendingGpuCompletionsForWait();
                            if (rasterizer) {
                                rasterizer->Flush();
                            }
                        }
                        if (!already_satisfied && !gpu_fence_bypass &&
                            vo_port->IsVoLabel(reinterpret_cast<const u64*>(poll_address)) &&
                            num_submits == mapped_queues[GfxQueueId].submits.size()) {
                            vo_port->WaitVoLabel([&] { return test_value(*poll_address); });
                        } else if (!already_satisfied && !gpu_fence_bypass) {
                            WAIT_MEMORY(GfxQueueId, poll_address, test_value(*poll_address), YIELD_GFX());
                        }
                    }
                } else {
                    const u32 register_index = wait_reg_mem->Reg();
                    if (!test_value(regs.reg_array[register_index])) {
                        FlushPendingGpuCompletionsForWait();
                    }
                    while (!test_value(regs.reg_array[register_index])) {
                        YIELD_GFX();
                    }
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {},
                    ib_depth + 1);
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                if (rasterizer) {
                    rasterizer->CpSync();
                }
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                WarnStrmoutBufferUpdate(*strmout);
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                WarnGetLodStats();
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    WarnReservedCondExec();
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    packet_words += cond_exec->exec_count.Value();
                }
                break;
            }
            default:
                UnknownType3Opcode(opcode, count);
            }
            dcb = NextPacket(dcb, packet_words);
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    FIBER_EXIT;
}

SHAD_NO_INLINE void Liverpool::ProcessComputeReleaseMem(const PM4CmdReleaseMem& release_packet,
                                                        const u32* queue_pipe_id) {
    const auto* release_mem = &release_packet;
    const auto data_sel = release_mem->data_sel.Value();
    bool has_writebacks = false;
    if (rasterizer) {
        has_writebacks = rasterizer->ProcessDownloadImages(VideoCore::TextureCache::DownloadContext{
            .trigger = VideoCore::TextureCache::DownloadTrigger::ReleaseMem,
            .trigger_control = release_mem->dw1,
            .trigger_data_control = release_mem->dw2,
        });
    }
    const u64 guest_copy_seq = GuestCopySeq();
    if (has_writebacks && data_sel != DataSelect::GdsMemStore) {
        const PM4CmdReleaseMem packet = *release_mem;
        auto* completion_rasterizer = rasterizer;
        const u32 pipe_id = *queue_pipe_id;
        DeferGpuCompletionInOrder(
            [packet, completion_rasterizer, pipe_id, guest_copy_seq] {
                CompleteGuestReads(guest_copy_seq);
                SignalReleaseMem(packet, completion_rasterizer, pipe_id);
            },
            SignalLabel(data_sel, packet.Address<VAddr>(), packet.DataQWord()));
        rasterizer->Flush();
    } else if (data_sel != DataSelect::GdsMemStore) {
        const PM4CmdReleaseMem packet = *release_mem;
        auto* completion_rasterizer = rasterizer;
        const u32 pipe_id = *queue_pipe_id;
        SignalAfterGuestReads(
            guest_copy_seq,
            [packet, completion_rasterizer, pipe_id] {
                SignalReleaseMem(packet, completion_rasterizer, pipe_id);
            },
            SignalLabel(data_sel, packet.Address<VAddr>(), packet.DataQWord()));
    } else {
        // Storing GDS records a copy, which has to stay at this point of the command stream.
        FlushGuestReadSignals();
        CompleteGuestReads(guest_copy_seq);
        SignalReleaseMem(*release_mem, rasterizer, *queue_pipe_id);
    }
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid, u32 ib_depth) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];
    const bool host_markers_enabled = rasterizer && EmulatorSettings.IsVkHostMarkersEnabled();

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        if (num_commands.load(std::memory_order_acquire) != 0) [[unlikely]] {
            ProcessCommands();
        }
        if (!guest_read_signals.empty()) [[unlikely]] {
            PollGuestReadSignals();
        }

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                PrepareGuestWrite(acb.data(), acb.size_bytes());
                PrepareGuestWrite(queue.read_addr, sizeof(u32));
                *queue.read_addr += acb.size();
                *queue.read_addr &= queue.ring_size_dw - 1;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                PrepareGuestWrite(queue.read_addr, sizeof(u32));
                *queue.read_addr += next_dw_off;
                *queue.read_addr &= queue.ring_size_dw - 1;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        switch (opcode) {
        case PM4ItOpcode::Nop: {
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid,
                ib_depth + 1);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}",
                                u32(dma_data->src_sel.Value()), u32(dma_data->dst_sel.Value()));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            if (!rewind->Valid()) {
                FlushPendingGpuCompletionsForWait();
            }
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto* payload = reinterpret_cast<const u32*>(header + 2);
            const u32 word_count = header->type3.NumWords() - 1;
            [[maybe_unused]] bool changed{};

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                WriteComputeProgramRegisters(mapped_queues[vqid + 1].cs_state, set_data->reg_offset,
                                             payload, word_count);
            } else {
                changed = WriteGraphicsRegisters(Regs::ShRegWordOffset + set_data->reg_offset,
                                                 payload, word_count);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                const auto cmd_address = reinterpret_cast<const void*>(header);
                if (host_markers_enabled) [[unlikely]] {
                    rasterizer->ScopeMarkerBegin(
                        fmt::format("asc[{}]:{}:DispatchDirect", vqid, cmd_address));
                    rasterizer->DispatchDirect();
                    rasterizer->ScopeMarkerEnd();
                } else {
                    rasterizer->DispatchDirect();
                }
            }
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) [[unlikely]] {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                const auto cmd_address = reinterpret_cast<const void*>(header);
                if (host_markers_enabled) [[unlikely]] {
                    rasterizer->ScopeMarkerBegin(
                        fmt::format("asc[{}]:{}:DispatchIndirect", vqid, cmd_address));
                    rasterizer->DispatchIndirect(ib_address, 0, size);
                    rasterizer->ScopeMarkerEnd();
                } else {
                    rasterizer->DispatchIndirect(ib_address, 0, size);
                }
            }
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                OrderAfterSkippedSignals();
                PrepareGuestWrite(write_data->Address<void*>(), data_size);
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
                if (rasterizer) {
                    rasterizer->NotifyMemoryWrite(
                        std::bit_cast<VAddr>(write_data->Address<void*>()), data_size,
                        VideoCore::MemoryWriteSource::CommandProcessor);
                }
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            OrderAfterSkippedSignals();
            PrepareGuestWrite(mem_semaphore->Address<VAddr>(), sizeof(u64));
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                if (!mem_semaphore->Signaled()) {
                    FlushPendingGpuCompletionsForWait();
                }
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            if (rasterizer) {
                rasterizer->NotifyMemoryWrite(
                    mem_semaphore->Address<VAddr>(), sizeof(u64),
                    VideoCore::MemoryWriteSource::CommandProcessor);
            }
            break;
        }
            case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            const auto function = wait_reg_mem->function.Value();
            const u32 mask = wait_reg_mem->mask;
            const u32 reference = wait_reg_mem->ref;
            const auto test_value = [function, mask, reference](u32 value) {
                return TestWaitValue(value, function, mask, reference);
            };
            if (wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory) {
                const u32* poll_address = wait_reg_mem->Address<const u32*>();
                bool already_satisfied = test_value(*poll_address);
                if (!already_satisfied && !guest_read_signals.empty()) {
                    // The wait may be for a queued signal. When its value passes, the stream
                    // runs on while the copies before it finish; otherwise waiting for them
                    // here is cheaper than submitting.
                    if (SkipWaitForQueuedSignal(reinterpret_cast<VAddr>(poll_address),
                                                static_cast<u32>(function), mask, reference)) {
                        already_satisfied = true;
                    } else {
                        FlushGuestReadSignals();
                        already_satisfied = test_value(*poll_address);
                    }
                }
                const bool gpu_fence_bypass =
                    !already_satisfied &&
                    TryBypassGpuCompletionWait(vqid + 1, reinterpret_cast<VAddr>(poll_address),
                                               static_cast<u32>(function), mask, reference);
                if (!already_satisfied && !gpu_fence_bypass) {
                    FlushPendingGpuCompletionsForWait();
                    if (rasterizer) {
                        rasterizer->Flush();
                    }
                    WAIT_MEMORY(vqid + 1, poll_address, test_value(*poll_address),
                                YIELD_ASC(vqid));
                }
            } else {
                const u32 register_index = wait_reg_mem->Reg();
                if (!test_value(regs.reg_array[register_index])) {
                    FlushPendingGpuCompletionsForWait();
                }
                while (!test_value(regs.reg_array[register_index])) {
                    YIELD_ASC(vqid);
                }
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            ProcessComputeReleaseMem(*reinterpret_cast<const PM4CmdReleaseMem*>(header),
                                     &queue.pipe_id);
            break;
        }
        case PM4ItOpcode::EventWrite: {
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        if constexpr (!is_indirect) {
            PrepareGuestWrite(acb.data(), static_cast<u64>(next_dw_off) * sizeof(u32));
            PrepareGuestWrite(queue.read_addr, sizeof(u32));
        }
        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr &= queue.ring_size_dw - 1;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    auto task = ProcessGraphics(dcb, ccb);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    constexpr u64 QueueBit = 1ULL << GfxQueueId;
    if ((blocked_queue_mask.load(std::memory_order_acquire) & QueueBit) == 0) {
        ready_queue_mask.fetch_or(QueueBit, std::memory_order_release);
    }
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    const u64 queue_bit = 1ULL << gnm_vqid;
    if ((blocked_queue_mask.load(std::memory_order_acquire) & queue_bit) == 0) {
        ready_queue_mask.fetch_or(queue_bit, std::memory_order_release);
    }
    submit_cv.notify_one();
}

} // namespace AmdGpu
