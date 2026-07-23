// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/ir/passes/ir_passes.h"

#include <vector>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/wave64_lowering.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

// Native ballot bits use host subgroup-local IDs, whose ordering is not required to match the
// guest's local invocation indices. Reconstruct a PS4 wave64 ballot through workgroup memory so the
// result is independent of host subgroup size and ordering. Only converged ballots are lowered
// because every invocation must reach the workgroup barriers in the same order.
u32 LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                          const Profile& profile) {
    const auto context = Wave64::PrepareLowering(program, runtime_info, profile);
    if (!context) {
        return 0;
    }

    struct Ballot {
        IR::Block* block;
        IR::Inst* inst;
    };
    u32 num_lowered{};
    while (true) {
        std::vector<Ballot> ballots;
        for (IR::Block* block : Wave64::FindConvergedBlocks(program)) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::Ballot) {
                    ballots.push_back({block, &inst});
                }
            }
        }
        if (ballots.empty()) {
            break;
        }

        for (const auto& [block, ballot] : ballots) {
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*ballot)};
            const IR::U32 invocation_index = Wave64::LocalInvocationIndex(ir, runtime_info);
            const IR::U32 invocation_offset =
                ir.IMul(invocation_index, ir.Imm32(Wave64::ScratchElementSize));
            ir.WriteShared(32, ir.Imm32(0),
                           ir.IAdd(ir.Imm32(context->scratch_base), invocation_offset));
            ir.Barrier();

            const IR::U1 high_half = ir.IGreaterThanEqual(invocation_index, ir.Imm32(32), false);
            const IR::U32 word_address{ir.Select(
                high_half, ir.Imm32(context->scratch_base + static_cast<u32>(sizeof(u32))),
                ir.Imm32(context->scratch_base))};
            const IR::U32 bit_index = ir.BitwiseAnd(invocation_index, ir.Imm32(31));
            const IR::U32 bit{ir.ShiftLeftLogical(ir.Imm32(1), bit_index)};
            const IR::U32 contribution{ir.Select(IR::U1{ballot->Arg(0)}, bit, ir.Imm32(0))};
            ir.Reference(ir.SharedAtomicOr(word_address, contribution, false));
            ir.Barrier();

            const IR::U32 lo{ir.LoadShared(32, false, ir.Imm32(context->scratch_base))};
            const IR::U32 hi{ir.LoadShared(
                32, false, ir.Imm32(context->scratch_base + static_cast<u32>(sizeof(u32))))};
            const IR::Value result = ir.CompositeConstruct(lo, hi, ir.Imm32(0), ir.Imm32(0));
            ballot->ReplaceUsesWithAndRemove(result);
        }
        num_lowered += static_cast<u32>(ballots.size());
    }

    if (num_lowered == 0) {
        return 0;
    }
    program.info.shared_memory_scratch_size = context->required_scratch;
    return num_lowered;
}

} // namespace Shader::Optimization
