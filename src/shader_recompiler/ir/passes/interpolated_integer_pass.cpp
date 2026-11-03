// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

static bool IsTruncation(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::ConvertU32F32:
    case IR::Opcode::ConvertS32F32:
    case IR::Opcode::FPTrunc32:
    case IR::Opcode::FPFloor32:
        return true;
    default:
        return false;
    }
}

static bool IsInterpolatedParam(const IR::Value& value) {
    const IR::Inst* inst = value.TryInst();
    return inst && inst->GetOpcode() == IR::Opcode::GetAttribute &&
           IR::IsParam(inst->Arg(0).Attribute());
}

void InterpolatedIntegerPass(IR::Program& program) {
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTruncation(inst.GetOpcode()) || !IsInterpolatedParam(inst.Arg(0))) {
                continue;
            }
            // Hardware without exact interpolation of equal vertex values can produce e.g.
            // 0.99999994 instead of 1.0; scaling away from zero restores the integer.
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::F32 value{inst.Arg(0)};
            inst.SetArg(0, ir.FPMul(value, ir.Imm32(1.0f + 1.0f / 65536.0f)));
        }
    }
}

} // namespace Shader::Optimization
