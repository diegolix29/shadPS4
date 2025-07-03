// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/frontend/copy_shader.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/ir/attribute.h"

namespace Shader {

CopyShaderData ParseCopyShader(std::span<const u32> code) {
    Gcn::GcnCodeSlice code_slice{code.data(), code.data() + code.size()};
    Gcn::GcnDecodeContext decoder;

    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    ASSERT_MSG(code[0] == token_mov_vcchi, "First instruction is not s_mov_b32 vcc_hi, #imm");

    std::array<s32, 32> offsets{};
    offsets.fill(-1);

    std::array<s32, 256> sources{};
    sources.fill(-1);

    CopyShaderData data{};
    auto last_attr{IR::Attribute::Position0};
    while (!code_slice.atEnd()) {
        auto inst = decoder.decodeInstruction(code_slice);
        switch (inst.opcode) {
        case Gcn::Opcode::S_MOVK_I32: {
            sources[inst.dst[0].code] = inst.control.sopk.simm;
            break;
        }
        case Gcn::Opcode::S_MOV_B32: {
            sources[inst.dst[0].code] = inst.src[0].code;
            break;
        }
        case Gcn::Opcode::S_ADDK_I32: {
            sources[inst.dst[0].code] += inst.control.sopk.simm;
            break;
        }
        case Gcn::Opcode::EXP: {
            const auto& exp = inst.control.exp;
            const IR::Attribute semantic = static_cast<IR::Attribute>(exp.target);

            fmt::print(stderr, "[ParseCopyShader] EXP semantic={} src_count={}\n", int(semantic),
                       inst.src_count);
            for (int i = 0; i < inst.src_count; ++i) {
                const auto ofs = offsets[inst.src[i].code];
                fmt::print(stderr, "  EXP src {} reg={} -> offset={}\n", i, inst.src[i].code, ofs);
                if (ofs != -1) {
                    data.attr_map[ofs] = {semantic, i};
                    if (semantic > last_attr) {
                        last_attr = semantic;
                    }
                } else {
                    fmt::print(stderr,
                               "[ParseCopyShader] WARNING: EXP src reg={} had no known offset "
                               "(semantic {}). Shader will likely break.\n",
                               inst.src[i].code, int(semantic));
                }
            }

            break;
        }

case Gcn::Opcode::BUFFER_LOAD_DWORD: {
            s32 base = inst.control.mubuf.offset;

            if (inst.src[3].field != Gcn::OperandField::ConstZero) {
                const u32 index = inst.src[3].code;
                ASSERT(sources[index] != -1);
                base += sources[index];
                fmt::print(stderr,
                           "[ParseCopyShader] BUFFER_LOAD_DWORD: Added src3 sources[{}]={}\n",
                           index, sources[index]);
            }

            if (inst.src[2].field != Gcn::OperandField::ConstZero) {
                fmt::print(
                    stderr,
                    "[ParseCopyShader] WARNING: Ignoring dynamic indexing via src[2] reg {}.\n",
                    inst.src[2].code);
            }

            offsets[inst.src[1].code] = base;

            fmt::print(stderr,
                       "[ParseCopyShader] BUFFER_LOAD_DWORD:\n"
                       "  dst_reg={} base_reg={} final offset={}\n",
                       inst.dst[0].code, inst.src[1].code, base);

            break;
        }
        default:
            break;
        }
    }

    if (last_attr != IR::Attribute::Position0) {
        data.num_attrs = static_cast<u32>(last_attr) - static_cast<u32>(IR::Attribute::Param0) + 1;
        const auto it = data.attr_map.begin();
        const u32 comp_stride = std::next(it)->first - it->first;
        data.output_vertices = comp_stride / 64;
    }

    return data;
}

} // namespace Shader
