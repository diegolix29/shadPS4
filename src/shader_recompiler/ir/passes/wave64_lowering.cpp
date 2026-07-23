// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/ir/passes/wave64_lowering.h"

#include <limits>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization::Wave64 {
namespace {

bool IsSingleGuestWaveWorkgroup(const RuntimeInfo& runtime_info) {
    u32 thread_count = 1;
    for (const u32 dimension : runtime_info.cs_info.workgroup_size) {
        if (dimension == 0 || dimension > GuestWaveSize / thread_count) {
            return false;
        }
        thread_count *= dimension;
    }
    return thread_count == GuestWaveSize;
}

} // namespace

bool IsDivergentCondition(const IR::U1& condition) {
    if (condition.IsImmediate()) {
        return false;
    }
    const IR::Inst* const condition_inst = condition.InstRecursive();
    return IR::BreadthFirstSearch(condition_inst,
                                  [](const IR::Inst* inst) -> std::optional<bool> {
                                      switch (inst->GetOpcode()) {
                                      case IR::Opcode::GuestLaneId:
                                      case IR::Opcode::LaneId:
                                          return true;
                                      case IR::Opcode::GetAttributeU32:
                                          if (inst->Arg(0).Attribute() ==
                                              IR::Attribute::LocalInvocationId) {
                                              return true;
                                          }
                                          break;
                                      default:
                                          break;
                                      }
                                      return std::nullopt;
                                  })
        .value_or(false);
}

std::vector<IR::Block*> FindConvergedBlocks(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;

    struct ConditionalScope {
        const IR::Block* merge;
        bool divergent;
    };

    std::vector<IR::Block*> blocks;
    std::vector<ConditionalScope> conditionals;
    std::vector<const IR::Block*> loops;
    u32 divergence_depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If: {
            const bool divergent = IsDivergentCondition(node.data.if_node.cond);
            conditionals.push_back({node.data.if_node.merge, divergent});
            divergence_depth += static_cast<u32>(divergent);
            break;
        }
        case Type::EndIf:
            if (conditionals.empty() || conditionals.back().merge != node.data.end_if.merge) {
                return {};
            }
            divergence_depth -= static_cast<u32>(conditionals.back().divergent);
            conditionals.pop_back();
            break;
        case Type::Loop:
            loops.push_back(node.data.loop.merge);
            break;
        case Type::Repeat:
            if (loops.empty() || loops.back() != node.data.repeat.merge) {
                return {};
            }
            loops.pop_back();
            break;
        case Type::Block:
            if (divergence_depth == 0 && loops.empty()) {
                blocks.push_back(node.data.block);
            }
            break;
        default:
            break;
        }
    }
    if (!conditionals.empty() || !loops.empty()) {
        return {};
    }
    return blocks;
}

std::optional<LoweringContext> PrepareLowering(const IR::Program& program,
                                               const RuntimeInfo& runtime_info,
                                               const Profile& profile) {
    if (program.info.stage != Stage::Compute || !IsSingleGuestWaveWorkgroup(runtime_info)) {
        return std::nullopt;
    }

    const u32 guest_size = runtime_info.cs_info.shared_memory_size;
    constexpr u64 alignment_mask = ScratchElementSize - 1;
    const u64 scratch_base = (u64{guest_size} + alignment_mask) & ~alignment_mask;
    const u64 total_size = scratch_base + ScratchSize;
    if (total_size > std::numeric_limits<u32>::max() ||
        total_size > profile.max_shared_memory_size) {
        return std::nullopt;
    }

    const u32 required_scratch = static_cast<u32>(total_size - guest_size);
    if (program.info.shared_memory_scratch_size != 0 &&
        program.info.shared_memory_scratch_size != required_scratch) {
        return std::nullopt;
    }
    return LoweringContext{
        .scratch_base = static_cast<u32>(scratch_base),
        .required_scratch = required_scratch,
    };
}

IR::U32 LocalInvocationIndex(IR::IREmitter& ir, const RuntimeInfo& runtime_info) {
    const auto& size = runtime_info.cs_info.workgroup_size;
    const IR::U32 x = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U32 y = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 1);
    const IR::U32 z = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 2);
    const IR::U32 xy = ir.IAdd(x, ir.IMul(y, ir.Imm32(size[0])));
    return ir.IAdd(xy, ir.IMul(z, ir.Imm32(size[0] * size[1])));
}

} // namespace Shader::Optimization::Wave64
