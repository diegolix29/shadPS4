// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"

namespace AmdGpu {

// =========================================================================
// GPU Command List
// =========================================================================
// Intermediary between PM4 parsing and Vulkan command recording.
//
// PM4 parsing emits lightweight command records into this list.
// A flush pass drains the list and dispatches to the rasterizer.
//
// Invariant: the command list is always flushed before any pipeline-state
// register write (SetContextReg / SetShReg / SetConfigReg / SetUconfigReg
// that actually changed data) and before any synchronization point.
// Therefore every command in the list shares the same pipeline state in
// `regs` at execution time.  Per-draw parameters that vary within a batch
// (num_indices, index_base, etc.) are snapshotted into the command record.
// =========================================================================

enum class GpuCmdType : u8 {
    Draw,
    DrawIndirect,
    Dispatch,
    DispatchIndirect,
};

// Per-draw snapshot — fields that can vary between consecutive draws
// without a pipeline-state change (set by draw packets, NumInstances,
// IndexType, IndexBase, etc.).
struct GpuDrawCmd {
    u32 num_indices;
    u32 num_instances;
    u32 max_index_size;
    u32 draw_initiator;
    u32 index_base_lo;
    u32 index_base_hi;
    u32 index_type_raw;    // regs.index_buffer_type.raw
    u32 index_offset;
    bool is_indexed;
};

struct GpuDrawIndirectCmd {
    VAddr arg_address;
    VAddr count_address;
    u32 offset;
    u32 stride;
    u32 max_count;
    bool is_indexed;
};

struct GpuDispatchCmd {
    u32 dim_x;
    u32 dim_y;
    u32 dim_z;
    u32 dispatch_initiator;
};

struct GpuDispatchIndirectCmd {
    VAddr arg_address;
    u32 offset;
    u32 size;
    u32 dispatch_initiator;
};

struct GpuCommand {
    GpuCmdType type;
    uintptr_t cmd_address; // Original PM4 packet address (for debug markers)

    union {
        GpuDrawCmd draw;
        GpuDrawIndirectCmd draw_indirect;
        GpuDispatchCmd dispatch;
        GpuDispatchIndirectCmd dispatch_indirect;
    };
};
static_assert(sizeof(GpuCommand) <= 64, "GpuCommand should fit in a cache line");

class GpuCommandList {
public:
    static constexpr u32 MaxCommands = 512;

    void PushDraw(const GpuDrawCmd& draw, uintptr_t addr) noexcept {
        if (count_ >= MaxCommands) [[unlikely]]
            return;
        auto& cmd = commands_[count_++];
        cmd.type = GpuCmdType::Draw;
        cmd.cmd_address = addr;
        cmd.draw = draw;
    }

    void PushDrawIndirect(const GpuDrawIndirectCmd& draw, uintptr_t addr) noexcept {
        if (count_ >= MaxCommands) [[unlikely]]
            return;
        auto& cmd = commands_[count_++];
        cmd.type = GpuCmdType::DrawIndirect;
        cmd.cmd_address = addr;
        cmd.draw_indirect = draw;
    }

    void PushDispatch(const GpuDispatchCmd& dispatch, uintptr_t addr) noexcept {
        if (count_ >= MaxCommands) [[unlikely]]
            return;
        auto& cmd = commands_[count_++];
        cmd.type = GpuCmdType::Dispatch;
        cmd.cmd_address = addr;
        cmd.dispatch = dispatch;
    }

    void PushDispatchIndirect(const GpuDispatchIndirectCmd& dispatch, uintptr_t addr) noexcept {
        if (count_ >= MaxCommands) [[unlikely]]
            return;
        auto& cmd = commands_[count_++];
        cmd.type = GpuCmdType::DispatchIndirect;
        cmd.cmd_address = addr;
        cmd.dispatch_indirect = dispatch;
    }

    [[nodiscard]] u32 Count() const noexcept {
        return count_;
    }
    [[nodiscard]] bool Empty() const noexcept {
        return count_ == 0;
    }
    [[nodiscard]] const GpuCommand& operator[](u32 i) const noexcept {
        return commands_[i];
    }
    void Clear() noexcept {
        count_ = 0;
    }

private:
    u32 count_{0};
    std::array<GpuCommand, MaxCommands> commands_;
};

} // namespace AmdGpu
