// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/ir/ir_emitter.h"

namespace Shader {
struct Profile;
struct RuntimeInfo;
} // namespace Shader

namespace Shader::IR {
struct Program;
} // namespace Shader::IR

namespace Shader::Optimization::Wave64 {

constexpr u32 GuestWaveSize = 64;
constexpr u32 ScratchElementSize = sizeof(u32);
constexpr u32 ScratchSize = GuestWaveSize * ScratchElementSize;

struct LoweringContext {
    u32 scratch_base;
    u32 required_scratch;
};

[[nodiscard]] bool IsDivergentCondition(const IR::U1& condition);
[[nodiscard]] std::vector<IR::Block*> FindConvergedBlocks(const IR::Program& program);
[[nodiscard]] std::optional<LoweringContext> PrepareLowering(const IR::Program& program,
                                                             const RuntimeInfo& runtime_info,
                                                             const Profile& profile);
[[nodiscard]] IR::U32 LocalInvocationIndex(IR::IREmitter& ir, const RuntimeInfo& runtime_info);

} // namespace Shader::Optimization::Wave64
