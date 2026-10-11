// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
void MemoryBarrier(EmitContext& ctx, spv::Scope scope) {
    const auto semantics{
        spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::UniformMemory |
        spv::MemorySemanticsMask::WorkgroupMemory | spv::MemorySemanticsMask::AtomicCounterMemory |
        spv::MemorySemanticsMask::ImageMemory};
    ctx.OpMemoryBarrier(ctx.ConstU32(static_cast<u32>(scope)),
                        ctx.ConstU32(static_cast<u32>(semantics)));
}
} // Anonymous namespace

void EmitBarrier(EmitContext& ctx) {
    const auto execution{spv::Scope::Workgroup};
    spv::Scope memory;
    spv::MemorySemanticsMask memory_semantics;
    if (ctx.sw_stage == Shader::SwStage::TessellationControl) {
        memory = spv::Scope::Invocation;
        memory_semantics = spv::MemorySemanticsMask::MaskNone;
    } else {
        memory = spv::Scope::Workgroup;
        memory_semantics =
            spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::WorkgroupMemory;
    }
    ctx.OpControlBarrier(ctx.ConstU32(static_cast<u32>(execution)),
                         ctx.ConstU32(static_cast<u32>(memory)),
                         ctx.ConstU32(static_cast<u32>(memory_semantics)));
}

void EmitWorkgroupMemoryBarrier(EmitContext& ctx) {
    // Keeps GCN lockstep LDS steps in order (inserted by SharedMemoryBarrierPass). A plain
    // memory barrier only orders accesses of one invocation; hosts that run a subgroup as
    // several hardware passes (e.g. SIMD32 on SIMD16 Intel Xe2) may still schedule one half's
    // loads before the other half's stores. A subgroup-scope control barrier syncs the lanes.
    // Only shared memory semantics: flushing uniform/image memory on every step is expensive.
    const auto semantics{spv::MemorySemanticsMask::AcquireRelease |
                         spv::MemorySemanticsMask::WorkgroupMemory};
    static const bool memory_only = [] {
        const char* env = std::getenv("SHADPS4_LDS_ORDER");
        return env && env[0] == '1';
    }();
    if (memory_only) {
        ctx.OpMemoryBarrier(ctx.ConstU32(static_cast<u32>(spv::Scope::Workgroup)),
                            ctx.ConstU32(static_cast<u32>(semantics)));
        return;
    }
    ctx.OpControlBarrier(ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup)),
                         ctx.ConstU32(static_cast<u32>(spv::Scope::Workgroup)),
                         ctx.ConstU32(static_cast<u32>(semantics)));
}

void EmitDeviceMemoryBarrier(EmitContext& ctx) {
    MemoryBarrier(ctx, spv::Scope::Device);
}

} // namespace Shader::Backend::SPIRV
