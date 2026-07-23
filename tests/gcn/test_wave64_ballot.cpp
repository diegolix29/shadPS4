// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>

#include <gtest/gtest.h>

#include "common/object_pool.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {
namespace {

class Wave64BallotTest : public testing::Test {
protected:
    Wave64BallotTest() : entry{inst_pool}, body{inst_pool}, merge{inst_pool}, program{info} {
        program.blocks = {&entry};
        program.syntax_list.push_back(
            {.data{.block = &entry}, .type = IR::AbstractSyntaxNode::Type::Block});

        info.stage = Stage::Compute;
        info.l_stage = LogicalStage::Compute;
        runtime_info.Initialize(Stage::Compute);
        runtime_info.cs_info.workgroup_size = {8, 8, 1};
        runtime_info.cs_info.shared_memory_size = 512;
        profile.subgroup_size = 32;
        profile.max_shared_memory_size = 64_KB;
        profile.needs_lds_barriers = true;
    }

    IR::Inst* AddBallot(IR::Block& block) {
        IR::IREmitter ir{block};
        const IR::U32 invocation = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        const IR::Value mask = ir.Ballot(ir.INotEqual(invocation, ir.Imm32(0)));
        ir.Reference(mask);
        return mask.Inst();
    }

    IR::U1 AddLocalInvocationCondition(IR::Block& block) {
        IR::IREmitter ir{block};
        const IR::U32 invocation = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        return ir.IEqual(invocation, ir.Imm32(0));
    }

    IR::U1 AddGuestLaneIdCondition(IR::Block& block) {
        IR::IREmitter ir{block};
        return ir.IEqual(ir.GuestLaneId(), ir.Imm32(0));
    }

    IR::U1 AddBallotCondition(IR::Block& block) {
        IR::IREmitter ir{block};
        const IR::U32 invocation = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        const IR::Value mask = ir.Ballot(ir.INotEqual(invocation, ir.Imm32(0)));
        const IR::U32 low_mask{ir.CompositeExtract(mask, 0)};
        return ir.INotEqual(low_mask, ir.Imm32(0));
    }

    void AddIfSyntax(const IR::U1& condition) {
        program.syntax_list.clear();
        program.syntax_list.push_back(
            {.data{.block = &entry}, .type = IR::AbstractSyntaxNode::Type::Block});
        IR::AbstractSyntaxNode if_node{};
        if_node.type = IR::AbstractSyntaxNode::Type::If;
        if_node.data.if_node = {.cond = condition, .body = &body, .merge = &merge};
        program.syntax_list.push_back(if_node);
        program.syntax_list.push_back(
            {.data{.block = &body}, .type = IR::AbstractSyntaxNode::Type::Block});
        IR::AbstractSyntaxNode end_if{};
        end_if.type = IR::AbstractSyntaxNode::Type::EndIf;
        end_if.data.end_if = {.merge = &merge};
        program.syntax_list.push_back(end_if);
        program.syntax_list.push_back(
            {.data{.block = &merge}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.blocks = {&entry, &body, &merge};
    }

    size_t CountOpcode(IR::Opcode opcode) const {
        size_t count{};
        for (const IR::Block* block : program.blocks) {
            count += std::ranges::count_if(block->Instructions(), [opcode](const IR::Inst& inst) {
                return inst.GetOpcode() == opcode;
            });
        }
        return count;
    }

    Info info{};
    RuntimeInfo runtime_info{};
    Profile profile{};
    Common::ObjectPool<IR::Inst> inst_pool{64};
    IR::Block entry;
    IR::Block body;
    IR::Block merge;
    IR::Program program;
};

TEST_F(Wave64BallotTest, LowersConvergedBallotThroughSharedScratch) {
    AddBallot(entry);

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 1);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::WriteSharedU32), 1);
    EXPECT_EQ(CountOpcode(IR::Opcode::SharedAtomicOr32), 1);
    EXPECT_EQ(CountOpcode(IR::Opcode::LoadSharedU32), 2);
    EXPECT_EQ(CountOpcode(IR::Opcode::CompositeConstructU32x4), 1);
    EXPECT_EQ(CountOpcode(IR::Opcode::Barrier), 2);
    EXPECT_EQ(info.shared_memory_scratch_size, 256);

    SharedMemoryBarrierPass(program, runtime_info, profile);
    EXPECT_EQ(CountOpcode(IR::Opcode::Barrier), 3);
}

TEST_F(Wave64BallotTest, ReusesCompatibleWave64Scratch) {
    info.shared_memory_scratch_size = 256;
    AddBallot(entry);

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 1);
    EXPECT_EQ(info.shared_memory_scratch_size, 256);
}

TEST_F(Wave64BallotTest, LowersNativeWave64BallotToPreserveGuestLaneOrder) {
    AddBallot(entry);
    profile.subgroup_size = 64;

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 1);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::SharedAtomicOr32), 1);
    EXPECT_EQ(info.shared_memory_scratch_size, 256);
}

TEST_F(Wave64BallotTest, PreservesNonWave64Workgroup) {
    AddBallot(entry);
    runtime_info.cs_info.workgroup_size = {32, 1, 1};

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 1);
}

TEST_F(Wave64BallotTest, PreservesBallotInsideDivergentConditional) {
    AddIfSyntax(AddLocalInvocationCondition(entry));
    AddBallot(body);

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 1);
}

TEST_F(Wave64BallotTest, PreservesBallotInsideGuestLaneIdConditional) {
    AddIfSyntax(AddGuestLaneIdCondition(entry));
    AddBallot(body);

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 1);
}

TEST_F(Wave64BallotTest, LowersAfterDivergentConditionalReconverges) {
    AddIfSyntax(AddLocalInvocationCondition(entry));
    AddBallot(merge);

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 1);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 0);
}

TEST_F(Wave64BallotTest, RecomputesConvergenceAfterLoweringUniformReduction) {
    AddIfSyntax(AddBallotCondition(entry));
    AddBallot(body);

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 2);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 0);
}

TEST_F(Wave64BallotTest, PreservesBallotWhenScratchWouldExceedHostLimit) {
    AddBallot(entry);
    profile.max_shared_memory_size = 640;

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 1);
    EXPECT_EQ(info.shared_memory_scratch_size, 0);
}

TEST_F(Wave64BallotTest, RejectsScratchAddressOverflow) {
    AddBallot(entry);
    runtime_info.cs_info.shared_memory_size = std::numeric_limits<u32>::max() - 1;
    profile.max_shared_memory_size = std::numeric_limits<u32>::max();

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 1);
    EXPECT_EQ(info.shared_memory_scratch_size, 0);
}

TEST_F(Wave64BallotTest, PreservesBallotWithIncompatibleScratchAllocation) {
    AddBallot(entry);
    info.shared_memory_scratch_size = 128;

    EXPECT_EQ(LowerWave64BallotPass(program, runtime_info, profile), 0);
    EXPECT_EQ(CountOpcode(IR::Opcode::Ballot), 1);
    EXPECT_EQ(info.shared_memory_scratch_size, 128);
}

TEST_F(Wave64BallotTest, SharedAtomicParticipatesInBarrierAnalysis) {
    IR::IREmitter ir{entry};
    ir.Reference(ir.SharedAtomicOr(ir.Imm32(0), ir.Imm32(1), false));
    ir.Reference(ir.LoadShared(32, false, ir.Imm32(0)));

    SharedMemoryBarrierPass(program, runtime_info, profile);

    EXPECT_EQ(CountOpcode(IR::Opcode::Barrier), 2);
}

TEST_F(Wave64BallotTest, ConsecutiveSharedAtomicsDoNotNeedControlBarrier) {
    IR::IREmitter ir{entry};
    ir.Reference(ir.SharedAtomicOr(ir.Imm32(0), ir.Imm32(1), false));
    ir.Reference(ir.SharedAtomicOr(ir.Imm32(0), ir.Imm32(2), false));

    SharedMemoryBarrierPass(program, runtime_info, profile);

    EXPECT_EQ(CountOpcode(IR::Opcode::Barrier), 1);
}

TEST_F(Wave64BallotTest, ExplicitBarrierResetsSharedAccessTracking) {
    IR::IREmitter ir{entry};
    ir.WriteShared(32, ir.Imm32(1), ir.Imm32(0));
    ir.Barrier();
    ir.Reference(ir.LoadShared(32, false, ir.Imm32(0)));

    SharedMemoryBarrierPass(program, runtime_info, profile);

    EXPECT_EQ(CountOpcode(IR::Opcode::Barrier), 2);
}

TEST_F(Wave64BallotTest, AvoidsBlockBarrierInsideGuestLaneIdConditional) {
    AddIfSyntax(AddGuestLaneIdCondition(entry));
    IR::IREmitter ir{body};
    ir.WriteShared(32, ir.Imm32(1), ir.Imm32(0));
    ir.Reference(ir.LoadShared(32, false, ir.Imm32(0)));

    SharedMemoryBarrierPass(program, runtime_info, profile);

    EXPECT_EQ(CountOpcode(IR::Opcode::Barrier), 1);
}

} // namespace
} // namespace Shader::Optimization
