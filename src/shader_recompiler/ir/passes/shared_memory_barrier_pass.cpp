// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        ir.Barrier();
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

static bool IsDivergent(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth) {
    const IR::U1 cond = data.if_node.cond;
    if (IsDivergent(cond)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list) {
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (IsDivergent(node.data.repeat.cond)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (IsDivergent(node.data.break_node.cond)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// Inserts shared-memory-only memory barriers (no execution sync) between shared memory writes
// and reads. Unlike control barriers they are valid in divergent control flow. GCN code runs
// lockstep LDS algorithms (scans, reductions) inside a wave without barriers, relying on the
// hardware executing each LDS instruction for all lanes in order. Host compilers may move an
// invocation's loads above its stores to other addresses, so neighbours read stale values.
static void EmitMemoryBarrierInBlock(IR::Block* block) {
    enum class Last : u32 { None, Read, Write };
    Last last{};
    for (IR::Inst& inst : block->Instructions()) {
        const bool is_read = IsLoadShared(inst);
        const bool is_write = IsWriteShared(inst);
        if (!is_read && !is_write) {
            continue;
        }
        if ((is_read && last == Last::Write) || (is_write && last == Last::Read)) {
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            ir.WorkgroupMemoryBarrier();
        }
        last = is_write ? Last::Write : Last::Read;
    }
    if (last != Last::None) {
        IR::IREmitter ir{*block, --block->end()};
        ir.WorkgroupMemoryBarrier();
    }
}

// Blocks inside control flow that depends on the invocation id (divergent ifs and loops).
// Lockstep LDS code that relies on wave execution order lives there, because GCN code at the top
// level synchronizes with s_barrier when it needs to.
static NodeSet CollectDivergentBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    NodeSet result;
    std::vector<std::pair<const IR::Block*, bool>> ifs;
    std::vector<std::pair<const IR::Block*, bool>> loops;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list);
    u32 depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergent(node.data.if_node.cond);
            ifs.emplace_back(node.data.if_node.merge, divergent);
            depth += divergent ? 1 : 0;
            break;
        }
        case Type::EndIf:
            if (!ifs.empty() && ifs.back().first == node.data.end_if.merge) {
                depth -= ifs.back().second ? 1 : 0;
                ifs.pop_back();
            }
            break;
        case Type::Loop: {
            const bool divergent = divergent_loops.contains(node.data.loop.merge);
            loops.emplace_back(node.data.loop.merge, divergent);
            depth += divergent ? 1 : 0;
            break;
        }
        case Type::Repeat:
            if (!loops.empty() && loops.back().first == node.data.repeat.merge) {
                depth -= loops.back().second ? 1 : 0;
                loops.pop_back();
            }
            break;
        case Type::Block:
            if (depth > 0) {
                result.insert(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    return result;
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // SHADPS4_LDS_ORDER: 0 = off, 1 = memory barrier, 2 = subgroup barrier (default).
    static const bool lds_order_enabled = [] {
        const char* env = std::getenv("SHADPS4_LDS_ORDER");
        return !(env && env[0] == '0');
    }();
    if (lds_order_enabled && shared_memory_size != 0 && threadgroup_size > 1) {
        const NodeSet divergent_blocks = CollectDivergentBlocks(program);
        for (IR::Block* block : program.blocks) {
            if (divergent_blocks.contains(block)) {
                EmitMemoryBarrierInBlock(block);
            }
        }
    }
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup.
    if (shared_memory_size == 0 || threadgroup_size != GcnSubgroupSize ||
        !profile.needs_lds_barriers) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block);
        }
    }
}

} // namespace Shader::Optimization
