// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/wave64_lowering.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {
namespace {

struct SharedAccess {
    bool read;
    bool write;
    bool atomic;

    explicit operator bool() const {
        return read || write;
    }
};

SharedAccess GetSharedAccess(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::LoadSharedU64:
        return {.read = true};
    case IR::Opcode::WriteSharedU16:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::WriteSharedU64:
        return {.write = true};
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicIAdd64:
    case IR::Opcode::SharedAtomicISub32:
    case IR::Opcode::SharedAtomicISub64:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicSMin64:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicUMin64:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicSMax64:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicUMax64:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicInc64:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicDec64:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicAnd64:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicOr64:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicXor64:
        return {.read = true, .write = true, .atomic = true};
    default:
        return {};
    }
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block) {
    SharedAccess previous_access{};
    for (IR::Inst& inst : block->Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::Barrier) {
            previous_access = {};
            continue;
        }
        const SharedAccess current_access = GetSharedAccess(inst);
        if (!current_access) {
            continue;
        }

        const bool atomic_sequence = previous_access.atomic && current_access.atomic;
        const bool needs_barrier =
            !atomic_sequence && ((previous_access.write && current_access.read) ||
                                 (previous_access.read && current_access.write));
        if (needs_barrier) {
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            ir.Barrier();
        }
        previous_access = current_access;
    }
    if (previous_access) {
        IR::IREmitter ir{*block, --block->end()};
        ir.Barrier();
    }
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EnterDivergentConditional(IR::Block* merge, u32& divergence_depth) {
    if (divergence_depth == 0) {
        auto insert_point = std::ranges::find_if_not(merge->Instructions(), IR::IsPhi);
        IR::IREmitter ir{*merge, insert_point};
        ir.Barrier();
    }
    ++divergence_depth;
}

} // namespace

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.stage != Stage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.cs_info;
    const u64 shared_memory_size =
        u64{cs_info.shared_memory_size} + program.info.shared_memory_scratch_size;
    const u64 threadgroup_size =
        u64{cs_info.workgroup_size[0]} * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup.
    const bool requires_wave_exchange_barriers = program.info.shared_memory_scratch_size != 0;
    if (shared_memory_size == 0 || threadgroup_size != Wave64::GuestWaveSize ||
        (!profile.needs_lds_barriers && !requires_wave_exchange_barriers)) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    u32 divergence_depth{};
    std::vector<ConditionalScope> conditionals;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = Wave64::IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            if (divergent) {
                EnterDivergentConditional(node.data.if_node.merge, divergence_depth);
            }
            break;
        }
        case Type::EndIf:
            if (conditionals.empty() || conditionals.back().merge != node.data.end_if.merge) {
                return;
            }
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Block:
            // Workgroup barriers are invalid in blocks that not every invocation reaches.
            if (divergence_depth == 0) {
                EmitBarrierInBlock(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty()) {
        return;
    }
}

} // namespace Shader::Optimization
