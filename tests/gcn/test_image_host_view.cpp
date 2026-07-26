// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <spirv/unified1/spirv.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_view.h"

namespace Shader::Optimization {
void PatchImageArgs(IR::Block& block, IR::Inst& inst, Info& info);
}

namespace {

AmdGpu::Image MakeCapturedSharp() {
    AmdGpu::Image image{};
    image.data_format = u64(AmdGpu::DataFormat::Format8_8_8_8);
    image.num_format = u64(AmdGpu::NumberFormat::Unorm);
    image.width = 1023;
    image.height = 0;
    image.depth = 0;
    image.pitch = 1023;
    image.last_level = 0;
    image.tiling_index = u64(AmdGpu::TileMode::Thin2DThin);
    image.type = u64(AmdGpu::ImageType::Color1D);
    return image;
}

Shader::ImageResource MakeSampledResource() {
    return Shader::ImageResource{
        .sharp_idx = 0,
        .is_depth = false,
        .is_atomic = false,
        .is_array = false,
        .is_written = false,
        .is_r128 = false,
    };
}

void ExpectGuestView(const Shader::ImageResource& resource, const AmdGpu::Image& image) {
    EXPECT_FALSE(resource.Is1DHostedAs2D(image));
    EXPECT_EQ(resource.GetHostViewType(image), image.GetViewType(resource.is_array));
}

struct SpirvInstruction {
    spv::Op opcode;
    std::vector<u32> words;
};

std::vector<SpirvInstruction> DecodeSpirv(const std::vector<u32>& spirv) {
    std::vector<SpirvInstruction> instructions;
    for (size_t offset = 5; offset < spirv.size();) {
        const u32 word_count = spirv[offset] >> 16;
        EXPECT_GT(word_count, 0u);
        if (word_count == 0 || offset + word_count > spirv.size()) {
            break;
        }
        instructions.push_back({
            .opcode = static_cast<spv::Op>(spirv[offset] & 0xFFFFu),
            .words = {spirv.begin() + offset, spirv.begin() + offset + word_count},
        });
        offset += word_count;
    }
    return instructions;
}

std::vector<u32> EmitHostedQuerySpirv() {
    Shader::Info info{};
    info.stage = Shader::Stage::Compute;
    info.l_stage = Shader::LogicalStage::Compute;
    info.flattened_ud_buf.resize(sizeof(AmdGpu::Image) / sizeof(u32));
    const auto image = MakeCapturedSharp();
    std::memcpy(info.flattened_ud_buf.data(), &image, sizeof(image));
    info.images.push_back(MakeSampledResource());
    info.samplers.push_back({
        .inline_sampler = {},
        .is_inline_sampler = true,
        .associated_image = 0,
        .disable_aniso = false,
    });

    Shader::IR::Program program{info};
    Shader::Pools pools{};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = Shader::IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = Shader::IR::AbstractSyntaxNode::Type::Return;
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front());

    Shader::IR::IREmitter ir{*block};
    const auto dynamic_x =
        Shader::IR::F32{ir.ConvertUToF(32, 32, ir.GetUserData(Shader::IR::ScalarReg::S0))};
    const auto guest_coords = ir.CompositeConstruct(dynamic_x, ir.Imm32(0.0f));
    Shader::IR::TextureInstInfo texture_info{};
    const auto query_lod = ir.ImageQueryLod(ir.Imm32(0u), guest_coords, texture_info);
    auto* query_lod_inst = query_lod.InstRecursive();
    Shader::Optimization::PatchImageArgs(*block, *query_lod_inst, program.info);
    [[maybe_unused]] const auto dimensions =
        ir.ImageQueryDimension(ir.Imm32(0u), ir.Imm32(0u), ir.Imm1(false), texture_info);

    const auto derivative_x =
        Shader::IR::F32{ir.ConvertUToF(32, 32, ir.GetUserData(Shader::IR::ScalarReg::S1))};
    const auto derivative_y =
        Shader::IR::F32{ir.ConvertUToF(32, 32, ir.GetUserData(Shader::IR::ScalarReg::S2))};
    const auto zero = ir.Imm32(0.0f);
    const auto address1 =
        ir.CompositeConstruct(ir.Imm32(std::bit_cast<f32>(0x3Fu)), derivative_x, derivative_y,
                              dynamic_x);
    const auto address2 = ir.CompositeConstruct(zero, zero, zero, zero);
    const auto address3 = ir.CompositeConstruct(zero, zero, zero, zero);
    Shader::IR::TextureInstInfo sample_info{};
    sample_info.has_offset.Assign(true);
    sample_info.has_derivatives.Assign(true);
    const auto raw_sample = ir.ImageSampleRaw(ir.Imm32(0u), ir.Imm32(0u), address1, address2,
                                              address3, zero, sample_info);
    Shader::Optimization::PatchImageArgs(*block, *raw_sample.InstRecursive(), program.info);
    Shader::Optimization::IdentityRemovalPass(program.blocks);

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 32;
    Shader::RuntimeInfo runtime_info{};
    runtime_info.Initialize(Shader::Stage::Compute);
    runtime_info.num_user_data = 3;
    runtime_info.cs_info.workgroup_size = {1, 1, 1};
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

TEST(ImageHostViewTest, TranslatesHostedOneDimensionalOperationsWithTwoDimensionalOperands) {
    const auto instructions = DecodeSpirv(EmitHostedQuerySpirv());
    std::unordered_map<u32, const SpirvInstruction*> definitions;
    for (const auto& inst : instructions) {
        switch (inst.opcode) {
        case spv::Op::OpTypeFloat:
        case spv::Op::OpTypeInt:
        case spv::Op::OpTypeVector:
        case spv::Op::OpTypeImage:
            definitions.emplace(inst.words[1], &inst);
            break;
        case spv::Op::OpConstant:
        case spv::Op::OpConstantComposite:
        case spv::Op::OpCompositeConstruct:
        case spv::Op::OpCompositeExtract:
        case spv::Op::OpImageQuerySizeLod:
        case spv::Op::OpImageQueryLod:
            definitions.emplace(inst.words[2], &inst);
            break;
        default:
            break;
        }
    }

    const SpirvInstruction* image_type = nullptr;
    const SpirvInstruction* query_lod = nullptr;
    const SpirvInstruction* query_size = nullptr;
    const SpirvInstruction* sample = nullptr;
    for (const auto& inst : instructions) {
        if (inst.opcode == spv::Op::OpTypeImage) {
            image_type = &inst;
        } else if (inst.opcode == spv::Op::OpImageQueryLod) {
            query_lod = &inst;
        } else if (inst.opcode == spv::Op::OpImageQuerySizeLod) {
            query_size = &inst;
        } else if (inst.opcode == spv::Op::OpImageSampleExplicitLod) {
            sample = &inst;
        }
    }

    ASSERT_NE(image_type, nullptr);
    ASSERT_GE(image_type->words.size(), 4u);
    EXPECT_EQ(image_type->words[3], u32(spv::Dim::Dim2D));

    ASSERT_NE(query_lod, nullptr);
    ASSERT_GE(query_lod->words.size(), 5u);
    const auto* coords = definitions.at(query_lod->words[4]);
    ASSERT_EQ(coords->opcode, spv::Op::OpCompositeConstruct);
    ASSERT_EQ(coords->words.size(), 5u);
    const auto* coord_type = definitions.at(coords->words[1]);
    ASSERT_EQ(coord_type->opcode, spv::Op::OpTypeVector);
    EXPECT_EQ(coord_type->words[3], 2u);
    const auto* y = definitions.at(coords->words[4]);
    ASSERT_EQ(y->opcode, spv::Op::OpConstant);
    EXPECT_EQ(std::bit_cast<f32>(y->words[3]), Shader::ImageResource::Hosted2DSampleY);

    ASSERT_NE(query_size, nullptr);
    const auto* size_type = definitions.at(query_size->words[1]);
    ASSERT_EQ(size_type->opcode, spv::Op::OpTypeVector);
    EXPECT_EQ(size_type->words[3], 2u);
    const auto extract = std::ranges::find_if(instructions, [&](const auto& inst) {
        return inst.opcode == spv::Op::OpCompositeExtract && inst.words.size() == 5 &&
               inst.words[3] == query_size->words[2] && inst.words[4] == 0;
    });
    EXPECT_NE(extract, instructions.end());

    ASSERT_NE(sample, nullptr);
    ASSERT_EQ(sample->words.size(), 9u);
    EXPECT_NE(sample->words[5] & u32(spv::ImageOperandsGradMask), 0u);
    EXPECT_NE(sample->words[5] & u32(spv::ImageOperandsConstOffsetMask), 0u);

    const auto* sample_coords = definitions.at(sample->words[4]);
    ASSERT_EQ(sample_coords->opcode, spv::Op::OpCompositeConstruct);
    ASSERT_EQ(sample_coords->words.size(), 5u);
    const auto* sample_y = definitions.at(sample_coords->words[4]);
    ASSERT_EQ(sample_y->opcode, spv::Op::OpConstant);
    EXPECT_EQ(std::bit_cast<f32>(sample_y->words[3]), Shader::ImageResource::Hosted2DSampleY);

    for (const u32 gradient_id : {sample->words[6], sample->words[7]}) {
        const auto* gradient = definitions.at(gradient_id);
        ASSERT_EQ(gradient->opcode, spv::Op::OpCompositeConstruct);
        ASSERT_EQ(gradient->words.size(), 5u);
        const auto* gradient_y = definitions.at(gradient->words[4]);
        ASSERT_EQ(gradient_y->opcode, spv::Op::OpConstant);
        EXPECT_EQ(std::bit_cast<f32>(gradient_y->words[3]), 0.0f);
    }

    const auto* offset = definitions.at(sample->words[8]);
    ASSERT_EQ(offset->opcode, spv::Op::OpConstantComposite);
    ASSERT_EQ(offset->words.size(), 5u);
    const auto* offset_y = definitions.at(offset->words[4]);
    ASSERT_EQ(offset_y->opcode, spv::Op::OpConstant);
    EXPECT_EQ(offset_y->words[3], 0u);
}

TEST(ImageHostViewTest, HostsCapturedOneDimensionalSharpAsTwoDimensional) {
    const auto image = MakeCapturedSharp();
    const auto resource = MakeSampledResource();

    EXPECT_TRUE(resource.Is1DHostedAs2D(image));
    EXPECT_EQ(resource.GetHostViewType(image), AmdGpu::ImageType::Color2D);
}

TEST(ImageHostViewTest, HostedAndNativeTwoDimensionalSpecializationsAreDistinct) {
    Shader::ImageSpecialization hosted{};
    Shader::ImageSpecialization native{};
    hosted.type = AmdGpu::ImageType::Color2D;
    native.type = AmdGpu::ImageType::Color2D;
    hosted.is_1d_hosted_as_2d = true;

    EXPECT_NE(hosted, native);
}

TEST(ImageHostViewTest, UsesRowCenterForSamplingAndZeroForIntegerOperands) {
    EXPECT_FLOAT_EQ(Shader::ImageResource::Hosted2DSampleY, 0.5f);
    EXPECT_EQ(Shader::ImageResource::Hosted2DIntegerY, 0u);
}

TEST(ImageHostViewTest, SampledAndStorageBackingsCannotShareIncompatibleViews) {
    const auto image = MakeCapturedSharp();
    const auto sampled = MakeSampledResource();
    auto storage = sampled;
    storage.is_written = true;

    EXPECT_FALSE(VideoCore::IsViewTypeCompatible(storage.GetHostViewType(image),
                                                 sampled.GetHostViewType(image)));
    EXPECT_FALSE(VideoCore::IsViewTypeCompatible(sampled.GetHostViewType(image),
                                                 storage.GetHostViewType(image)));
    EXPECT_TRUE(VideoCore::NeedsViewTypeRecreation(storage.GetHostViewType(image),
                                                   sampled.GetHostViewType(image)));
    EXPECT_TRUE(VideoCore::NeedsViewTypeRecreation(sampled.GetHostViewType(image),
                                                   storage.GetHostViewType(image)));
    EXPECT_FALSE(VideoCore::NeedsViewTypeRecreation(AmdGpu::ImageType::Color2D,
                                                    AmdGpu::ImageType::Color2D));
}

TEST(ImageHostViewTest, RejectsTwoDimensionalGuestType) {
    auto image = MakeCapturedSharp();
    image.type = u64(AmdGpu::ImageType::Color2D);

    ExpectGuestView(MakeSampledResource(), image);
}

TEST(ImageHostViewTest, RejectsOneDimensionalArrayGuestType) {
    auto image = MakeCapturedSharp();
    image.type = u64(AmdGpu::ImageType::Color1DArray);

    ExpectGuestView(MakeSampledResource(), image);
}

TEST(ImageHostViewTest, RejectsHeightGreaterThanOne) {
    auto image = MakeCapturedSharp();
    image.height = 1;

    ExpectGuestView(MakeSampledResource(), image);
}

TEST(ImageHostViewTest, RejectsMultipleLayers) {
    auto image = MakeCapturedSharp();
    image.depth = 1;

    ExpectGuestView(MakeSampledResource(), image);
}

TEST(ImageHostViewTest, RejectsMultipleMipLevels) {
    auto image = MakeCapturedSharp();
    image.last_level = 1;

    ExpectGuestView(MakeSampledResource(), image);
}

TEST(ImageHostViewTest, RejectsOneDimensionalTiling) {
    auto image = MakeCapturedSharp();
    image.tiling_index = u64(AmdGpu::TileMode::Thin1DThin);

    ExpectGuestView(MakeSampledResource(), image);
}

TEST(ImageHostViewTest, RejectsArrayResource) {
    auto resource = MakeSampledResource();
    resource.is_array = true;

    ExpectGuestView(resource, MakeCapturedSharp());
}

TEST(ImageHostViewTest, RejectsWrittenResource) {
    auto resource = MakeSampledResource();
    resource.is_written = true;

    ExpectGuestView(resource, MakeCapturedSharp());
}

TEST(ImageHostViewTest, RejectsDepthResource) {
    auto resource = MakeSampledResource();
    resource.is_depth = true;

    ExpectGuestView(resource, MakeCapturedSharp());
}

TEST(ImageHostViewTest, RejectsAtomicResource) {
    auto resource = MakeSampledResource();
    resource.is_atomic = true;

    ExpectGuestView(resource, MakeCapturedSharp());
}

TEST(ImageHostViewTest, RejectsR128Resource) {
    auto resource = MakeSampledResource();
    resource.is_r128 = true;

    ExpectGuestView(resource, MakeCapturedSharp());
}

} // namespace
