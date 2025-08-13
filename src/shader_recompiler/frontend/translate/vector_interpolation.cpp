// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/frontend/translate/translate.h"

namespace Shader::Gcn {

void Translator::EmitVectorInterpolation(const GcnInst& inst) {
    switch (inst.opcode) {
        // VINTRP
    case Opcode::V_INTERP_P1_F32:
        return V_INTERP_P1_F32(inst);
    case Opcode::V_INTERP_P2_F32:
        return V_INTERP_P2_F32(inst);
    case Opcode::V_INTERP_MOV_F32:
        return V_INTERP_MOV_F32(inst);
    default:
        LogMissingOpcode(inst);
    }
}

// VINTRP
void Translator::V_INTERP_P1_F32(const GcnInst& inst) {
    // VDST = P10 * VSRC + P0
    const u32 attr_index = inst.control.vintrp.attr;
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
    const IR::F32 p1 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 1);
    const IR::F32 i = GetSrc<IR::F32>(inst.src[0]);
    const IR::F32 result = ir.FPFma(ir.FPSub(p1, p0), i, p0);
    SetDst(inst.dst[0], result);
}

void Translator::V_INTERP_P2_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    const auto& attr = runtime_info.fs_info.inputs.at(attr_index);
    info.interp_qualifiers[attr_index] = vgpr_to_interp[inst.src[0].code];
    const IR::Attribute attrib{IR::Attribute::Param0 + attr_index};
    SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));
}

void Translator::V_INTERP_MOV_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    const auto& attr = runtime_info.fs_info.inputs.at(attr_index);
    const IR::Attribute attrib{IR::Attribute::Param0 + attr_index};
    SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));
}

} // namespace Shader::Gcn
